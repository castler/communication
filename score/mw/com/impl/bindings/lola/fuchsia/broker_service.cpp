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
#include "score/mw/com/impl/bindings/lola/fuchsia/broker_service.h"

#include "score/mw/com/impl/bindings/lola/fuchsia/incarnation_header.h"
#include "score/mw/log/logging.h"

#include <lib/async/cpp/task.h>
#include <lib/zx/vmar.h>
#include <zircon/syscalls.h>

#include <string>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr const char* kLogContext = "lola_broker";

InstanceKey ToKey(const score_mw_com::ServiceInstanceId& id)
{
    return InstanceKey{id.service_id(), id.instance_id(), id.major_version(), id.minor_version()};
}

score_mw_com::BrokerError ToError(const BrokerStatus status)
{
    switch (status)
    {
        case BrokerStatus::kAlreadyProvisioned:
            return score_mw_com::BrokerError::kAlreadyProvisioned;
        case BrokerStatus::kResourceExhausted:
            return score_mw_com::BrokerError::kResourceExhausted;
        case BrokerStatus::kStaleEpoch:
            return score_mw_com::BrokerError::kStaleEpoch;
        case BrokerStatus::kOk:
        case BrokerStatus::kNotActive:
        case BrokerStatus::kUnknownLease:
        default:
            return score_mw_com::BrokerError::kNotActive;
    }
}

/// Maps `vmo` and checks the incarnation header at `offset` against `epoch`. On success `mapping`/`size` are set and
/// the caller owns the mapping.
bool MapAndCheckHeader(const zx::vmo& vmo,
                       const bool writable,
                       const std::uint64_t offset,
                       const Epoch epoch,
                       zx_vaddr_t& mapping,
                       std::size_t& size)
{
    std::uint64_t vmo_size{0U};
    if (vmo.get_size(&vmo_size) != ZX_OK || offset % alignof(IncarnationHeader) != 0U ||
        offset + sizeof(IncarnationHeader) > vmo_size)
    {
        return false;
    }
    const zx_vm_option_t options = ZX_VM_PERM_READ | (writable ? ZX_VM_PERM_WRITE : 0U);
    if (zx::vmar::root_self()->map(options, 0U, vmo, 0U, vmo_size, &mapping) != ZX_OK)
    {
        return false;
    }
    size = vmo_size;
    const auto& header = *reinterpret_cast<const IncarnationHeader*>(mapping + offset);
    if (!IsIncarnationLive(header, epoch))
    {
        zx::vmar::root_self()->unmap(mapping, size);
        mapping = 0U;
        return false;
    }
    return true;
}

}  // namespace

BrokerService::BrokerService(async_dispatcher_t* dispatcher, BrokerLimits limits)
    : dispatcher_{dispatcher},
      core_{[] { return static_cast<std::uint64_t>(zx_clock_get_boot()); },
            BrokerCoreHooks{[this](const InstanceKey& key, Epoch epoch, BrokerFenceReason reason) { OnFence(key, epoch, reason); },
                            [this](const InstanceKey& key, Epoch epoch) { OnRelease(key, epoch); },
                            [this](LeaseId lease, LeaseKind kind) { OnLeaseEnd(lease, kind); }},
            limits}
{
}

BrokerService::~BrokerService()
{
    for (auto& entry : retained_)
    {
        if (entry.second.control_mapping != 0U)
        {
            zx::vmar::root_self()->unmap(entry.second.control_mapping, entry.second.control_size);
        }
    }
}

fidl::ProtocolHandler<score_mw_com::Broker> BrokerService::CreateHandler()
{
    return bindings_.CreateHandler(this, dispatcher_, fidl::kIgnoreBindingClosure);
}

void BrokerService::SetLeaseEndObserver(std::function<void(LeaseId, LeaseKind)> observer)
{
    lease_end_observer_ = std::move(observer);
}

void BrokerService::StartWatch(LeaseId lease, LeaseKind kind, zx::eventpair liveness, zx::process process)
{
    auto watch = std::make_unique<Watch>();
    watch->liveness = std::move(liveness);
    watch->process = std::move(process);
    watch->liveness_wait =
        std::make_unique<async::WaitOnce>(watch->liveness.get(), ZX_EVENTPAIR_PEER_CLOSED, 0U);
    watch->liveness_wait->Begin(dispatcher_,
                                [this, lease, kind](async_dispatcher_t*, async::WaitOnce*, zx_status_t status, const zx_packet_signal_t*) {
                                    if (status == ZX_OK)
                                    {
                                        OnHolderGone(lease, kind, BrokerFenceReason::kLivenessClosed);
                                    }
                                });
    if (watch->process.is_valid())
    {
        watch->process_wait = std::make_unique<async::WaitOnce>(watch->process.get(), ZX_PROCESS_TERMINATED, 0U);
        watch->process_wait->Begin(dispatcher_,
                                   [this, lease, kind](async_dispatcher_t*, async::WaitOnce*, zx_status_t status, const zx_packet_signal_t*) {
                                       if (status == ZX_OK)
                                       {
                                           OnHolderGone(lease, kind, BrokerFenceReason::kProcessTerminated);
                                       }
                                   });
    }
    watches_[lease] = std::move(watch);
}

