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
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/incarnation_header.h"

#include <fidl/score.mw.com.test.broker/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/process.h>
#include <zircon/syscalls.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

// Proxy: Find/Watch for an epoch it has not attached to yet, Attach over the routed Endpoint, validate the incarnation
// headers, take a lease at the broker, and later report how it noticed the death of the skeleton. The data region is
// kept mapped after the skeleton died (stale, readable); the next Attach replaces it by the new incarnation.

namespace
{
using namespace broker_test;
using score::memory::shared::ISharedMemoryResource;
using score::memory::shared::fuchsia::VmoSharedMemoryFactory;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleClient;
using score::mw::com::impl::lola::fuchsia::IncarnationHeader;
using score::mw::com::impl::lola::fuchsia::IsIncarnationFenced;
using score::mw::com::impl::lola::fuchsia::IsIncarnationLive;

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

std::uint64_t NowUs()
{
    return static_cast<std::uint64_t>(zx_clock_get_monotonic() / 1000);
}

struct Attached
{
    std::shared_ptr<FidlVmoHandleClient> client;
    std::shared_ptr<ISharedMemoryResource> data;
    std::shared_ptr<ISharedMemoryResource> control;
    zx::eventpair fenced;
    zx::eventpair liveness;
    std::uint64_t epoch{0U};
    std::uint64_t lease{0U};

    const IncarnationHeader* data_header() const
    {
        return static_cast<const IncarnationHeader*>(data->getUsableBaseAddress());
    }
    const IncarnationHeader* control_header() const
    {
        return static_cast<const IncarnationHeader*>(control->getUsableBaseAddress());
    }
    std::uint64_t value() const
    {
        return reinterpret_cast<const DataRegion*>(data_header() + 1)->value;
    }
};

class Proxy final : public fidl::Server<score_mw_com_test_broker::Proxy>
{
  public:
    explicit Proxy(fidl::SyncClient<score_mw_com::Broker> broker, fidl::SyncClient<score_mw_com::ServiceDiscovery> discovery)
        : broker_{std::move(broker)}, discovery_{std::move(discovery)}
    {
    }

    void Attach(AttachCompleter::Sync& completer) override
    {
        completer.Reply({AttachInternal()});
    }

    void WaitFenced(WaitFencedRequest& request, WaitFencedCompleter::Sync& completer) override
    {
        completer.Reply({WaitFencedInternal(request.timeout_ms())});
    }

