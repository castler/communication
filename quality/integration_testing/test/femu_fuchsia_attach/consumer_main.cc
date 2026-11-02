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
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"

#include <fidl/score.mw.com.test/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

// Consumer (argument "proxy" or "rogue"): Find, StartFind + Watch, Attach, read. The same binary runs as the routed
// proxy and as the third, not routed consumer; the realm decides what each of them can reach. The driver fetches the
// outcome through the Probe protocol.

namespace
{
using namespace attach_test;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleClient;
using score::mw::com::impl::lola::fuchsia::kControlRights;
using score::mw::com::impl::lola::fuchsia::kDataRights;

struct DataRegion
{
    std::uint32_t magic;
    std::atomic<std::uint64_t> counter;
};

score_mw_com::InstanceFilter Filter()
{
    score_mw_com::InstanceFilter filter;
    filter.service_id(kServiceId);
    return filter;
}

std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> ConnectEndpoint()
{
    auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
    if (endpoints.is_error())
    {
        return std::nullopt;
    }
    if (component::Connect(std::move(endpoints->server), "/svc/score.mw.com.Endpoint").is_error())
    {
        return std::nullopt;
    }
    return std::move(endpoints->client);
}

class Probe final : public fidl::Server<score_mw_com_test::Probe>
{
  public:
    explicit Probe(bool is_proxy) : is_proxy_{is_proxy} {}

    void Run(RunCompleter::Sync& completer) override
    {
        completer.Reply(RunInternal());
    }

  private:
    score_mw_com_test::ProbeResult RunInternal()
    {
        score_mw_com_test::ProbeResult result{};
        std::uint64_t discovery_epoch = 0U;

        auto discovery_end = component::Connect<score_mw_com::ServiceDiscovery>("/svc/score.mw.com.ServiceDiscovery");
        if (discovery_end.is_error())
        {
            std::printf("consumer: no discovery\n");
            return result;
        }
        fidl::SyncClient<score_mw_com::ServiceDiscovery> discovery{std::move(*discovery_end)};

        auto found = discovery->Find({Filter()});
        result.find_count() = found.is_ok() ? static_cast<std::uint32_t>(found->instances().size()) : 0xffffU;

        auto search = fidl::CreateEndpoints<score_mw_com::Search>();
        if (search.is_error() || discovery->StartFind({Filter(), std::move(search->server)}).is_error())
        {
            std::printf("consumer: StartFind failed\n");
            return result;
        }
        fidl::SyncClient<score_mw_com::Search> watch{std::move(search->client)};
        auto first = watch->Watch();
        result.first_watch_count() = first.is_ok() ? static_cast<std::uint32_t>(first->instances().size()) : 0xffffU;
        auto current = std::move(first);
        if (is_proxy_)
        {
            // The hanging get returns when the skeleton offers.
            while (current.is_ok() && current->instances().empty())
            {
                current = watch->Watch();
            }
        }
        else
        {
            // Not allowed to see the service: the snapshot stays empty even after the skeleton offered.
            std::this_thread::sleep_for(std::chrono::seconds(kOfferDelaySeconds + 3));
            auto again = discovery->Find({Filter()});
            result.find_count() = again.is_ok() ? static_cast<std::uint32_t>(again->instances().size()) : 0xffffU;
        }
        if (current.is_ok())
        {
            result.watch_count() = static_cast<std::uint32_t>(current->instances().size());
            if (!current->instances().empty())
            {
                discovery_epoch = current->instances()[0].epoch();
            }
        }
        result.discovery_epoch() = discovery_epoch;

        // Attach #1: raw handles, rights as they arrived.
        auto endpoint = ConnectEndpoint();
        if (!endpoint.has_value())
        {
            result.attach_errno() = ENOENT;
            return result;
        }
        FidlVmoHandleClient client{std::move(*endpoint)};
        auto data = client.Acquire(kDataPath);
        if (!data.has_value())
        {
            result.attach_errno() = static_cast<std::int32_t>(data.error().GetOsDependentErrorCode());
            return result;
        }
        auto control = client.Acquire(kControlPath);
        const auto& info = client.attach_info();
        result.data_rights() = info->data_rights;
        result.control_rights() = info->control_rights;
        result.application_id() = info->application_id;
        result.epoch() = info->epoch;

        zx_vaddr_t address = 0U;
        result.data_write_denied() =
            zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0U, data->get(), 0U, kRegionSize, &address) ==
            ZX_ERR_ACCESS_DENIED;
        if (control.has_value() &&
            zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0U, control->get(), 0U, kRegionSize, &address) ==
                ZX_OK)
        {
            auto* word = reinterpret_cast<volatile std::uint32_t*>(address);
            *word = 0xC0FFEEU;
            result.control_write_ok() = (*word == 0xC0FFEEU);
        }

        // Attach #2: through the VmoSharedMemoryFactory seam, as the LoLa bindings use it.
        auto second_end = ConnectEndpoint();
        if (second_end.has_value())
        {
            auto provider = std::make_shared<FidlVmoHandleClient>(std::move(*second_end));
            score::memory::shared::fuchsia::VmoSharedMemoryFactory factory{provider};
            auto resource = factory.Open(kDataPath, false, std::nullopt);
            if (resource != nullptr)
            {
                const auto* region = static_cast<const DataRegion*>(resource->getUsableBaseAddress());
                result.data_read_ok() = (region->magic == kMagic) && (region->counter.load() == kCounter);
            }
        }
        return result;
    }

    bool is_proxy_;
};
}  // namespace

int main(int argc, char** argv)
{
    const bool is_proxy = argc > 1 && std::strcmp(argv[1], "proxy") == 0;
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};
    Probe probe{is_proxy};
    fidl::ServerBindingGroup<score_mw_com_test::Probe> bindings;
    if (outgoing
            .AddUnmanagedProtocol<score_mw_com_test::Probe>(
                [&](fidl::ServerEnd<score_mw_com_test::Probe> server_end) {
                    bindings.AddBinding(loop.dispatcher(), std::move(server_end), &probe, fidl::kIgnoreBindingClosure);
                })
            .is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        return 1;
    }
    loop.Run();
    return 0;
}
