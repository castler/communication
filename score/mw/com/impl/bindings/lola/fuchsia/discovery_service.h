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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_SERVICE_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_SERVICE_H

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async/dispatcher.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief The registry/discovery component logic (README 6.3): `ServiceRegistry` for skeletons, `ServiceDiscovery` and
///        `Search` for proxies.
/// \details One protocol handler per *caller*: the realm routes a per-caller capability (renamed with `as`), so which
///          handler a connection arrives on tells who is calling. A skeleton may only offer the service ids it is
///          configured for; a consumer only ever sees instances of service ids it is allowed to use. Both tables are
///          meant to be generated from the mw::com configuration, like the realm.
///
///          Closing a registry connection (also a crash of the skeleton) withdraws every offer made through it.
///          `Search.Watch` is a hanging get that always returns full snapshots.
///
///          Not thread-safe: everything runs on `dispatcher`, which must be single-threaded, and the service must
///          outlive the bindings created from its handlers.
class DiscoveryService
{
  public:
    struct Participant
    {
        std::uint64_t application_id;
        /// Skeleton: service ids it may offer. Consumer: service ids it may find.
        std::set<std::uint64_t> service_ids;
    };

    explicit DiscoveryService(async_dispatcher_t* dispatcher);
    ~DiscoveryService();
    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    fidl::ProtocolHandler<score_mw_com::ServiceRegistry> RegistryHandler(Participant skeleton);
    fidl::ProtocolHandler<score_mw_com::ServiceDiscovery> DiscoveryHandler(Participant consumer);

    std::size_t OfferCount() const noexcept;
    std::size_t SearchCount() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_SERVICE_H
