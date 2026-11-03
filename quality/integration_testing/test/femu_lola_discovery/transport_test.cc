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
#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_test_support.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_discovery_transport.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_service_discovery_client.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The real FuchsiaServiceDiscoveryClient and FidlDiscoveryTransport against an in-process DiscoveryService that is
// restarted (all channels closed, the new instance knows nothing) and that is unreachable for a while: the search is
// established again, the skeleton offers again, a proxy notices the changes made after the restart.

namespace
{
using namespace score::mw::com::impl;
using namespace score::mw::com::impl::lola::fuchsia;
using namespace std::chrono_literals;

constexpr std::uint16_t kServiceId = 0x1234U;
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

bool WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

/// The discovery component of this process: a service on its own thread that can be replaced.
class Hub
{
  public:
    Hub() : loop_{&kAsyncLoopConfigNoAttachToCurrentThread}
    {
        Create();
        loop_.StartThread("hub");
    }

    ~Hub()
    {
        Run([this]() { service_.reset(); });
        loop_.Shutdown();
    }

    FidlDiscoveryConnectors Connectors()
    {
        return FidlDiscoveryConnectors{
            [this]() -> zx::result<fidl::ClientEnd<score_mw_com::ServiceRegistry>> {
                if (down_)
                {
                    return zx::error(ZX_ERR_UNAVAILABLE);
                }
                auto endpoints = fidl::CreateEndpoints<score_mw_com::ServiceRegistry>();
                async::PostTask(loop_.dispatcher(), [this, server = std::move(endpoints->server)]() mutable {
                    if (!down_ && registry_handler_)
                    {
                        registry_handler_(std::move(server));
                    }
                });
                return zx::ok(std::move(endpoints->client));
            },
            [this]() -> zx::result<fidl::ClientEnd<score_mw_com::ServiceDiscovery>> {
                if (down_)
                {
                    return zx::error(ZX_ERR_UNAVAILABLE);
                }
                auto endpoints = fidl::CreateEndpoints<score_mw_com::ServiceDiscovery>();
                async::PostTask(loop_.dispatcher(), [this, server = std::move(endpoints->server)]() mutable {
                    if (!down_ && discovery_handler_)
                    {
                        discovery_handler_(std::move(server));
                    }
                });
                return zx::ok(std::move(endpoints->client));
            },
        };
    }

    /// Replaces the service: every channel is closed, the new instance has no offers.
    void Restart(const std::chrono::milliseconds downtime)
    {
        down_ = true;
        Run([this]() { service_.reset(); });
        std::this_thread::sleep_for(downtime);
        Run([this]() { Create(); });
        down_ = false;
    }

    std::size_t OfferCount()
    {
        std::size_t count = 0U;
        Run([&]() { count = service_->OfferCount(); });
        return count;
    }

    std::size_t SearchCount()
    {
        std::size_t count = 0U;
        Run([&]() { count = service_->SearchCount(); });
        return count;
    }

  private:
    void Create()
    {
        service_ = std::make_unique<DiscoveryService>(loop_.dispatcher());
        registry_handler_ = service_->RegistryHandler({1000U, {kServiceId}});
        discovery_handler_ = service_->DiscoveryHandler({4002U, {kServiceId}});
    }

    void Run(std::function<void()> task)
    {
        std::promise<void> done;
        async::PostTask(loop_.dispatcher(), [&]() {
            task();
            done.set_value();
        });
        done.get_future().wait();
    }

    async::Loop loop_;
    std::atomic<bool> down_{false};
    std::unique_ptr<DiscoveryService> service_;
    fidl::ProtocolHandler<score_mw_com::ServiceRegistry> registry_handler_;
    fidl::ProtocolHandler<score_mw_com::ServiceDiscovery> discovery_handler_;
};

struct Observed
{
    std::mutex mutex;
    std::vector<std::vector<std::uint16_t>> calls;

    std::size_t Count()
    {
        const std::lock_guard lock{mutex};
        return calls.size();
    }
    std::vector<std::uint16_t> Last()
    {
        const std::lock_guard lock{mutex};
        return calls.empty() ? std::vector<std::uint16_t>{} : calls.back();
    }
};
}  // namespace

