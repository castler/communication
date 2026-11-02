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
#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_service.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <chrono>
#include <cstdio>
#include <thread>

// In-process test of the discovery logic: per-caller configuration, snapshots, hanging get, withdraw on disconnect.

namespace
{
using score::mw::com::impl::lola::fuchsia::DiscoveryService;

int failures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

score_mw_com::ServiceInstanceId Id(const std::uint64_t service, const std::uint32_t instance)
{
    score_mw_com::ServiceInstanceId id;
    id.service_id() = service;
    id.instance_id() = instance;
    id.major_version() = 1U;
    id.minor_version() = 0U;
    return id;
}

score_mw_com::InstanceFilter ServiceFilter(const std::uint64_t service)
{
    score_mw_com::InstanceFilter filter;
    filter.service_id(service);
    return filter;
}

template <typename Protocol>
fidl::SyncClient<Protocol> Connect(const fidl::ProtocolHandler<Protocol>& handler)
{
    auto endpoints = fidl::CreateEndpoints<Protocol>();
    handler(std::move(endpoints->server));
    return fidl::SyncClient<Protocol>{std::move(endpoints->client)};
}

std::size_t FindCount(fidl::SyncClient<score_mw_com::ServiceDiscovery>& discovery, const score_mw_com::InstanceFilter& filter)
{
    auto result = discovery->Find({filter});
    return result.is_ok() ? result->instances().size() : 9999U;
}
}  // namespace

int main()
{
    async::Loop loop{&kAsyncLoopConfigNoAttachToCurrentThread};
    DiscoveryService service{loop.dispatcher()};
    // The handlers bind on the caller's thread; with the service running on the loop thread this is only done while
    // the loop is stopped, so start the thread afterwards.
    auto registry_handler = service.RegistryHandler({1000U, {1U}});
    auto consumer_handler = service.DiscoveryHandler({2000U, {1U}});
    auto outsider_handler = service.DiscoveryHandler({3000U, {}});
    auto registry = Connect<score_mw_com::ServiceRegistry>(registry_handler);
    auto consumer = Connect<score_mw_com::ServiceDiscovery>(consumer_handler);
    auto outsider = Connect<score_mw_com::ServiceDiscovery>(outsider_handler);
    loop.StartThread("discovery");

    // Only configured service ids can be offered, and only once.
    auto not_configured = registry->Offer({Id(2U, 1U), score_mw_com::Quality::kQm, 5U});
    CHECK(not_configured.is_error() && not_configured.error_value().is_domain_error() &&
          not_configured.error_value().domain_error() == score_mw_com::OfferError::kNotConfigured);
    CHECK(registry->Offer({Id(1U, 1U), score_mw_com::Quality::kQm, 5U}).is_ok());
    auto duplicate = registry->Offer({Id(1U, 1U), score_mw_com::Quality::kQm, 6U});
    CHECK(duplicate.is_error() && duplicate.error_value().is_domain_error() &&
          duplicate.error_value().domain_error() == score_mw_com::OfferError::kAlreadyOffered);
    // Another quality of the same instance is a different offer.
    CHECK(registry->Offer({Id(1U, 1U), score_mw_com::Quality::kAsilB, 5U}).is_ok());

    // Snapshots are filtered per caller and by the filter.
    CHECK(FindCount(consumer, ServiceFilter(1U)) == 2U);
    CHECK(FindCount(consumer, {}) == 2U);
    CHECK(FindCount(consumer, ServiceFilter(2U)) == 0U);
    CHECK(FindCount(outsider, {}) == 0U);
    {
        score_mw_com::InstanceFilter asil;
        asil.quality(score_mw_com::Quality::kAsilB);
        CHECK(FindCount(consumer, asil) == 1U);
    }

    // Watch: first call returns at once, the second hangs until the set changes.
    auto search_ends = fidl::CreateEndpoints<score_mw_com::Search>();
    CHECK(consumer->StartFind({ServiceFilter(1U), std::move(search_ends->server)}).is_ok());
    fidl::SyncClient<score_mw_com::Search> search{std::move(search_ends->client)};
    auto first = search->Watch();
    CHECK(first.is_ok() && first->instances().size() == 2U);
    std::size_t second_size = 9999U;
    std::thread hanging{[&] {
        auto second = search->Watch();
        second_size = second.is_ok() ? second->instances().size() : 9998U;
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(second_size == 9999U);  // still parked
    CHECK(registry->StopOffer({Id(1U, 1U), score_mw_com::Quality::kAsilB}).is_ok());
    hanging.join();
    CHECK(second_size == 1U);

    // A second outstanding Watch is a protocol violation: the channel is closed.
    std::size_t parked_result = 0U;
    std::thread parked{[&] {
        auto r = search->Watch();
        parked_result = r.is_ok() ? 1U : 2U;
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto violation = search->Watch();
    parked.join();
    CHECK(violation.is_error());
    CHECK(parked_result == 2U);

    // The connection of a skeleton going away (crash) withdraws its offers.
    auto other_registry_handler = service.RegistryHandler({1001U, {1U}});
    {
        auto other = Connect<score_mw_com::ServiceRegistry>(other_registry_handler);
        CHECK(other->Offer({Id(1U, 2U), score_mw_com::Quality::kQm, 9U}).is_ok());
        CHECK(FindCount(consumer, ServiceFilter(1U)) == 2U);
    }
    std::size_t remaining = 9999U;
    for (int i = 0; i < 50 && remaining != 1U; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        remaining = FindCount(consumer, ServiceFilter(1U));
    }
    CHECK(remaining == 1U);

    loop.Quit();
    loop.JoinThreads();
    // Unbinds the connections, which still refer to the service.
    loop.Shutdown();
    std::printf("DISCOVERY_SERVICE_TEST %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
