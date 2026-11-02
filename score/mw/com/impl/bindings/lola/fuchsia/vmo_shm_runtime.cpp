/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_shm_runtime.h"

#include "score/memory/shared/lock_provider/lock_provider.h"
#include "score/memory/shared/shared_memory_factory.h"

#include <lib/component/incoming/cpp/protocol.h>

#include <cerrno>
#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

class VmoShmRuntime::CompositeProvider final : public score::memory::shared::fuchsia::IVmoHandleProvider
{
  public:
    CompositeProvider(std::shared_ptr<FidlVmoHandleServer> server,
                      EndpointConnector connector,
                      FuchsiaEventNotifier& notifier)
        : IVmoHandleProvider{}, server_{std::move(server)}, connector_{std::move(connector)}, notifier_{notifier}
    {
    }

    /// Attaches a new client for the instance (the skeleton restarted): used by the notifier to get a live eventpair.
    std::shared_ptr<FidlVmoHandleClient> Reattach(const std::uint16_t service_id, const std::uint16_t instance_id)
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        for (auto& entry : clients_)
        {
            std::uint16_t service{};
            std::uint16_t instance{};
            if (!FuchsiaEventNotifier::ParseInstanceKey(entry.first, service, instance) || (service != service_id) ||
                (instance != instance_id))
            {
                continue;
            }
            auto client = MakeClient();
            if ((client == nullptr) || (client->Attach() != ZX_OK))
            {
                return nullptr;
            }
            entry.second = client;
            return client;
        }
        return nullptr;
    }

    score::cpp::expected<zx::vmo, score::os::Error> Acquire(const std::string& key) noexcept override
    {
        if (!DefaultKeyClassifier(key).has_value())
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
        }
        const std::lock_guard<std::mutex> lock{mutex_};
        const auto instance = InstanceKeyOf(key);
        auto it = clients_.find(instance);
        if (it != clients_.end())
        {
            auto result = it->second->Acquire(key);
            // Every handle is handed out once. A client that attached before has used up this role: attach again.
            if (result.has_value() || !it->second->attach_info().has_value())
            {
                return result;
            }
            clients_.erase(it);
        }
        std::shared_ptr<FidlVmoHandleClient> client = MakeClient();
        if (client == nullptr)
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
        }
        auto result = client->Acquire(key);
        if (client->attach_info().has_value())
        {
            clients_[instance] = client;
            std::uint16_t service_id{};
            std::uint16_t instance_id{};
            if (FuchsiaEventNotifier::ParseInstanceKey(instance, service_id, instance_id))
            {
                notifier_.AddProvider(service_id, instance_id, std::move(client));
            }
        }
        return result;
    }

    score::cpp::expected_blank<score::os::Error> Publish(const std::string& key, zx::vmo consumer_handle) noexcept override
    {
        return server_->Publish(key, std::move(consumer_handle));
    }

    void Withdraw(const std::string& key) noexcept override
    {
        server_->Withdraw(key);
    }

  private:
    std::shared_ptr<FidlVmoHandleClient> MakeClient()
    {
        if (!connector_)
        {
            return nullptr;
        }
        auto endpoint = connector_();
        if (!endpoint.has_value())
        {
            return nullptr;
        }
        return std::make_shared<FidlVmoHandleClient>(std::move(*endpoint));
    }

    std::shared_ptr<FidlVmoHandleServer> server_;
    EndpointConnector connector_;
    FuchsiaEventNotifier& notifier_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<FidlVmoHandleClient>> clients_;
};

auto VmoShmRuntime::InstanceKeyOf(const std::string& region_key) -> std::string
{
    std::string key{region_key};
    for (const std::string prefix : {"lola-data-", "lola-ctl-"})
    {
        const auto pos = key.find(prefix);
        if (pos != std::string::npos)
        {
            key.replace(pos, prefix.size(), "lola-");
            break;
        }
    }
    if ((key.size() >= 2U) && (key.compare(key.size() - 2U, 2U, "-b") == 0))
    {
        key.resize(key.size() - 2U);
    }
    return key;
}

auto VmoShmRuntime::ConnectRoutedEndpoint() -> std::optional<fidl::ClientEnd<score_mw_com::Endpoint>>
{
    auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
    if (endpoints.is_error())
    {
        return std::nullopt;
    }
    if (component::Connect(std::move(endpoints->server), "/svc/score.mw.com.Endpoint").is_error())
    {
        return std::nullopt;
    }
    return std::move(endpoints->client);
}

VmoShmRuntime::VmoShmRuntime(async_dispatcher_t* dispatcher,
                             EndpointConnector connector,
                             score::memory::shared::ILockProvider* lock_provider)
    : server_{std::make_shared<FidlVmoHandleServer>(dispatcher)},
      notifier_{std::make_unique<FuchsiaEventNotifier>()},
      provider_{std::make_shared<CompositeProvider>(server_, std::move(connector), *notifier_)}
{
    notifier_->SetServer(server_.get());
    notifier_->SetReattacher([provider = provider_.get()](const std::uint16_t service_id, const std::uint16_t instance_id) {
        return provider->Reattach(service_id, instance_id);
    });
    IEventNotifier::SetCurrent(notifier_.get());
    factory_ = std::make_unique<score::memory::shared::fuchsia::VmoSharedMemoryFactory>(
        provider_, [](const std::string& key) { return IsConsumerWritable(key); });
    score::memory::shared::SharedMemoryFactory::InjectMock(factory_.get());
    if (lock_provider != nullptr)
    {
        score::memory::shared::LockProvider::Inject(lock_provider);
        lock_provider_installed_ = true;
    }
}

VmoShmRuntime::~VmoShmRuntime()
{
    IEventNotifier::SetCurrent(nullptr);
    score::memory::shared::SharedMemoryFactory::InjectMock(nullptr);
    if (lock_provider_installed_)
    {
        score::memory::shared::LockProvider::Inject(nullptr);
    }
}

}  // namespace score::mw::com::impl::lola::fuchsia