int main()
{
    Hub hub;
    DiscoveryTestInstance skeleton_instance{"/lola/test/offered", kServiceId, 1U, QualityType::kASIL_QM};
    DiscoveryTestInstance find_any{"/lola/test/find", kServiceId, std::nullopt, QualityType::kASIL_QM};
    FuchsiaServiceDiscoveryClient skeleton{std::make_unique<FidlDiscoveryTransport>(hub.Connectors())};
    FuchsiaServiceDiscoveryClient proxy{std::make_unique<FidlDiscoveryTransport>(hub.Connectors())};

    Observed observed;
    CHECK(proxy
              .StartFindService(
                  make_FindServiceHandle(1U),
                  [&observed](ServiceHandleContainer<HandleType> handles, FindServiceHandle) {
                      std::vector<std::uint16_t> ids;
                      for (const auto& handle : handles)
                      {
                          ids.push_back(InstanceIdOf(handle));
                      }
                      const std::lock_guard lock{observed.mutex};
                      observed.calls.push_back(std::move(ids));
                  },
                  find_any.GetEnrichedInstanceIdentifier())
              .has_value());
    CHECK(WaitUntil([&]() { return hub.SearchCount() == 1U; }));
    CHECK(observed.Count() == 0U);

    // Plain offer / stop-offer through the real transport.
    CHECK(skeleton.OfferService(skeleton_instance.GetInstanceIdentifier()).has_value());
    CHECK(WaitUntil([&]() { return observed.Count() == 1U; }));
    CHECK(observed.Last() == std::vector<std::uint16_t>{1U});
    CHECK(hub.OfferCount() == 1U);

    // Discovery restarts quickly: every channel closed, no state. The skeleton offers again, the search is back.
    hub.Restart(0ms);
    CHECK(WaitUntil([&]() { return hub.OfferCount() == 1U && hub.SearchCount() == 1U; }));
    {
        auto found = proxy.FindService(find_any.GetEnrichedInstanceIdentifier());
        CHECK(found.has_value() && found.value().size() == 1U);
    }

    // The search works after the restart: changes are reported.
    CHECK(skeleton.StopOfferService(skeleton_instance.GetInstanceIdentifier(),
                                    IServiceDiscovery::QualityTypeSelector::kBoth)
              .has_value());
    CHECK(WaitUntil([&]() { return hub.OfferCount() == 0U; }));
    CHECK(WaitUntil([&]() { return observed.Last().empty(); }));
    CHECK(skeleton.OfferService(skeleton_instance.GetInstanceIdentifier()).has_value());
    CHECK(WaitUntil([&]() { return observed.Last() == std::vector<std::uint16_t>{1U}; }));

    // Discovery is away for a while (connections fail, searches back off): afterwards everything is restored.
    hub.Restart(700ms);
    CHECK(WaitUntil([&]() { return hub.OfferCount() == 1U && hub.SearchCount() == 1U; }));
    CHECK(WaitUntil([&]() { return observed.Last() == std::vector<std::uint16_t>{1U}; }));
    CHECK(skeleton.StopOfferService(skeleton_instance.GetInstanceIdentifier(),
                                    IServiceDiscovery::QualityTypeSelector::kBoth)
              .has_value());
    CHECK(WaitUntil([&]() { return observed.Last().empty(); }));

    // After StopFindService the handler is not called any more.
    CHECK(proxy.StopFindService(make_FindServiceHandle(1U)).has_value());
    const auto calls = observed.Count();
    CHECK(skeleton.OfferService(skeleton_instance.GetInstanceIdentifier()).has_value());
    std::this_thread::sleep_for(500ms);
    CHECK(observed.Count() == calls);
    CHECK(WaitUntil([&]() { return hub.SearchCount() == 0U; }));

    std::printf(failures == 0 ? "LOLA_DISCOVERY_TRANSPORT_TEST PASS\n" : "LOLA_DISCOVERY_TRANSPORT_TEST FAIL\n");
    return failures == 0 ? 0 : 1;
}
