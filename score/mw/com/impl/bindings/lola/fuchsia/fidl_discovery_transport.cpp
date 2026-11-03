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
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_discovery_transport.h"

#include "score/mw/log/logging.h"

#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/zx/time.h>
#include <score/utility.hpp>

#include <atomic>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr const char* kLogContext = "lola_discovery";
constexpr const char* kRegistryPath = "/svc/score.mw.com.ServiceRegistry";
constexpr const char* kDiscoveryPath = "/svc/score.mw.com.ServiceDiscovery";

// An offer is refused with ALREADY_OFFERED as long as discovery has not noticed that the previous incarnation of the
// skeleton (same instance, old connection) is gone; that is a matter of milliseconds (README 6.3, E2), wait for it.
constexpr int kMaxOfferAttempts = 100;
constexpr auto kOfferRetryDelay = std::chrono::milliseconds{20};

score_mw_com::Quality ToFidl(const WireQuality quality) noexcept
{
    return quality == WireQuality::kAsilB ? score_mw_com::Quality::kAsilB : score_mw_com::Quality::kQm;
}

score_mw_com::ServiceInstanceId ToFidlId(const WireInstance& instance) noexcept
{
    score_mw_com::ServiceInstanceId id;
    id.service_id() = instance.service_id;
    id.instance_id() = instance.instance_id;
    id.major_version() = 0U;
    id.minor_version() = 0U;
    return id;
}

score_mw_com::InstanceFilter ToFidl(const WireFilter& filter)
{
    score_mw_com::InstanceFilter result;
    result.service_id(filter.service_id);
    if (filter.instance_id.has_value())
    {
        result.instance_id(filter.instance_id.value());
    }
    result.quality(ToFidl(filter.quality));
    return result;
}

std::vector<WireInstance> FromFidl(const std::vector<score_mw_com::OfferedInstance>& instances)
{
    std::vector<WireInstance> result;
    result.reserve(instances.size());
    for (const auto& instance : instances)
    {
        const auto quality = static_cast<std::uint8_t>(instance.quality());
        if ((quality != static_cast<std::uint8_t>(WireQuality::kQm)) &&
            (quality != static_cast<std::uint8_t>(WireQuality::kAsilB)))
        {
            continue;  // a quality level of a newer discovery that this client does not know
        }
        result.push_back(WireInstance{instance.id().service_id(),
                                      instance.id().instance_id(),
                                      static_cast<WireQuality>(quality),
                                      instance.epoch()});
    }
    return result;
}

}  // namespace

FidlDiscoveryConnectors FidlDiscoveryConnectors::Default()
{
    return FidlDiscoveryConnectors{
        []() { return component::Connect<score_mw_com::ServiceRegistry>(kRegistryPath); },
        []() { return component::Connect<score_mw_com::ServiceDiscovery>(kDiscoveryPath); },
    };
}

/// One running `Search`: a hanging-get loop on the transport thread. All members except `cancelled` are only touched
/// on the transport thread.
class FidlDiscoveryTransport::Search final : public ISearch
{
  public:
    Search(FidlDiscoveryTransport& transport,
           const WireFilter& filter,
           SnapshotHandler on_snapshot,
           ClosedHandler on_closed)
        : state_{std::make_shared<State>(transport, filter, std::move(on_snapshot), std::move(on_closed))}
    {
        async::PostTask(transport.loop_.dispatcher(), [state = state_]() { Begin(state); });
    }

    ~Search() override
    {
        state_->cancelled = true;
        // The client must be destroyed on the dispatcher thread. If the loop is already shut down the task is dropped
        // (and the state released here), which is fine as well.
        async::PostTask(state_->dispatcher, [state = state_]() { state->client.reset(); });
    }

  private:
    struct State final : public fidl::AsyncEventHandler<score_mw_com::Search>
    {
        State(FidlDiscoveryTransport& transport_in,
              const WireFilter& filter_in,
              SnapshotHandler snapshot_in,
              ClosedHandler closed_in)
            : transport{transport_in},
              dispatcher{transport_in.loop_.dispatcher()},
              filter{filter_in},
              on_snapshot{std::move(snapshot_in)},
              on_closed{std::move(closed_in)}
        {
        }

        void on_fidl_error(fidl::UnbindInfo) override
        {
            ReportClosed();
        }

        void ReportClosed()
        {
            if (cancelled || closed_reported)
            {
                return;
            }
            closed_reported = true;
            // The discovery component may be gone: reconnect for the next search.
            transport.DropDiscovery(nullptr);
            on_closed();
        }