void BrokerService::OnHolderGone(LeaseId lease, LeaseKind kind, BrokerFenceReason reason)
{
    if (kind == LeaseKind::kSkeleton)
    {
        core_.SkeletonLost(lease, reason);
    }
    else
    {
        core_.ProxyEnded(lease);
    }
}

void BrokerService::OnLeaseEnd(LeaseId lease, LeaseKind kind)
{
    // Destroying the waits from within their own handler is avoided by deferring.
    async::PostTask(dispatcher_, [this, lease] { watches_.erase(lease); });
    if (kind == LeaseKind::kProxy)
    {
        const auto owner = proxy_incarnation_.find(lease);
        if (owner != proxy_incarnation_.end())
        {
            const auto retained = retained_.find(owner->second);
            if (retained != retained_.end())
            {
                retained->second.fenced.erase(lease);
            }
            proxy_incarnation_.erase(owner);
        }
    }
    if (lease_end_observer_)
    {
        lease_end_observer_(lease, kind);
    }
}

void BrokerService::OnFence(const InstanceKey& key, Epoch epoch, BrokerFenceReason reason)
{
    score::mw::log::LogInfo(kLogContext) << "Fencing epoch " << epoch << " of instance " << key.instance_id << " (reason "
                                         << static_cast<int>(reason) << ").";
    const auto found = retained_.find({key, epoch});
    if (found == retained_.end())
    {
        return;
    }
    auto& retained = found->second;
    // Tombstone first, then the signal: a proxy woken by USER_0 must already see the header fenced.
    if (retained.control_mapping != 0U)
    {
        auto& header = *reinterpret_cast<IncarnationHeader*>(retained.control_mapping + retained.control_header_offset);
        FenceIncarnation(header);
    }
    for (auto& local : retained.fenced)
    {
        local.second.signal_peer(0U, ZX_USER_SIGNAL_0);
    }
    retained.fenced.clear();
}

void BrokerService::OnRelease(const InstanceKey& key, Epoch epoch)
{
    const auto found = retained_.find({key, epoch});
    if (found == retained_.end())
    {
        return;
    }
    if (found->second.control_mapping != 0U)
    {
        zx::vmar::root_self()->unmap(found->second.control_mapping, found->second.control_size);
    }
    retained_.erase(found);
}

void BrokerService::BeginIncarnation(BeginIncarnationRequest& request, BeginIncarnationCompleter::Sync& completer)
{
    Epoch epoch{};
    LeaseId lease{};
    const auto status = core_.BeginIncarnation(ToKey(request.instance()), epoch, lease);
    if (status != BrokerStatus::kOk)
    {
        completer.Reply(fit::error(ToError(status)));
        return;
    }
    StartWatch(lease, LeaseKind::kSkeleton, std::move(request.liveness()), std::move(request.process()));
    completer.Reply(fit::ok(score_mw_com::BrokerBeginIncarnationResponse{epoch, lease}));
}

bool BrokerService::VerifyRegions(zx::vmo& data,
                                  zx::vmo& control,
                                  const std::uint64_t data_offset,
                                  const std::uint64_t control_offset,
                                  const Epoch epoch,
                                  Retained& retained)
{
    zx_vaddr_t data_mapping{0U};
    std::size_t data_size{0U};
    const bool data_ok = MapAndCheckHeader(data, false, data_offset, epoch, data_mapping, data_size);
    if (data_ok)
    {
        zx::vmar::root_self()->unmap(data_mapping, data_size);
    }
    if (!data_ok ||
        !MapAndCheckHeader(control, true, control_offset, epoch, retained.control_mapping, retained.control_size))
    {
        return false;
    }
    retained.data = std::move(data);
    retained.control = std::move(control);
    retained.control_header_offset = control_offset;
    return true;
}

