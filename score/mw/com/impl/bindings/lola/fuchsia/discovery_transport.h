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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_TRANSPORT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_TRANSPORT_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Quality of an offer as carried by the FIDL `Quality` enum (QM = 1, ASIL_B = 2).
enum class WireQuality : std::uint8_t
{
    kQm = 1U,
    kAsilB = 2U,
};

/// \brief Platform neutral image of `score.mw.com.OfferedInstance`.
/// \details Mapping to LoLa (see FuchsiaServiceDiscoveryClient): service_id = LolaServiceId, instance_id =
///          LolaServiceInstanceId. major/minor version are not part of the LoLa deployment and always 0 on the wire.
struct WireInstance
{
    std::uint64_t service_id;
    std::uint32_t instance_id;
    WireQuality quality;
    std::uint64_t epoch;
};

inline bool operator==(const WireInstance& lhs, const WireInstance& rhs) noexcept
{
    return (lhs.service_id == rhs.service_id) && (lhs.instance_id == rhs.instance_id) &&
           (lhs.quality == rhs.quality) && (lhs.epoch == rhs.epoch);
}

/// \brief Platform neutral image of `score.mw.com.InstanceFilter` (the service id is always set by LoLa).
struct WireFilter
{
    std::uint64_t service_id;
    std::optional<std::uint32_t> instance_id;
    WireQuality quality;
};

enum class OfferStatus
{
    kOk,
    /// The registry refuses the offer (service id not configured for this application).
    kRejected,
    /// The registry cannot be reached (not routed, discovery gone).
    kUnavailable,
};

/// \brief Handle of a running search. Destroying it cancels the search (closes the `Search` channel). A handler call
///        that is already in flight may still arrive afterwards; the client ignores it. May be destroyed on any thread.
class ISearch
{
  public:
    virtual ~ISearch() = default;
};

/// \brief Seam between the LoLa discovery logic and the FIDL protocols `ServiceRegistry` / `ServiceDiscovery` /
///        `Search`, so that the logic can be tested on the host with a fake.
/// \details All handlers are called from one transport thread. Offer/StopOffer/Find may be called from any thread and
///          block until the discovery component answered.
class IDiscoveryTransport
{
  public:
    /// Called with every full snapshot (the first one right after the search was started, i.e. also after a restart).
    using SnapshotHandler = std::function<void(std::vector<WireInstance>)>;
    /// Called when the search channel is closed (discovery restarted/crashed/not routed). The search is dead and has
    /// to be started again.
    using ClosedHandler = std::function<void()>;

    virtual ~IDiscoveryTransport() = default;

    virtual OfferStatus Offer(const WireInstance& instance) = 0;
    virtual void StopOffer(const WireInstance& instance) = 0;
    virtual std::optional<std::vector<WireInstance>> Find(const WireFilter& filter) = 0;
    virtual std::unique_ptr<ISearch> StartSearch(const WireFilter& filter,
                                                 SnapshotHandler on_snapshot,
                                                 ClosedHandler on_closed) = 0;

    /// Called (on the transport thread) when the registry connection was lost: the discovery component forgot all
    /// offers of this process, they have to be offered again.
    virtual void SetRegistryLostHandler(std::function<void()> handler) = 0;

    /// Runs `task` on the transport thread after `delay`. Tasks must not run after destruction of the transport.
    virtual void PostDelayed(std::function<void()> task, std::chrono::milliseconds delay) = 0;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_TRANSPORT_H
