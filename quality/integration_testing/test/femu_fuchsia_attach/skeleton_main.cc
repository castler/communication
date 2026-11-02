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
#include "quality/integration_testing/test/femu_fuchsia_attach/test_topology.h"
#include "score/memory/shared/fuchsia/vmo_shared_memory_factory.h"
#include "score/memory/shared/shared_memory_factory.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <set>
#include <string>

// Skeleton: creates the data and the control region through the VmoSharedMemoryFactory (whose handle provider is the
// FIDL server), serves the Endpoint of the proxy application only, and offers the instance at the registry after a
// delay so that the proxy's Search.Watch is a real hanging get.

namespace
{
using namespace attach_test;
using score::memory::shared::ISharedMemoryResource;
using score::memory::shared::fuchsia::VmoSharedMemoryFactory;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleServer;
using score::mw::com::impl::lola::fuchsia::IsConsumerWritable;

struct DataRegion
{
    std::uint32_t magic;
    std::atomic<std::uint64_t> counter;
};
}  // namespace

int main()
{
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};

    auto server = std::make_shared<FidlVmoHandleServer>(loop.dispatcher());
    server->SetEpoch(kEpoch);
    server->SetLayout({1U, 2U, 3U});
    server->SetAllowedApplications(std::set<std::uint64_t>{kProxyAppId});

    VmoSharedMemoryFactory factory{server, IsConsumerWritable};
    score::memory::shared::SharedMemoryFactory::InjectMock(&factory);
    auto data = score::memory::shared::SharedMemoryFactory::Create(
        kDataPath,
        [](std::shared_ptr<ISharedMemoryResource> r) {
            auto* region = r->construct<DataRegion>();
            region->magic = kMagic;
            region->counter = kCounter;
        },
        kRegionSize);
    auto control = score::memory::shared::SharedMemoryFactory::Create(
        kControlPath, [](std::shared_ptr<ISharedMemoryResource>) {}, kRegionSize);
    if (data == nullptr || control == nullptr)
    {
        std::printf("skeleton: region creation failed\n");
        return 1;
    }

    // Only the proxy's endpoint exists; there is deliberately no endpoint for the rogue application.
    if (outgoing.AddUnmanagedProtocol<score_mw_com::Endpoint>(server->CreateHandler(kProxyAppId),
                                                              "score.mw.com.Endpoint.for_app_" + std::to_string(kProxyAppId))
            .is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        return 2;
    }

    auto registry_end = component::Connect<score_mw_com::ServiceRegistry>(
        "/svc/score.mw.com.ServiceRegistry");
    if (registry_end.is_error())
    {
        return 3;
    }
    fidl::Client<score_mw_com::ServiceRegistry> registry{std::move(*registry_end), loop.dispatcher()};

    async::PostDelayedTask(
        loop.dispatcher(),
        [&registry] {
            score_mw_com::ServiceInstanceId id;
            id.service_id() = kServiceId;
            id.instance_id() = kInstanceId;
            id.major_version() = 1U;
            id.minor_version() = 0U;
            registry->Offer({id, score_mw_com::Quality::kQm, kEpoch})
                .Then([](fidl::Result<score_mw_com::ServiceRegistry::Offer>& result) {
                    std::printf("skeleton: Offer %s\n", result.is_ok() ? "ok" : "FAILED");
                });
        },
        zx::sec(kOfferDelaySeconds));

    std::printf("skeleton: running\n");
    loop.Run();
    return 0;
}