        FidlDiscoveryTransport& transport;
        async_dispatcher_t* dispatcher;
        WireFilter filter;
        SnapshotHandler on_snapshot;
        ClosedHandler on_closed;
        std::atomic<bool> cancelled{false};
        bool closed_reported{false};
        std::optional<fidl::Client<score_mw_com::Search>> client;
    };

    static void Begin(const std::shared_ptr<State>& state)
    {
        if (state->cancelled)
        {
            return;
        }
        auto end = state->transport.OpenSearch(state->filter);
        if (!end.has_value())
        {
            state->ReportClosed();
            return;
        }
        state->client.emplace();
        state->client->Bind(std::move(end.value()), state->dispatcher, state.get());
        Watch(state);
    }

    static void Watch(const std::shared_ptr<State>& state)
    {
        (*state->client)
            ->Watch()
            .Then([weak = std::weak_ptr<State>{state}](fidl::Result<score_mw_com::Search::Watch>& result) {
                const auto state = weak.lock();
                if (!state || state->cancelled)
                {
                    return;
                }
                if (result.is_error())
                {
                    state->ReportClosed();
                    return;
                }
                state->on_snapshot(FromFidl(result->instances()));
                if (!state->cancelled && !state->closed_reported && state->client.has_value())
                {
                    Watch(state);
                }
            });
    }

    std::shared_ptr<State> state_;
};

FidlDiscoveryTransport::FidlDiscoveryTransport(FidlDiscoveryConnectors connectors)
    : IDiscoveryTransport{}, connectors_{std::move(connectors)}, loop_{&kAsyncLoopConfigNoAttachToCurrentThread}
{
    const auto status = loop_.StartThread("lola_discovery");
    if (status != ZX_OK)
    {
        score::mw::log::LogFatal(kLogContext) << "Cannot start the discovery transport thread:" << status;
        std::terminate();
    }
}

FidlDiscoveryTransport::~FidlDiscoveryTransport()
{
    loop_.Shutdown();
}

auto FidlDiscoveryTransport::GetRegistry() -> std::shared_ptr<RegistryConnection>
{
    const std::lock_guard lock{mutex_};
    if (registry_ != nullptr)
    {
        return registry_;
    }
    auto end = connectors_.registry();
    if (end.is_error())
    {
        score::mw::log::LogWarn(kLogContext) << "Cannot connect to ServiceRegistry:" << end.status_value();
        return nullptr;
    }
    auto connection = std::make_shared<RegistryConnection>(std::move(end.value()));
    connection->peer_closed = std::make_unique<async::WaitOnce>(connection->client.client_end().channel().get(),
                                                                ZX_CHANNEL_PEER_CLOSED);
    const auto status = connection->peer_closed->Begin(
        loop_.dispatcher(),
        [this, weak = std::weak_ptr<RegistryConnection>{connection}](
            async_dispatcher_t*, async::WaitOnce*, zx_status_t wait_status, const zx_packet_signal_t*) {
            if (wait_status != ZX_OK)
            {
                return;
            }
            if (const auto closed = weak.lock())
            {
                DropRegistry(closed, true);
            }
        });
    if (status != ZX_OK)
    {
        return nullptr;
    }
    registry_ = connection;
    return registry_;
}

auto FidlDiscoveryTransport::DropRegistry(const std::shared_ptr<RegistryConnection>& stale, const bool notify) -> void
{
    std::function<void()> lost;
    {
        const std::lock_guard lock{mutex_};
        if (registry_ != stale)
        {
            return;
        }
        registry_.reset();
        lost = registry_lost_;
    }
    score::mw::log::LogInfo(kLogContext) << "Registry connection lost";
    // Destroyed (and the wait cancelled) on the loop thread, never inside the wait handler itself.
    async::PostTask(loop_.dispatcher(), [stale]() {});
    if (notify && lost)
    {
        async::PostTask(loop_.dispatcher(), std::move(lost));
    }
}

