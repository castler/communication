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
#include "quality/integration_testing/test/femu_fuchsia_broker/test_topology.h"
#include "score/memory/shared/fuchsia/vmo_shared_memory_factory.h"
#include "score/memory/shared/shared_memory_factory.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/incarnation_header.h"

#include <fidl/score.mw.com.test.broker/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/process.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>

// Skeleton: one incarnation per process start (README 6.5). BeginIncarnation at the broker (epoch, lease), regions with
// the incarnation header, Publish (the broker verifies the headers), Endpoint for the proxy application, Offer with the
// broker's epoch. Test hook: Skeleton.Crash kills the process by zx_process_exit or by a trap.

namespace
{
using namespace broker_test;
using score::memory::shared::ISharedMemoryResource;
using score::memory::shared::fuchsia::VmoSharedMemoryFactory;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleServer;
using score::mw::com::impl::lola::fuchsia::IncarnationHeader;
using score::mw::com::impl::lola::fuchsia::IsConsumerWritable;
using score::mw::com::impl::lola::fuchsia::PublishIncarnation;
using score::mw::com::impl::lola::fuchsia::VmoRole;

struct DataRegion
{
    std::uint64_t value;
};

score_mw_com::ServiceInstanceId InstanceId()
{
    score_mw_com::ServiceInstanceId id;
    id.service_id() = kServiceId;
    id.instance_id() = kInstanceId;
    id.major_version() = 1U;
    id.minor_version() = 0U;
    return id;
}

class Control final : public fidl::Server<score_mw_com_test_broker::Skeleton>
{
  public:
    explicit Control(std::uint64_t epoch) : epoch_{epoch} {}

    void Epoch(EpochCompleter::Sync& completer) override
    {
        completer.Reply({epoch_});
    }

    void Crash(CrashRequest& request, CrashCompleter::Sync&) override
    {
        std::printf("skeleton: crash requested (%s)\n", request.mode() == score_mw_com_test_broker::CrashMode::kExit ? "exit" : "trap");
        if (request.mode() == score_mw_com_test_broker::CrashMode::kExit)
        {
            zx_process_exit(42);
        }
        __builtin_trap();
    }

  private:
    std::uint64_t epoch_;
};
}  // namespace

int main()
{
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};

    auto broker_end = component::Connect<score_mw_com::Broker>("/svc/score.mw.com.Broker");
    if (broker_end.is_error())
    {
        return 1;
    }
    fidl::SyncClient<score_mw_com::Broker> broker{std::move(*broker_end)};

    // Liveness: the broker holds `remote`; it closes when this process dies. The process handle is the second, slower
    // but crash-aware signal.
    zx::eventpair liveness;
    zx::eventpair liveness_remote;
    if (zx::eventpair::create(0U, &liveness, &liveness_remote) != ZX_OK)
    {
        return 2;
    }
    // The previous incarnation of a crashed skeleton may not have been noticed by the broker yet: retry.
    std::uint64_t epoch{0U};
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        zx::eventpair remote_copy;
        zx::process self_copy;
        if (liveness_remote.duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &remote_copy) != ZX_OK ||
            zx::process::self()->duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &self_copy) != ZX_OK)
        {
            return 2;
        }
        auto begun = broker->BeginIncarnation({InstanceId(), std::move(remote_copy), std::move(self_copy)});
        if (begun.is_ok())
        {
            epoch = begun->epoch();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (epoch == 0U)
    {
        std::printf("skeleton: BeginIncarnation failed\n");
        return 3;
    }
    liveness_remote.reset();

    auto server = std::make_shared<FidlVmoHandleServer>(loop.dispatcher());
    server->SetEpoch(epoch);
    server->SetLayout({1U, 2U, 3U});
    server->SetAllowedApplications(std::set<std::uint64_t>{kProxyAppId});

    VmoSharedMemoryFactory factory{server, IsConsumerWritable};
    score::memory::shared::SharedMemoryFactory::InjectMock(&factory);
    std::uint64_t data_offset{0U};
    std::uint64_t control_offset{0U};
    auto offset_of = [](ISharedMemoryResource& resource, const void* object) {
        return static_cast<std::uint64_t>(static_cast<const char*>(object) - static_cast<const char*>(resource.getBaseAddress()));
    };
    auto data = score::memory::shared::SharedMemoryFactory::Create(
        kDataPath,
        [&](std::shared_ptr<ISharedMemoryResource> r) {
            auto* header = r->construct<IncarnationHeader>();
            auto* region = r->construct<DataRegion>();
            region->value = epoch ^ kValueMask;
            data_offset = offset_of(*r, header);
            PublishIncarnation(*header, epoch);  // last write
        },
        kRegionSize);
    auto control = score::memory::shared::SharedMemoryFactory::Create(
        kControlPath,
        [&](std::shared_ptr<ISharedMemoryResource> r) {
            auto* header = r->construct<IncarnationHeader>();
            control_offset = offset_of(*r, header);
            PublishIncarnation(*header, epoch);
        },
        kRegionSize);
    if (data == nullptr || control == nullptr)
    {
        std::printf("skeleton: region creation failed\n");
        return 4;
    }

    auto data_for_broker = server->DuplicateForBroker(VmoRole::kData);
    auto control_for_broker = server->DuplicateForBroker(VmoRole::kControl);
    if (!data_for_broker.has_value() || !control_for_broker.has_value())
    {
        return 5;
    }
    auto published = broker->Publish({InstanceId(),
                                      epoch,
                                      std::move(*data_for_broker),
                                      std::move(*control_for_broker),
                                      data_offset,
                                      control_offset});
    if (published.is_error())
    {
        std::printf("skeleton: Publish failed\n");
        return 6;
    }

    Control control_impl{epoch};
    fidl::ServerBindingGroup<score_mw_com_test_broker::Skeleton> bindings;
    if (outgoing.AddUnmanagedProtocol<score_mw_com::Endpoint>(server->CreateHandler(kProxyAppId),
                                                              "score.mw.com.Endpoint.for_app_" + std::to_string(kProxyAppId))
            .is_error() ||
        outgoing
            .AddUnmanagedProtocol<score_mw_com_test_broker::Skeleton>(
                [&](fidl::ServerEnd<score_mw_com_test_broker::Skeleton> end) {
                    bindings.AddBinding(loop.dispatcher(), std::move(end), &control_impl, fidl::kIgnoreBindingClosure);
                })
            .is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        return 7;
    }

    auto registry_end = component::Connect<score_mw_com::ServiceRegistry>("/svc/score.mw.com.ServiceRegistry");
    if (registry_end.is_error())
    {
        return 8;
    }
    fidl::SyncClient<score_mw_com::ServiceRegistry> registry{std::move(*registry_end)};
    auto offered = registry->Offer({InstanceId(), score_mw_com::Quality::kQm, epoch});
    std::printf("skeleton: epoch %lu, Offer %s\n", static_cast<unsigned long>(epoch), offered.is_ok() ? "ok" : "FAILED");

    // `liveness` stays alive until the process ends: its peer-closed is the death signal the broker waits for.
    loop.Run();
    return 0;
}