void BrokerService::Publish(PublishRequest& request, PublishCompleter::Sync& completer)
{
    const InstanceKey key = ToKey(request.instance());
    const Epoch epoch = request.epoch();
    if (retained_.count({key, epoch}) != 0U)
    {
        completer.Reply(fit::error(score_mw_com::BrokerError::kAlreadyProvisioned));
        return;
    }
    Retained retained;
    if (!VerifyRegions(request.data(),
                       request.control(),
                       request.data_header_offset(),
                       request.control_header_offset(),
                       epoch,
                       retained))
    {
        completer.Reply(fit::error(score_mw_com::BrokerError::kInvalidRegion));
        return;
    }
    const auto status = core_.Publish(key, epoch);
    if (status != BrokerStatus::kOk)
    {
        zx::vmar::root_self()->unmap(retained.control_mapping, retained.control_size);
        completer.Reply(fit::error(ToError(status)));
        return;
    }
    retained_.emplace(RetainedKey{key, epoch}, std::move(retained));
    completer.Reply(fit::ok());
}

void BrokerService::Reannounce(ReannounceRequest& request, ReannounceCompleter::Sync& completer)
{
    const InstanceKey key = ToKey(request.instance());
    const Epoch epoch = request.epoch();
    if (retained_.count({key, epoch}) != 0U)
    {
        completer.Reply(fit::error(score_mw_com::BrokerError::kAlreadyProvisioned));
        return;
    }
    Retained retained;
    if (!VerifyRegions(request.data(),
                       request.control(),
                       request.data_header_offset(),
                       request.control_header_offset(),
                       epoch,
                       retained))
    {
        completer.Reply(fit::error(score_mw_com::BrokerError::kInvalidRegion));
        return;
    }
    LeaseId lease{};
    const auto status = core_.AdoptIncarnation(key, epoch, lease);
    if (status != BrokerStatus::kOk)
    {
        zx::vmar::root_self()->unmap(retained.control_mapping, retained.control_size);
        completer.Reply(fit::error(ToError(status)));
        return;
    }
    retained_.emplace(RetainedKey{key, epoch}, std::move(retained));
    StartWatch(lease, LeaseKind::kSkeleton, std::move(request.liveness()), std::move(request.process()));
    score::mw::log::LogInfo(kLogContext) << "Adopted epoch " << epoch << " of instance " << key.instance_id << ".";
    completer.Reply(fit::ok(score_mw_com::BrokerReannounceResponse{lease}));
}

void BrokerService::Resign(ResignRequest& request, ResignCompleter::Sync& completer)
{
    core_.Resign(ToKey(request.instance()), request.epoch());
    completer.Reply();
}

void BrokerService::RegisterProxy(RegisterProxyRequest& request, RegisterProxyCompleter::Sync& completer)
{
    const InstanceKey key = ToKey(request.instance());
    LeaseId lease{};
    const auto status = core_.RegisterProxy(key, request.epoch(), lease);
    if (status != BrokerStatus::kOk)
    {
        completer.Reply(fit::error(ToError(status)));
        return;
    }
    zx::eventpair local;
    zx::eventpair remote;
    if (zx::eventpair::create(0U, &local, &remote) != ZX_OK)
    {
        core_.ProxyEnded(lease);
        completer.Reply(fit::error(score_mw_com::BrokerError::kResourceExhausted));
        return;
    }
    retained_.at({key, request.epoch()}).fenced.emplace(lease, std::move(local));
    proxy_incarnation_.emplace(lease, RetainedKey{key, request.epoch()});
    StartWatch(lease, LeaseKind::kProxy, std::move(request.liveness()), std::move(request.process()));
    completer.Reply(fit::ok(score_mw_com::BrokerRegisterProxyResponse{lease, std::move(remote)}));
}

void BrokerService::ReleaseLease(ReleaseLeaseRequest& request, ReleaseLeaseCompleter::Sync& completer)
{
    core_.ProxyEnded(request.proxy_lease());
    completer.Reply();
}

void BrokerService::Inspect(InspectRequest& request, InspectCompleter::Sync& completer)
{
    const CoreStats stats = core_.Stats(ToKey(request.instance()));
    score_mw_com::BrokerStats result{};
    result.active_epoch() = stats.active_epoch;
    result.last_epoch() = stats.last_epoch;
    result.retained_incarnations() = stats.retained_incarnations;
    result.draining_incarnations() = stats.draining_incarnations;
    result.live_proxy_leases() = stats.live_proxy_leases;
    result.fenced_total() = stats.fenced_total;
    result.released_total() = stats.released_total;
    result.last_fence_reason() = score_mw_com::FenceReason(static_cast<std::uint8_t>(stats.last_fence_reason));
    result.fenced_by_process() = stats.fenced_by_process;
    result.fenced_by_liveness() = stats.fenced_by_liveness;
    completer.Reply(std::move(result));
}

}  // namespace score::mw::com::impl::lola::fuchsia
