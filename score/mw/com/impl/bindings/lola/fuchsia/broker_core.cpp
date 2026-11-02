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
#include "score/mw/com/impl/bindings/lola/fuchsia/broker_core.h"

#include <algorithm>
#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

BrokerCore::BrokerCore(Clock boot_clock_ns, BrokerCoreHooks hooks, BrokerLimits limits)
    : clock_{std::move(boot_clock_ns)}, hooks_{std::move(hooks)}, limits_{limits}
{
}

std::size_t BrokerCore::Draining(const Instance& instance)
{
    return static_cast<std::size_t>(std::count_if(instance.incarnations.begin(),
                                                  instance.incarnations.end(),
                                                  [](const auto& entry) { return entry.second.state == State::kFenced; }));
}

BrokerStatus BrokerCore::BeginIncarnation(const InstanceKey& key, Epoch& epoch, LeaseId& skeleton_lease)
{
    auto& instance = instances_[key];
    for (const auto& entry : instance.incarnations)
    {
        if (entry.second.state != State::kFenced)
        {
            return BrokerStatus::kAlreadyProvisioned;
        }
    }
    if (Draining(instance) >= limits_.max_draining_per_instance)
    {
        return BrokerStatus::kResourceExhausted;
    }
    epoch = std::max(instance.last_epoch + 1U, clock_());
    instance.last_epoch = epoch;
    skeleton_lease = next_lease_++;
    instance.incarnations[epoch] = Incarnation{State::kProvisioning, skeleton_lease, 0U};
    leases_[skeleton_lease] = LeaseInfo{LeaseKind::kSkeleton, key, epoch};
    return BrokerStatus::kOk;
}

BrokerStatus BrokerCore::Publish(const InstanceKey& key, Epoch epoch)
{
    const auto instance = instances_.find(key);
    if (instance == instances_.end())
    {
        return BrokerStatus::kNotActive;
    }
    const auto incarnation = instance->second.incarnations.find(epoch);
    if (incarnation == instance->second.incarnations.end() || incarnation->second.state == State::kFenced)
    {
        return BrokerStatus::kStaleEpoch;
    }
    incarnation->second.state = State::kActive;
    return BrokerStatus::kOk;
}

void BrokerCore::Resign(const InstanceKey& key, Epoch epoch)
{
    const auto instance = instances_.find(key);
    if (instance != instances_.end() && instance->second.incarnations.count(epoch) != 0U)
    {
        Fence(key, instance->second, epoch, BrokerFenceReason::kResigned);
    }
}

void BrokerCore::SkeletonLost(LeaseId skeleton_lease, BrokerFenceReason reason)
{
    const auto lease = leases_.find(skeleton_lease);
    if (lease == leases_.end() || lease->second.kind != LeaseKind::kSkeleton)
    {
        return;
    }
    const LeaseInfo info = lease->second;
    Fence(info.instance, instances_[info.instance], info.epoch, reason);
}

void BrokerCore::Fence(const InstanceKey& key, Instance& instance, Epoch epoch, BrokerFenceReason reason)
{
    auto& incarnation = instance.incarnations.at(epoch);
    if (incarnation.state == State::kFenced)
    {
        return;
    }
    incarnation.state = State::kFenced;
    ++instance.counters.fenced_total;
    instance.counters.last_fence_reason = reason;
    if (reason == BrokerFenceReason::kProcessTerminated)
    {
        ++instance.counters.fenced_by_process;
    }
    else if (reason == BrokerFenceReason::kLivenessClosed)
    {
        ++instance.counters.fenced_by_liveness;
    }
    if (hooks_.on_fence)
    {
        hooks_.on_fence(key, epoch, reason);
    }
    // The skeleton lease ends with the fence, whichever way it came.
    if (leases_.erase(incarnation.skeleton_lease) != 0U && hooks_.on_lease_end)
    {
        hooks_.on_lease_end(incarnation.skeleton_lease, LeaseKind::kSkeleton);
    }
    ReleaseIfUnpinned(key, instance, epoch);
}

void BrokerCore::ReleaseIfUnpinned(const InstanceKey& key, Instance& instance, Epoch epoch)
{
    const auto incarnation = instance.incarnations.find(epoch);
    if (incarnation == instance.incarnations.end() || incarnation->second.state != State::kFenced ||
        incarnation->second.proxy_leases != 0U)
    {
        return;
    }
    instance.incarnations.erase(incarnation);
    ++instance.counters.released_total;
    if (hooks_.on_release)
    {
        hooks_.on_release(key, epoch);
    }
}

BrokerStatus BrokerCore::RegisterProxy(const InstanceKey& key, Epoch epoch, LeaseId& proxy_lease)
{
    const auto instance = instances_.find(key);
    if (instance == instances_.end())
    {
        return BrokerStatus::kNotActive;
    }
    const auto active = std::find_if(instance->second.incarnations.begin(),
                                     instance->second.incarnations.end(),
                                     [](const auto& entry) { return entry.second.state == State::kActive; });
    if (active == instance->second.incarnations.end())
    {
        return BrokerStatus::kNotActive;
    }
    if (active->first != epoch)
    {
        return BrokerStatus::kStaleEpoch;
    }
    proxy_lease = next_lease_++;
    ++active->second.proxy_leases;
    leases_[proxy_lease] = LeaseInfo{LeaseKind::kProxy, key, epoch};
    return BrokerStatus::kOk;
}

BrokerStatus BrokerCore::ProxyEnded(LeaseId proxy_lease)
{
    const auto lease = leases_.find(proxy_lease);
    if (lease == leases_.end() || lease->second.kind != LeaseKind::kProxy)
    {
        return BrokerStatus::kUnknownLease;
    }
    const LeaseInfo info = lease->second;
    leases_.erase(lease);
    auto& instance = instances_[info.instance];
    --instance.incarnations.at(info.epoch).proxy_leases;
    if (hooks_.on_lease_end)
    {
        hooks_.on_lease_end(proxy_lease, LeaseKind::kProxy);
    }
    ReleaseIfUnpinned(info.instance, instance, info.epoch);
    return BrokerStatus::kOk;
}

CoreStats BrokerCore::Stats(const InstanceKey& key) const
{
    const auto found = instances_.find(key);
    if (found == instances_.end())
    {
        return CoreStats{};
    }
    CoreStats stats = found->second.counters;
    stats.last_epoch = found->second.last_epoch;
    for (const auto& entry : found->second.incarnations)
    {
        stats.live_proxy_leases += static_cast<std::uint32_t>(entry.second.proxy_leases);
        if (entry.second.state == State::kActive)
        {
            stats.active_epoch = entry.first;
        }
        if (entry.second.state != State::kProvisioning)
        {
            ++stats.retained_incarnations;
        }
        if (entry.second.state == State::kFenced)
        {
            ++stats.draining_incarnations;
        }
    }
    return stats;
}

}  // namespace score::mw::com::impl::lola::fuchsia
