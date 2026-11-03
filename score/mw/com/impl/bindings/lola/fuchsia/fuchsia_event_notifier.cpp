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
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_event_notifier.h"

#include "score/mw/log/logging.h"

#include <zircon/syscalls.h>

#include <cstdlib>
#include <string>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr const char* kLogContext = "lola_events";
constexpr std::uint64_t kQuitKey{0U};
constexpr zx_signals_t kWatchedSignals{ZX_USER_SIGNAL_0 | ZX_EVENTPAIR_PEER_CLOSED};

bool ParseNumber(const std::string& text, std::uint16_t& out)
{
    if (text.empty() || (text.find_first_not_of("0123456789") != std::string::npos))
    {
        return false;
    }
    const unsigned long value = std::strtoul(text.c_str(), nullptr, 10);
    if (value > 0xFFFFU)
    {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

FuchsiaEventNotifier::FuchsiaEventNotifier() : IEventNotifier{}
{
    if (zx::port::create(0U, &port_) != ZX_OK)
    {
        score::mw::log::LogFatal(kLogContext) << "Cannot create the notification port.";
        std::abort();
    }
    thread_ = std::thread{[this]() noexcept { Run(); }};
}

FuchsiaEventNotifier::~FuchsiaEventNotifier()
{
    zx_port_packet_t quit{};
    quit.key = kQuitKey;
    quit.type = ZX_PKT_TYPE_USER;
    static_cast<void>(port_.queue(&quit));
    thread_.join();
}

auto FuchsiaEventNotifier::SetServer(FidlVmoHandleServer* server) noexcept -> void
{
    server_.store(server);
}

auto FuchsiaEventNotifier::SetProviderLostCallback(ProviderLostCallback callback) -> void
{
    const std::lock_guard<std::mutex> lock{mutex_};
    provider_lost_ = std::move(callback);
}

auto FuchsiaEventNotifier::SetReattacher(Reattacher reattacher) -> void
{
    const std::lock_guard<std::mutex> lock{mutex_};
    reattacher_ = std::move(reattacher);
}

auto FuchsiaEventNotifier::ParseInstanceKey(const std::string& instance_key,
                                            std::uint16_t& service_id,
                                            std::uint16_t& instance_id) -> bool
{
    const std::string prefix{"lola-"};
    const std::size_t start = (!instance_key.empty() && instance_key.front() == '/') ? 1U : 0U;
    if (instance_key.compare(start, prefix.size(), prefix) != 0)
    {
        return false;
    }
    const auto dash = instance_key.find('-', start + prefix.size());
    if (dash == std::string::npos)
    {
        return false;
    }
    return ParseNumber(instance_key.substr(start + prefix.size(), dash - start - prefix.size()), service_id) &&
           ParseNumber(instance_key.substr(dash + 1U), instance_id);
}

auto FuchsiaEventNotifier::KeyOf(const ElementFqId event_id) noexcept -> ProviderKey
{
    return ProviderKey{event_id.service_id_, event_id.instance_id_};
}

auto FuchsiaEventNotifier::ArmLocked(Provider& provider) -> void
{
    if (!provider.client->notification().is_valid())
    {
        return;
    }
    const zx_status_t status = provider.client->notification().wait_async(port_, provider.port_key, kWatchedSignals, 0U);
    if (status != ZX_OK)
    {
        score::mw::log::LogError(kLogContext) << "wait_async on the notification failed: " << status;
        provider.lost = true;
    }
}

auto FuchsiaEventNotifier::AddProvider(const std::uint16_t service_id,
                                       const std::uint16_t instance_id,
                                       std::shared_ptr<FidlVmoHandleClient> client) -> void
{
    const std::lock_guard<std::mutex> lock{mutex_};
    const ProviderKey key{service_id, instance_id};
    const auto old = providers_.find(key);
    if (old != providers_.end())
    {
        if (old->second.client->notification().is_valid())
        {
            static_cast<void>(port_.cancel(old->second.client->notification(), old->second.port_key));
        }
        providers_.erase(old);
    }
    const auto inserted = providers_.emplace(key, Provider{next_port_key_++, std::move(client), false});
    ArmLocked(inserted.first->second);
}

auto FuchsiaEventNotifier::IsProviderAlive(const std::uint16_t service_id, const std::uint16_t instance_id) const -> bool
{
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto found = providers_.find(ProviderKey{service_id, instance_id});
    return (found != providers_.end()) && !found->second.lost;
}

auto FuchsiaEventNotifier::NotifySubscribers() noexcept -> std::size_t
{
    auto* const server = server_.load();
    return (server != nullptr) ? server->NotifySubscribers() : 0U;
}

auto FuchsiaEventNotifier::HasProvider(const ElementFqId event_id) const noexcept -> bool
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return providers_.count(KeyOf(event_id)) != 0U;
}

auto FuchsiaEventNotifier::Register(const ElementFqId event_id,
                                    std::weak_ptr<ScopedEventReceiveHandler> handler) noexcept
    -> std::optional<HandlerRegistrationNo>
{
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto key = KeyOf(event_id);
    if (providers_.count(key) == 0U)
    {
        return std::nullopt;
    }
    const HandlerRegistrationNo number = (next_registration_++ & ~kRegistrationBit) | kRegistrationBit;
    registrations_[key].push_back(Registration{number, event_id, std::move(handler)});
    return number;
}

auto FuchsiaEventNotifier::Unregister(const ElementFqId event_id, const HandlerRegistrationNo registration_no) noexcept
    -> void
{
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto found = registrations_.find(KeyOf(event_id));
    if (found == registrations_.end())
    {
        return;
    }
    auto& list = found->second;
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->number == registration_no)
        {
            list.erase(it);
            break;
        }
    }
    if (list.empty())
    {
        registrations_.erase(found);
    }
}