  private:
    std::optional<std::uint64_t> Discover()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!search_.has_value())
            {
                auto endpoints = fidl::CreateEndpoints<score_mw_com::Search>();
                score_mw_com::InstanceFilter filter;
                filter.service_id(kServiceId);
                if (endpoints.is_error() || discovery_->StartFind({filter, std::move(endpoints->server)}).is_error())
                {
                    return std::nullopt;
                }
                search_.emplace(std::move(endpoints->client));
            }
            auto reply = (*search_)->Watch();
            if (reply.is_error())
            {
                search_.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            for (const auto& instance : reply->instances())
            {
                if (instance.epoch() != 0U && instance.epoch() != last_epoch_)
                {
                    return instance.epoch();
                }
            }
        }
        return std::nullopt;
    }

    score_mw_com_test_broker::AttachResult AttachInternal()
    {
        score_mw_com_test_broker::AttachResult result{};
        const auto start = NowUs();
        const auto discovered = Discover();
        if (!discovered.has_value())
        {
            std::printf("proxy: discovery found no new epoch\n");
            return result;
        }
        result.discovery_epoch() = *discovered;

        auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
        if (endpoints.is_error() || component::Connect(std::move(endpoints->server), "/svc/score.mw.com.Endpoint").is_error())
        {
            return result;
        }
        // The registry rejects a second resource under the same path, so the old mappings are dropped before the new Open;
        // the old lease is still held until the new one is registered.
        std::uint64_t old_lease = 0U;
        if (current_.has_value())
        {
            old_lease = current_->lease;
            current_->data.reset();
            current_->control.reset();
            current_->client.reset();
        }
        Attached next;
        next.client = std::make_shared<FidlVmoHandleClient>(std::move(endpoints->client));
        VmoSharedMemoryFactory factory{next.client};
        next.data = factory.Open(kDataPath, false, std::nullopt);
        next.control = factory.Open(kControlPath, true, std::nullopt);
        if (next.data == nullptr || next.control == nullptr || !next.client->attach_info().has_value())
        {
            std::printf("proxy: Attach failed\n");
            return result;
        }
        next.epoch = next.client->attach_info()->epoch;
        result.epoch() = next.epoch;
        result.data_header_epoch() = next.data_header()->epoch.load();
        result.control_header_epoch() = next.control_header()->epoch.load();
        result.data_value() = next.value();
        if (next.epoch != *discovered || !IsIncarnationLive(*next.data_header(), next.epoch) ||
            !IsIncarnationLive(*next.control_header(), next.epoch))
        {
            std::printf("proxy: epoch/header mismatch (discovery %lu, attach %lu)\n",
                        static_cast<unsigned long>(*discovered),
                        static_cast<unsigned long>(next.epoch));
            return result;
        }

        zx::eventpair remote;
        zx::eventpair remote_for_broker;
        zx::process self;
        if (zx::eventpair::create(0U, &next.liveness, &remote) != ZX_OK ||
            remote.duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &remote_for_broker) != ZX_OK ||
            zx::process::self()->duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &self) != ZX_OK)
        {
            return result;
        }
        auto registered = broker_->RegisterProxy({InstanceId(), next.epoch, kProxyAppId, std::move(remote_for_broker), std::move(self)});
        if (registered.is_error())
        {
            std::printf("proxy: RegisterProxy refused\n");
            return result;
        }
        next.lease = registered->proxy_lease();
        next.fenced = std::move(registered->fenced());
        result.proxy_lease() = next.lease;

        // Only now the previous incarnation is let go (its mappings and its lease).
        if (current_.has_value())
        {
            result.released_lease() = old_lease;
            broker_->ReleaseLease({old_lease});
        }
        current_ = std::move(next);
        last_epoch_ = current_->epoch;
        result.ok() = true;
        result.attach_us() = NowUs() - start;
        return result;
    }

    score_mw_com_test_broker::FenceResult WaitFencedInternal(const std::uint32_t timeout_ms)
    {
        score_mw_com_test_broker::FenceResult result{};
        if (!current_.has_value())
        {
            result.timed_out() = true;
            return result;
        }
        const auto start = NowUs();
        zx_wait_item_t items[2] = {
            {current_->client->liveness().get(), ZX_EVENTPAIR_PEER_CLOSED, 0U},
            {current_->fenced.get(), ZX_EVENTPAIR_PEER_CLOSED | ZX_USER_SIGNAL_0, 0U},
        };
        const zx_status_t status = zx_object_wait_many(items, 2, zx_deadline_after(ZX_MSEC(timeout_ms)));
        result.detect_us() = NowUs() - start;
        if (status != ZX_OK)
        {
            result.timed_out() = true;
            return result;
        }
        result.skeleton_liveness_closed() = (items[0].pending & ZX_EVENTPAIR_PEER_CLOSED) != 0U;
        // The broker tombstones before it signals; give it up to 2 s after the skeleton liveness fired.
        zx_signals_t observed = 0U;
        result.fenced_signalled() =
            current_->fenced.wait_one(ZX_USER_SIGNAL_0, zx::deadline_after(zx::sec(2)), &observed) == ZX_OK;
        result.control_tombstoned() = IsIncarnationFenced(*current_->control_header());
        result.tombstone_us() = NowUs() - start;
        result.data_header_epoch_after() = current_->data_header()->epoch.load();
        result.data_value_after() = current_->value();
        return result;
    }

    fidl::SyncClient<score_mw_com::Broker> broker_;
    fidl::SyncClient<score_mw_com::ServiceDiscovery> discovery_;
    std::optional<fidl::SyncClient<score_mw_com::Search>> search_;
    std::optional<Attached> current_;
    std::uint64_t last_epoch_{0U};
};
}  // namespace

int main()
{
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};

    auto broker_end = component::Connect<score_mw_com::Broker>("/svc/score.mw.com.Broker");
    auto discovery_end = component::Connect<score_mw_com::ServiceDiscovery>("/svc/score.mw.com.ServiceDiscovery");
    if (broker_end.is_error() || discovery_end.is_error())
    {
        return 1;
    }
    Proxy proxy{fidl::SyncClient<score_mw_com::Broker>{std::move(*broker_end)},
                fidl::SyncClient<score_mw_com::ServiceDiscovery>{std::move(*discovery_end)}};
    fidl::ServerBindingGroup<score_mw_com_test_broker::Proxy> bindings;
    if (outgoing
            .AddUnmanagedProtocol<score_mw_com_test_broker::Proxy>(
                [&](fidl::ServerEnd<score_mw_com_test_broker::Proxy> end) {
                    bindings.AddBinding(loop.dispatcher(), std::move(end), &proxy, fidl::kIgnoreBindingClosure);
                })
            .is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        return 2;
    }
    loop.Run();
    return 0;
}