auto FidlDiscoveryTransport::Offer(const WireInstance& instance) -> OfferStatus
{
    const OfferKey key{instance.service_id, instance.instance_id, static_cast<std::uint8_t>(instance.quality)};
    for (int attempt = 0; attempt < kMaxOfferAttempts; ++attempt)
    {
        const auto registry = GetRegistry();
        if (registry == nullptr)
        {
            return OfferStatus::kUnavailable;
        }
        {
            const std::lock_guard lock{mutex_};
            if (registry->offered.count(key) != 0U)
            {
                return OfferStatus::kOk;
            }
        }
        const auto result =
            registry->client->Offer({ToFidlId(instance), ToFidl(instance.quality), instance.epoch});
        if (result.is_ok())
        {
            const std::lock_guard lock{mutex_};
            score::cpp::ignore = registry->offered.insert(key);
            return OfferStatus::kOk;
        }
        if (result.error_value().is_domain_error())
        {
            if (result.error_value().domain_error() != score_mw_com::OfferError::kAlreadyOffered)
            {
                return OfferStatus::kRejected;
            }
        }
        else
        {
            DropRegistry(registry, true);
        }
        std::this_thread::sleep_for(kOfferRetryDelay);
    }
    return OfferStatus::kUnavailable;
}

auto FidlDiscoveryTransport::StopOffer(const WireInstance& instance) -> void
{
    std::shared_ptr<RegistryConnection> registry;
    {
        const std::lock_guard lock{mutex_};
        registry = registry_;
        if (registry != nullptr)
        {
            score::cpp::ignore = registry->offered.erase(
                OfferKey{instance.service_id, instance.instance_id, static_cast<std::uint8_t>(instance.quality)});
        }
    }
    if (registry == nullptr)
    {
        // No connection, so discovery does not know any offer of ours.
        return;
    }
    const auto result = registry->client->StopOffer({ToFidlId(instance), ToFidl(instance.quality)});
    if (result.is_error())
    {
        DropRegistry(registry, true);
    }
}

auto FidlDiscoveryTransport::GetDiscovery() -> std::shared_ptr<fidl::SyncClient<score_mw_com::ServiceDiscovery>>
{
    const std::lock_guard lock{mutex_};
    if (discovery_ == nullptr)
    {
        auto end = connectors_.discovery();
        if (end.is_error())
        {
            score::mw::log::LogWarn(kLogContext) << "Cannot connect to ServiceDiscovery:" << end.status_value();
            return nullptr;
        }
        discovery_ = std::make_shared<fidl::SyncClient<score_mw_com::ServiceDiscovery>>(std::move(end.value()));
    }
    return discovery_;
}

auto FidlDiscoveryTransport::DropDiscovery(
    const std::shared_ptr<fidl::SyncClient<score_mw_com::ServiceDiscovery>>& stale) -> void
{
    const std::lock_guard lock{mutex_};
    if ((stale == nullptr) || (discovery_ == stale))
    {
        discovery_.reset();
    }
}

auto FidlDiscoveryTransport::Find(const WireFilter& filter) -> std::optional<std::vector<WireInstance>>
{
    // A second attempt reconnects after a restart of the discovery component.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto discovery = GetDiscovery();
        if (discovery == nullptr)
        {
            return std::nullopt;
        }
        const auto result = (*discovery)->Find({ToFidl(filter)});
        if (result.is_ok())
        {
            return FromFidl(result->instances());
        }
        DropDiscovery(discovery);
    }
    return std::nullopt;
}

auto FidlDiscoveryTransport::OpenSearch(const WireFilter& filter) -> std::optional<fidl::ClientEnd<score_mw_com::Search>>
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto discovery = GetDiscovery();
        if (discovery == nullptr)
        {
            return std::nullopt;
        }
        auto endpoints = fidl::CreateEndpoints<score_mw_com::Search>();
        if (endpoints.is_error())
        {
            return std::nullopt;
        }
        if ((*discovery)->StartFind({ToFidl(filter), std::move(endpoints->server)}).is_ok())
        {
            return std::move(endpoints->client);
        }
        DropDiscovery(discovery);
    }
    return std::nullopt;
}

auto FidlDiscoveryTransport::StartSearch(const WireFilter& filter, SnapshotHandler on_snapshot, ClosedHandler on_closed)
    -> std::unique_ptr<ISearch>
{
    return std::make_unique<Search>(*this, filter, std::move(on_snapshot), std::move(on_closed));
}

auto FidlDiscoveryTransport::SetRegistryLostHandler(std::function<void()> handler) -> void
{
    const std::lock_guard lock{mutex_};
    registry_lost_ = std::move(handler);
}

auto FidlDiscoveryTransport::PostDelayed(std::function<void()> task, const std::chrono::milliseconds delay) -> void
{
    async::PostDelayedTask(
        loop_.dispatcher(), [task = std::move(task)]() { task(); }, zx::msec(delay.count()));
}

}  // namespace score::mw::com::impl::lola::fuchsia
