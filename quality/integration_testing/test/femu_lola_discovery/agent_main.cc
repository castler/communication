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
#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_test_support.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_discovery_transport.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_service_discovery_client.h"

#include <fidl/score.mw.com.test.lola/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Agent that drives the real LoLa FuchsiaServiceDiscoveryClient over the real FidlDiscoveryTransport against the
// discovery component. Role "skeleton" uses OfferService, role "proxy" uses (Start/Stop)FindService; the realm only
// routes the matching capability to each of them. The driver controls them through the Agent protocol.

namespace
{
using namespace score::mw::com::impl;
using namespace score::mw::com::impl::lola::fuchsia;

constexpr std::uint16_t kServiceId = 0x1234U;
constexpr std::uint16_t kForeignServiceId = 0x4321U;

class Agent final : public fidl::Server<score_mw_com_test_lola::Agent>
{
  public:
    explicit Agent(async_dispatcher_t* dispatcher)
        : dispatcher_{dispatcher},
          client_{std::make_unique<FuchsiaServiceDiscoveryClient>(std::make_unique<FidlDiscoveryTransport>())},
          offered_{"/lola/test/offered", kServiceId, 1U, QualityType::kASIL_QM},
          other_{"/lola/test/other", kServiceId, 2U, QualityType::kASIL_QM},
          foreign_{"/lola/test/foreign", kForeignServiceId, 1U, QualityType::kASIL_QM},
          find_any_{"/lola/test/find", kServiceId, std::nullopt, QualityType::kASIL_QM}
    {
    }

    void Offer(OfferRequest& request, OfferCompleter::Sync& completer) override
    {
        const auto* instance = Instance(request.instance());
        completer.Reply({(instance != nullptr) && client_->OfferService(instance->GetInstanceIdentifier()).has_value()});
    }

    void StopOffer(StopOfferRequest& request, StopOfferCompleter::Sync& completer) override
    {
        const auto* instance = Instance(request.instance());
        completer.Reply(
            {(instance != nullptr) &&
             client_->StopOfferService(instance->GetInstanceIdentifier(), IServiceDiscovery::QualityTypeSelector::kBoth)
                 .has_value()});
    }

    void OfferForeign(OfferForeignCompleter::Sync& completer) override
    {
        completer.Reply({client_->OfferService(foreign_.GetInstanceIdentifier()).has_value()});
    }

    void Crash(CrashCompleter::Sync& completer) override
    {
        completer.Reply();
        async::PostTask(dispatcher_, []() { std::_Exit(3); });
    }

    void StartFind(StartFindCompleter::Sync& completer) override
    {
        const auto result = client_->StartFindService(
            handle_,
            [this](ServiceHandleContainer<HandleType> handles, FindServiceHandle) {
                std::vector<std::uint16_t> ids;
                for (const auto& handle : handles)
                {
                    ids.push_back(InstanceIdOf(handle));
                }
                const std::lock_guard lock{mutex_};
                last_ = std::move(ids);
                ++calls_;
                cv_.notify_all();
            },
            find_any_.GetEnrichedInstanceIdentifier());
        if (!result.has_value())
        {
            std::printf("StartFindService failed\n");
        }
        const std::lock_guard lock{mutex_};
        completer.Reply({{calls_, last_}});
    }

    void StopFind(StopFindCompleter::Sync& completer) override
    {
        score::cpp::ignore = client_->StopFindService(handle_);
        const std::lock_guard lock{mutex_};
        completer.Reply({calls_});
    }

    void FindOnce(FindOnceCompleter::Sync& completer) override
    {
        const auto result = client_->FindService(find_any_.GetEnrichedInstanceIdentifier());
        std::vector<std::uint16_t> ids;
        if (result.has_value())
        {
            for (const auto& handle : result.value())
            {
                ids.push_back(InstanceIdOf(handle));
            }
        }
        completer.Reply({{result.has_value(), ids}});
    }

    void WaitCalls(WaitCallsRequest& request, WaitCallsCompleter::Sync& completer) override
    {
        std::unique_lock lock{mutex_};
        const auto reached = cv_.wait_for(lock, std::chrono::milliseconds{request.timeout_ms()}, [&]() {
            return calls_ >= request.min_calls();
        });
        completer.Reply({{reached, calls_, last_}});
    }

    void Calls(CallsCompleter::Sync& completer) override
    {
        const std::lock_guard lock{mutex_};
        completer.Reply({calls_});
    }

  private:
    const DiscoveryTestInstance* Instance(const std::uint16_t instance) const
    {
        if (instance == 1U)
        {
            return &offered_;
        }
        if (instance == 2U)
        {
            return &other_;
        }
        return nullptr;
    }

    async_dispatcher_t* dispatcher_;
    std::unique_ptr<FuchsiaServiceDiscoveryClient> client_;
    DiscoveryTestInstance offered_;
    DiscoveryTestInstance other_;
    DiscoveryTestInstance foreign_;
    DiscoveryTestInstance find_any_;
    const FindServiceHandle handle_{make_FindServiceHandle(1U)};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::uint32_t calls_{0U};
    std::vector<std::uint16_t> last_;
};

}  // namespace

int main(int, char**)
{
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};
    Agent agent{loop.dispatcher()};
    fidl::ServerBindingGroup<score_mw_com_test_lola::Agent> bindings;
    if (outgoing
            .AddUnmanagedProtocol<score_mw_com_test_lola::Agent>(
                [&](fidl::ServerEnd<score_mw_com_test_lola::Agent> server_end) {
                    bindings.AddBinding(loop.dispatcher(), std::move(server_end), &agent, fidl::kIgnoreBindingClosure);
                })
            .is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        return 1;
    }
    loop.Run();
    return 0;
}
