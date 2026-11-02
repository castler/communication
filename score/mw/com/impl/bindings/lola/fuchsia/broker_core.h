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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_BROKER_CORE_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_BROKER_CORE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <tuple>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Platform neutral state machine of the lease broker (t03 section 4.1/4.2); BrokerService is the Fuchsia shell
///        around it (VMOs, eventpairs, process handles).
/// \details States of an incarnation of one instance:
///          PROVISIONING --Publish--> ACTIVE --skeleton lost/Resign--> FENCED(draining while proxy leases live)
///          --last proxy lease dead/released--> released (erased). A new incarnation always gets a new epoch and new
///          regions (rebirth), the fenced one is never reused.
///          Not thread safe: all calls must come from one thread (the dispatcher of the service).
struct InstanceKey
{
    std::uint64_t service_id;
    std::uint32_t instance_id;
    std::uint8_t major_version;
    std::uint8_t minor_version;

    friend bool operator<(const InstanceKey& a, const InstanceKey& b) noexcept
    {
        return std::tie(a.service_id, a.instance_id, a.major_version, a.minor_version) <
               std::tie(b.service_id, b.instance_id, b.major_version, b.minor_version);
    }
};

enum class BrokerStatus : std::uint8_t
{
    kOk,
    kNotActive,
    kAlreadyProvisioned,
    kResourceExhausted,
    kStaleEpoch,
    kUnknownLease,
};

enum class BrokerFenceReason : std::uint8_t
{
    kNone = 0,
    kResigned = 1,
    kProcessTerminated = 2,
    kLivenessClosed = 3,
};

enum class LeaseKind : std::uint8_t
{
    kSkeleton,
    kProxy,
};

using LeaseId = std::uint64_t;
using Epoch = std::uint64_t;

struct CoreStats
{
    Epoch active_epoch{0U};
    Epoch last_epoch{0U};
    std::uint32_t retained_incarnations{0U};
    std::uint32_t draining_incarnations{0U};
    std::uint32_t live_proxy_leases{0U};
    std::uint32_t fenced_total{0U};
    std::uint32_t released_total{0U};
    BrokerFenceReason last_fence_reason{BrokerFenceReason::kNone};
    std::uint32_t fenced_by_process{0U};
    std::uint32_t fenced_by_liveness{0U};
};

struct BrokerCoreHooks
{
    /// The incarnation is fenced: tombstone the control header, signal the proxies. Called exactly once per epoch.
    std::function<void(const InstanceKey&, Epoch, BrokerFenceReason)> on_fence;
    /// The incarnation is no longer pinned: drop the retained regions. Called exactly once per epoch.
    std::function<void(const InstanceKey&, Epoch)> on_release;
    /// Every lease that ends (skeleton or proxy, by death or release). The seam for holders of other resources
    /// (e.g. a lock registry: locks keyed by lease id are dropped here, never by pid).
    std::function<void(LeaseId, LeaseKind)> on_lease_end;
};

struct BrokerLimits
{
    /// Fenced incarnations of one instance that live proxies may still pin before BeginIncarnation is refused.
    std::size_t max_draining_per_instance{2U};
};

class BrokerCore
{
  public:
    using Clock = std::function<std::uint64_t()>;

    BrokerCore(Clock boot_clock_ns, BrokerCoreHooks hooks, BrokerLimits limits = {});

    /// \brief Allocates epoch = max(last + 1, clock) and a skeleton lease; the incarnation is PROVISIONING.
    BrokerStatus BeginIncarnation(const InstanceKey& instance, Epoch& epoch, LeaseId& skeleton_lease);
    /// \brief Broker restart: a live skeleton re-announces its incarnation `epoch` (allocated by the previous broker).
    ///        The incarnation becomes ACTIVE at once; `last_epoch` is raised so later epochs stay greater. Refused with
    ///        kAlreadyProvisioned while the instance has a live incarnation. The caller has verified the regions.
    BrokerStatus AdoptIncarnation(const InstanceKey& instance, Epoch epoch, LeaseId& skeleton_lease);
    /// \brief PROVISIONING -> ACTIVE. The caller has verified the regions.
    BrokerStatus Publish(const InstanceKey& instance, Epoch epoch);
    /// \brief Orderly StopOffer. Idempotent; a stale epoch is ignored.
    void Resign(const InstanceKey& instance, Epoch epoch);
    /// \brief The skeleton holder died (process terminated or liveness closed). Idempotent per lease.
    void SkeletonLost(LeaseId skeleton_lease, BrokerFenceReason reason);
    /// \brief Proxy lease on the ACTIVE incarnation `epoch`, else kNotActive/kStaleEpoch.
    BrokerStatus RegisterProxy(const InstanceKey& instance, Epoch epoch, LeaseId& proxy_lease);
    /// \brief Proxy died or detached. Idempotent.
    BrokerStatus ProxyEnded(LeaseId proxy_lease);

    CoreStats Stats(const InstanceKey& instance) const;

  private:
    enum class State
    {
        kProvisioning,
        kActive,
        kFenced,
    };
    struct Incarnation
    {
        State state{State::kProvisioning};
        LeaseId skeleton_lease{0U};
        std::size_t proxy_leases{0U};
    };
    struct Instance
    {
        Epoch last_epoch{0U};
        std::map<Epoch, Incarnation> incarnations;
        CoreStats counters;
    };
    struct LeaseInfo
    {
        LeaseKind kind;
        InstanceKey instance;
        Epoch epoch;
    };

    void Fence(const InstanceKey& key, Instance& instance, Epoch epoch, BrokerFenceReason reason);
    void ReleaseIfUnpinned(const InstanceKey& key, Instance& instance, Epoch epoch);
    static std::size_t Draining(const Instance& instance);

    Clock clock_;
    BrokerCoreHooks hooks_;
    BrokerLimits limits_;
    LeaseId next_lease_{1U};
    std::map<InstanceKey, Instance> instances_;
    std::map<LeaseId, LeaseInfo> leases_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_BROKER_CORE_H
