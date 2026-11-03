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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FUCHSIA_EVENT_NOTIFIER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FUCHSIA_EVENT_NOTIFIER_H

#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/i_event_notifier.h"

#include <lib/zx/port.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief IEventNotifier on the notification eventpairs of Attach.
/// \details Skeleton: NotifySubscribers() forwards to FidlVmoHandleServer (signal USER_0 on every subscriber).
///
///          Proxy: one thread waits on one port for the notification eventpair of every attached provider
///          (service instance). A wake-up (USER_0) is handled as: clear the signal, then invoke every receive handler
///          registered for the provider, then wait again. Clearing first is what makes a coalesced signal harmless:
///          a publication after the clear raises the signal again and the wait fires at once, a publication before it
///          is seen by the handlers' re-read of the sample queue.
///
///          Provider death (peer closed): the handlers are invoked one last time (samples published before the death
///          are still readable), the provider is reported through the lost callback and stays known as lost.
///          Reregister() attaches again through the reattacher; the registrations survive.
///
///          The handlers run on the notification thread, one provider after the other. A handler that blocks delays
///          the other providers' handlers (never loses their events, see above).
class FuchsiaEventNotifier final : public IEventNotifier
{
  public:
    using ProviderLostCallback = std::function<void(std::uint16_t service_id, std::uint16_t instance_id)>;
    /// Creates and attaches a client for the instance, nullptr if that fails.
    using Reattacher =
        std::function<std::shared_ptr<FidlVmoHandleClient>(std::uint16_t service_id, std::uint16_t instance_id)>;

    FuchsiaEventNotifier();
    ~FuchsiaEventNotifier() override;

    /// \brief Skeleton side; the server must outlive the notifier or be reset to nullptr first.
    void SetServer(FidlVmoHandleServer* server) noexcept;
    void SetProviderLostCallback(ProviderLostCallback callback);
    void SetReattacher(Reattacher reattacher);

    /// \brief Proxy side: `client` attached to the skeleton of (service_id, instance_id). Replaces a former client of
    ///        the instance, the registrations are kept.
    void AddProvider(const std::uint16_t service_id,
                     const std::uint16_t instance_id,
                     std::shared_ptr<FidlVmoHandleClient> client);

    /// \brief false if the provider is unknown or its skeleton is gone.
    bool IsProviderAlive(const std::uint16_t service_id, const std::uint16_t instance_id) const;

    /// \brief "lola-<service>-<instance>" as produced by VmoShmRuntime::InstanceKeyOf.
    static bool ParseInstanceKey(const std::string& instance_key, std::uint16_t& service_id, std::uint16_t& instance_id);

    std::size_t NotifySubscribers() noexcept override;
    bool HasProvider(const ElementFqId event_id) const noexcept override;
    std::optional<HandlerRegistrationNo> Register(const ElementFqId event_id,
                                                  std::weak_ptr<ScopedEventReceiveHandler> handler) noexcept override;
    void Unregister(const ElementFqId event_id, const HandlerRegistrationNo registration_no) noexcept override;
    void Reregister(const ElementFqId event_id) noexcept override;

  private:
    using ProviderKey = std::pair<std::uint16_t, std::uint16_t>;

    struct Provider
    {
        std::uint64_t port_key;
        std::shared_ptr<FidlVmoHandleClient> client;
        bool lost;
    };
    struct Registration
    {
        HandlerRegistrationNo number;
        ElementFqId event_id;
        std::weak_ptr<ScopedEventReceiveHandler> handler;
    };

    static ProviderKey KeyOf(const ElementFqId event_id) noexcept;
    void Run() noexcept;
    void HandlePacket(const zx_port_packet_t& packet) noexcept;
    void InvokeHandlers(const ProviderKey& key) noexcept;
    void ArmLocked(Provider& provider);

    zx::port port_;
    std::thread thread_;

    mutable std::mutex mutex_;
    std::atomic<FidlVmoHandleServer*> server_{nullptr};
    ProviderLostCallback provider_lost_;
    Reattacher reattacher_;
    std::map<ProviderKey, Provider> providers_;
    std::map<ProviderKey, std::vector<Registration>> registrations_;
    std::uint64_t next_port_key_{1U};
    HandlerRegistrationNo next_registration_{0U};
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FUCHSIA_EVENT_NOTIFIER_H
