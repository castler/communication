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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_BROKER_SERVICE_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_BROKER_SERVICE_H

#include "score/mw/com/impl/bindings/lola/fuchsia/broker_core.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/process.h>
#include <lib/zx/vmo.h>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief `score.mw.com.Broker`: the Fuchsia shell around BrokerCore.
/// \details Retains the data and control VMO of every incarnation (a new pair per incarnation, never reused), watches
///          each skeleton and proxy lease for termination (peer-closed of the liveness eventpair and/or
///          ZX_PROCESS_TERMINATED of the process handle the holder sent: a broker cannot obtain a peer's process
///          handle by itself), and on skeleton loss writes the tombstone into the control header and signals every
///          proxy's `fenced` eventpair, then releases the VMOs once no proxy lease pins the incarnation any more.
///
///          Single threaded: everything (handlers, waits, tasks) runs on `dispatcher`. Broker restart is not
///          implemented: the state is soft, a restarted broker is empty (t03 4.6, "re-announce" is future work).
class BrokerService final : public fidl::Server<score_mw_com::Broker>
{
  public:
    explicit BrokerService(async_dispatcher_t* dispatcher, BrokerLimits limits = {});
    ~BrokerService() override;

    BrokerService(const BrokerService&) = delete;
    BrokerService& operator=(const BrokerService&) = delete;

    fidl::ProtocolHandler<score_mw_com::Broker> CreateHandler();

    /// \brief Lease-end notifications for other resources keyed by lease id (the w01 lock registry idea: locks are
    ///        released when the lease of their holder ends, not by pid or file).
    void SetLeaseEndObserver(std::function<void(LeaseId, LeaseKind)> observer);

    void BeginIncarnation(BeginIncarnationRequest& request, BeginIncarnationCompleter::Sync& completer) override;
    void Publish(PublishRequest& request, PublishCompleter::Sync& completer) override;
    void Resign(ResignRequest& request, ResignCompleter::Sync& completer) override;
    void RegisterProxy(RegisterProxyRequest& request, RegisterProxyCompleter::Sync& completer) override;
    void ReleaseLease(ReleaseLeaseRequest& request, ReleaseLeaseCompleter::Sync& completer) override;
    void Inspect(InspectRequest& request, InspectCompleter::Sync& completer) override;

  private:
    struct Watch
    {
        zx::eventpair liveness;
        zx::process process;
        std::unique_ptr<async::WaitOnce> liveness_wait;
        std::unique_ptr<async::WaitOnce> process_wait;
    };
    struct Retained
    {
        zx::vmo data;
        zx::vmo control;
        zx_vaddr_t control_mapping{0U};
        std::size_t control_size{0U};
        std::uint64_t control_header_offset{0U};
        std::vector<zx::eventpair> fenced;
    };
    using RetainedKey = std::pair<InstanceKey, Epoch>;

    void StartWatch(LeaseId lease, LeaseKind kind, zx::eventpair liveness, zx::process process);
    void OnHolderGone(LeaseId lease, LeaseKind kind, BrokerFenceReason reason);
    void OnFence(const InstanceKey& key, Epoch epoch, BrokerFenceReason reason);
    void OnRelease(const InstanceKey& key, Epoch epoch);
    void OnLeaseEnd(LeaseId lease, LeaseKind kind);

    async_dispatcher_t* dispatcher_;
    BrokerCore core_;
    fidl::ServerBindingGroup<score_mw_com::Broker> bindings_;
    std::map<LeaseId, std::unique_ptr<Watch>> watches_;
    std::map<RetainedKey, Retained> retained_;
    std::function<void(LeaseId, LeaseKind)> lease_end_observer_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_BROKER_SERVICE_H
