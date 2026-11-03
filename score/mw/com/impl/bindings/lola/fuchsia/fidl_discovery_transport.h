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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_DISCOVERY_TRANSPORT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_DISCOVERY_TRANSPORT_H

#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_transport.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/result.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief How the transport reaches the discovery component. The default connects to the capabilities the realm
///        routes to the process (`/svc/score.mw.com.ServiceRegistry`, `/svc/score.mw.com.ServiceDiscovery`); tests
///        can connect to an in-process DiscoveryService instead.
struct FidlDiscoveryConnectors
{
    std::function<zx::result<fidl::ClientEnd<score_mw_com::ServiceRegistry>>()> registry;
    std::function<zx::result<fidl::ClientEnd<score_mw_com::ServiceDiscovery>>()> discovery;

    static FidlDiscoveryConnectors Default();
};

/// \brief IDiscoveryTransport over the FIDL protocols of score_mw_com.fidl.
/// \details Owns a thread with an async loop for the `Search` hanging gets and for noticing a lost registry channel.
///          Offer/StopOffer/Find are synchronous FIDL calls on the caller's thread. Connections are made lazily (a
///          skeleton only needs the registry, a proxy only discovery; a capability that is not routed to the process
///          only fails the calls that need it) and again after they broke, which covers a restart of the discovery
///          component (it keeps no state, the clients offer/search again).
class FidlDiscoveryTransport final : public IDiscoveryTransport
{
  public:
    explicit FidlDiscoveryTransport(FidlDiscoveryConnectors connectors = FidlDiscoveryConnectors::Default());
    ~FidlDiscoveryTransport() override;

    FidlDiscoveryTransport(const FidlDiscoveryTransport&) = delete;
    FidlDiscoveryTransport& operator=(const FidlDiscoveryTransport&) = delete;

    OfferStatus Offer(const WireInstance& instance) override;
    void StopOffer(const WireInstance& instance) override;
    std::optional<std::vector<WireInstance>> Find(const WireFilter& filter) override;
    std::unique_ptr<ISearch> StartSearch(const WireFilter& filter,
                                         SnapshotHandler on_snapshot,
                                         ClosedHandler on_closed) override;
    void SetRegistryLostHandler(std::function<void()> handler) override;
    void PostDelayed(std::function<void()> task, std::chrono::milliseconds delay) override;

  private:
    using OfferKey = std::tuple<std::uint64_t, std::uint32_t, std::uint8_t>;

    struct RegistryConnection
    {
        explicit RegistryConnection(fidl::ClientEnd<score_mw_com::ServiceRegistry> end) : client{std::move(end)} {}
        fidl::SyncClient<score_mw_com::ServiceRegistry> client;
        std::unique_ptr<async::WaitOnce> peer_closed;
        // What was offered on this very connection; a repeated offer on it is a no-op, on a new connection it is
        // refused by discovery until the old connection is withdrawn.
        std::set<OfferKey> offered;
    };

    class Search;

    std::shared_ptr<RegistryConnection> GetRegistry();
    void DropRegistry(const std::shared_ptr<RegistryConnection>& stale, bool notify);
    std::shared_ptr<fidl::SyncClient<score_mw_com::ServiceDiscovery>> GetDiscovery();
    void DropDiscovery(const std::shared_ptr<fidl::SyncClient<score_mw_com::ServiceDiscovery>>& stale);
    std::optional<fidl::ClientEnd<score_mw_com::Search>> OpenSearch(const WireFilter& filter);

    FidlDiscoveryConnectors connectors_;
    async::Loop loop_;

    std::mutex mutex_;
    std::shared_ptr<RegistryConnection> registry_;
    std::shared_ptr<fidl::SyncClient<score_mw_com::ServiceDiscovery>> discovery_;
    std::function<void()> registry_lost_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_DISCOVERY_TRANSPORT_H