auto FuchsiaEventNotifier::Reregister(const ElementFqId event_id) noexcept -> void
{
    const auto key = KeyOf(event_id);
    Reattacher reattacher;
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        const auto found = providers_.find(key);
        if ((found == providers_.end()) || !found->second.lost)
        {
            return;
        }
        reattacher = reattacher_;
    }
    if (!reattacher)
    {
        score::mw::log::LogWarn(kLogContext) << "Provider " << key.first << "/" << key.second
                                             << " is gone and no reattacher is installed.";
        return;
    }
    auto client = reattacher(key.first, key.second);
    if (client == nullptr)
    {
        score::mw::log::LogWarn(kLogContext) << "Reattach of provider " << key.first << "/" << key.second << " failed.";
        return;
    }
    AddProvider(key.first, key.second, std::move(client));
    // The new skeleton has published nothing the handlers have seen: let them re-read.
    InvokeHandlers(key);
}

auto FuchsiaEventNotifier::InvokeHandlers(const ProviderKey& key) noexcept -> void
{
    std::vector<std::weak_ptr<ScopedEventReceiveHandler>> handlers;
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        const auto found = registrations_.find(key);
        if (found == registrations_.end())
        {
            return;
        }
        for (const auto& registration : found->second)
        {
            handlers.push_back(registration.handler);
        }
    }
    for (auto& weak : handlers)
    {
        if (auto handler = weak.lock())
        {
            // The result only tells whether the scope of the handler has expired.
            static_cast<void>((*handler)());
        }
    }
}

auto FuchsiaEventNotifier::HandlePacket(const zx_port_packet_t& packet) noexcept -> void
{
    ProviderKey key{};
    std::shared_ptr<FidlVmoHandleClient> client;
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        for (const auto& entry : providers_)
        {
            if (entry.second.port_key == packet.key)
            {
                key = entry.first;
                client = entry.second.client;
                break;
            }
        }
    }
    if (client == nullptr)
    {
        return;  // packet of a replaced provider
    }
    const zx_signals_t observed = packet.signal.observed;
    if ((observed & ZX_USER_SIGNAL_0) != 0U)
    {
        // Clear before the handlers re-read the queues: a publication after this raises the signal again.
        static_cast<void>(client->notification().signal(ZX_USER_SIGNAL_0, 0U));
    }
    const bool peer_closed = (observed & ZX_EVENTPAIR_PEER_CLOSED) != 0U;

    InvokeHandlers(key);

    ProviderLostCallback lost_callback;
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        const auto found = providers_.find(key);
        if ((found == providers_.end()) || (found->second.port_key != packet.key))
        {
            return;
        }
        if (peer_closed)
        {
            found->second.lost = true;
            lost_callback = provider_lost_;
        }
        else
        {
            ArmLocked(found->second);
        }
    }
    if (peer_closed)
    {
        score::mw::log::LogInfo(kLogContext) << "Provider " << key.first << "/" << key.second << " is gone.";
        if (lost_callback)
        {
            lost_callback(key.first, key.second);
        }
    }
}

auto FuchsiaEventNotifier::Run() noexcept -> void
{
    for (;;)
    {
        zx_port_packet_t packet{};
        if (port_.wait(zx::time::infinite(), &packet) != ZX_OK)
        {
            return;
        }
        if (packet.key == kQuitKey)
        {
            return;
        }
        HandlePacket(packet);
    }
}

}  // namespace score::mw::com::impl::lola::fuchsia
