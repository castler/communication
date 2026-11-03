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
#include "score/mw/com/impl/bindings/lola/fuchsia/event_notifying_service_instance.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_event_notifier.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/lola_shm_harness.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_shm_runtime.h"
#include "score/mw/com/impl/service_element_type.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/clock.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/process.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Event notification of LoLa on Fuchsia, with real LoLa structures and four processes:
//   orchestrator: spawns the skeleton, watches the phases through eventpairs, kills the skeleton at the end.
//   --skeleton:   VmoShmRuntime (Attach server + notification eventpairs) and SkeletonSide (real SkeletonMemoryManager
//                 regions). Publishes like SkeletonEvent::Send (EventDataControlComposite::AllocateNextSlot, EventReady)
//                 followed by NotifyEvent on the EventNotifyingServiceInstance decorator. Spawns the subscribers.
//   --sub <i>:    VmoShmRuntime (Attach client), ProxySide (real ConsumerEventDataControlLocalView). Registers a
//                 receive handler through the decorator, which re-reads the slot queue whenever it is called.
// Phases: A) all three subscribers receive up to counter kPhaseA. The skeleton kills subscriber 2; B) the remaining
// two receive up to kPhaseB and the skeleton prunes the dead subscriber; C) the skeleton publishes up to kFinal in a
// burst and is killed; the subscribers have to drain kFinal after the skeleton's death (provider lost).

namespace
{
namespace harness = score::mw::com::impl::lola::fuchsia::harness;
namespace lola = score::mw::com::impl::lola;
using score::mw::com::impl::ScopedEventReceiveHandler;
using score::mw::com::impl::lola::fuchsia::EventNotifyingServiceInstance;
using score::mw::com::impl::lola::fuchsia::IEventNotifier;
using score::mw::com::impl::lola::fuchsia::VmoShmRuntime;

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

constexpr int kSubscribers{3};
constexpr int kVictim{2};
constexpr std::uint64_t kApplicationId{77U};
constexpr std::uint64_t kPhaseA{500U};
constexpr std::uint64_t kPhaseB{1500U};
constexpr std::uint64_t kFinal{10000U};
constexpr zx_signals_t kPhaseASignal{ZX_USER_SIGNAL_0};
constexpr zx_signals_t kPhaseBSignal{ZX_USER_SIGNAL_1};
constexpr zx_signals_t kDrainedSignal{ZX_USER_SIGNAL_2};
constexpr zx_signals_t kFinalPublished{ZX_USER_SIGNAL_0};
constexpr zx_signals_t kSkeletonChecksOk{ZX_USER_SIGNAL_1};
constexpr std::uint32_t kCtlHandle = PA_HND(PA_USER0, 0xF0);
constexpr std::int64_t kTimeoutSeconds{90};

std::uint32_t AckHandleId(const int index)
{
    return PA_HND(PA_USER0, static_cast<std::uint32_t>(index));
}
std::uint32_t AckDuplicateHandleId(const int index)
{
    return PA_HND(PA_USER0, 0x10U + static_cast<std::uint32_t>(index));
}
constexpr std::uint32_t kEndpointHandleId = PA_HND(PA_USER0, 0);
constexpr std::uint32_t kSubAckHandleId = PA_HND(PA_USER0, 1);

const lola::ElementFqId kEvent{
    harness::kServiceId, 1U, harness::kInstanceId, score::mw::com::impl::ServiceElementType::EVENT};

bool WaitSignals(const zx::eventpair& pair, const zx_signals_t signals, const std::int64_t seconds = kTimeoutSeconds)
{
    zx_signals_t observed{};
    return (pair.wait_one(signals, zx::deadline_after(zx::sec(seconds)), &observed) == ZX_OK) &&
           ((observed & signals) == signals);
}

bool WaitProcessEnd(zx::process& process)
{
    return process.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(30)), nullptr) == ZX_OK;
}

std::optional<zx::process> Spawn(const std::string& mode,
                                 const std::string& argument,
                                 std::vector<fdio_spawn_action_t>& actions)
{
    const char* const argv[] = {"/pkg/bin/lola_events_test", mode.c_str(), argument.c_str(), nullptr};
    zx_handle_t process = ZX_HANDLE_INVALID;
    char error[FDIO_SPAWN_ERR_MSG_MAX_LENGTH] = {};
    const zx_status_t status = fdio_spawn_etc(
        ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, argv[0], argv, nullptr, actions.size(), actions.data(), &process, error);
    if (status != ZX_OK)
    {
        std::printf("FAIL spawn %s: %d %s\n", mode.c_str(), status, error);
        ++failures;
        return std::nullopt;
    }
    return zx::process{process};
}

fdio_spawn_action_t AddHandle(const std::uint32_t id, zx::handle handle)
{
    fdio_spawn_action_t action{};
    action.action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    action.h.id = id;
    action.h.handle = handle.release();
    return action;
}

/// Everything of the decorated instance except the event notification subset is irrelevant here.
class InnerInstance final : public lola::IMessagePassingServiceInstance
{
  public:
    void NotifyEvent(const lola::ElementFqId) noexcept override {}
    lola::IMessagePassingService::HandlerRegistrationNoType RegisterEventNotification(
        const lola::ElementFqId,
        std::weak_ptr<ScopedEventReceiveHandler>,
        const NodeId) noexcept override
    {
        std::printf("FAIL registration reached the message passing instance\n");
        ++failures;
        return 0U;
    }
    void ReregisterEventNotification(const lola::ElementFqId, const NodeId) noexcept override {}
    void UnregisterEventNotification(const lola::ElementFqId,
                                     const lola::IMessagePassingService::HandlerRegistrationNoType,
                                     const NodeId) noexcept override
    {
    }
    score::Result<void> RegisterOnServiceMethodSubscribedHandler(
        const lola::SkeletonInstanceIdentifier,
        lola::IMessagePassingService::ServiceMethodSubscribedHandler,
        lola::IMessagePassingService::AllowedConsumerUids) override
    {
        return {};
    }
    score::Result<void> RegisterOnServiceMethodUnsubscribedHandler(
        const lola::SkeletonInstanceIdentifier,
        lola::IMessagePassingService::ServiceMethodUnsubscribedHandler) override
    {
        return {};
    }
    score::Result<void> RegisterMethodCallHandler(const lola::ProxyMethodInstanceIdentifier,
                                                  lola::IMessagePassingService::MethodCallHandler,
                                                  const PrincipalId) override
    {
        return {};
    }
    void UnregisterOnServiceMethodSubscribedHandler(lola::SkeletonInstanceIdentifier) override {}
    void UnregisterOnServiceMethodUnsubscribedHandler(lola::SkeletonInstanceIdentifier) override {}
    void UnregisterMethodCallHandler(lola::ProxyMethodInstanceIdentifier) override {}
    void NotifyOutdatedNodeId(const NodeId, const NodeId) noexcept override {}
    void RegisterEventNotificationExistenceChangedCallback(
        const lola::ElementFqId,
        lola::IMessagePassingService::HandlerStatusChangeCallback) noexcept override
    {
    }
    void UnregisterEventNotificationExistenceChangedCallback(const lola::ElementFqId) noexcept override {}
    score::Result<void> SubscribeServiceMethod(const lola::SkeletonInstanceIdentifier&,
                                               const lola::ProxyInstanceIdentifier&,
                                               const NodeId) override
    {
        return {};
    }
    score::Result<void> UnsubscribeServiceMethod(const lola::SkeletonInstanceIdentifier&,
                                                 const lola::ProxyInstanceIdentifier&,
                                                 const NodeId) override
    {
        return {};
    }
    score::Result<void> CallMethod(const lola::ProxyMethodInstanceIdentifier&, const std::size_t, const NodeId) override
    {
        return {};
    }
};

EventNotifyingServiceInstance MakeDecorator()
{
    return EventNotifyingServiceInstance{std::make_unique<InnerInstance>()};
}

int RunSubscriber(const int index)
{
    zx::channel channel{zx_take_startup_handle(kEndpointHandleId)};
    zx::eventpair ack{zx_take_startup_handle(kSubAckHandleId)};
    CHECK(channel.is_valid());
    CHECK(ack.is_valid());
    if (failures != 0)
    {
        return 1;
    }
    std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> endpoint{
        fidl::ClientEnd<score_mw_com::Endpoint>{std::move(channel)}};
    async::Loop loop{&kAsyncLoopConfigNeverAttachToThread};
    VmoShmRuntime runtime{loop.dispatcher(), [&]() -> std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> {
                              auto result = std::move(endpoint);
                              endpoint.reset();
                              return result;
                          }};

    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t counter{0U};
    std::uint32_t timestamp{0U};
    std::uint64_t wakeups{0U};
    std::uint64_t delivered{0U};
    bool lost{false};
    bool order_ok{true};

    harness::ProxySide proxy{static_cast<std::uint32_t>(index) + 10U};
    CHECK(proxy.Open(harness::Quality::kQm));
    if (failures != 0)
    {
        return 2;
    }

    runtime.notifier().SetProviderLostCallback([&](std::uint16_t, std::uint16_t) {
        std::lock_guard<std::mutex> lock{mutex};
        lost = true;
        condition.notify_all();
    });

    // The receive handler: re-read the slot queue, whatever the number of coalesced notifications was.
    score::safecpp::Scope<> scope;
    auto handler = std::make_shared<ScopedEventReceiveHandler>(scope, [&]() noexcept {
        std::lock_guard<std::mutex> lock{mutex};
        ++wakeups;
        std::uint64_t value{};
        while (proxy.ReceiveNewest(timestamp, value))
        {
            ++delivered;
            if (value != timestamp || value < counter)
            {
                order_ok = false;
            }
            counter = value;
        }
        condition.notify_all();
    });
    auto decorator = MakeDecorator();
    const auto registration = decorator.RegisterEventNotification(kEvent, handler, {});
    CHECK((registration & IEventNotifier::kRegistrationBit) != 0U);

    const auto wait_for = [&](const std::function<bool()>& predicate) {
        std::unique_lock<std::mutex> lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds(kTimeoutSeconds), predicate);
    };

    CHECK(wait_for([&] { return counter >= kPhaseA; }));
    {
        std::lock_guard<std::mutex> lock{mutex};
        std::printf("SUB %d phase A counter=%lu wakeups=%lu delivered=%lu\n",
                    index,
                    static_cast<unsigned long>(counter),
                    static_cast<unsigned long>(wakeups),
                    static_cast<unsigned long>(delivered));
    }
    CHECK(ack.signal_peer(0U, kPhaseASignal) == ZX_OK);
    if (index == kVictim)
    {
        // Waits for being killed by the skeleton; the handler keeps running meanwhile.
        zx_nanosleep(zx_deadline_after(ZX_SEC(120)));
        return 3;
    }

    CHECK(wait_for([&] { return counter >= kPhaseB; }));
    CHECK(ack.signal_peer(0U, kPhaseBSignal) == ZX_OK);

    // Skeleton gets killed after the final burst; whether the last notification arrived before or after its death,
    // the death drain has to leave the final counter in place.
    CHECK(wait_for([&] { return lost; }));
    {
        std::lock_guard<std::mutex> lock{mutex};
        std::printf("SUB %d provider lost: counter=%lu wakeups=%lu delivered=%lu ordered=%d alive=%d\n",
                    index,
                    static_cast<unsigned long>(counter),
                    static_cast<unsigned long>(wakeups),
                    static_cast<unsigned long>(delivered),
                    order_ok ? 1 : 0,
                    runtime.notifier().IsProviderAlive(harness::kServiceId, harness::kInstanceId) ? 1 : 0);
        CHECK(counter == kFinal);
        CHECK(order_ok);
        CHECK(!runtime.notifier().IsProviderAlive(harness::kServiceId, harness::kInstanceId));
        CHECK(wakeups >= 1U);
    }
    if (failures == 0)
    {
        CHECK(ack.signal_peer(0U, kDrainedSignal) == ZX_OK);
    }
    decorator.UnregisterEventNotification(kEvent, registration, {});
    return failures == 0 ? 0 : 4;
}

bool Publish(harness::SkeletonSide& skeleton, EventNotifyingServiceInstance& decorator, std::uint64_t& counter)
{
    if (!skeleton.Publish(counter + 1U))
    {
        return false;
    }
    ++counter;
    decorator.NotifyEvent(kEvent);
    return true;
}

bool AllAcked(const std::vector<zx::eventpair>& acks, const zx_signals_t signal, const bool include_victim)
{
    for (int i = 0; i < kSubscribers; ++i)
    {
        if (!include_victim && i == kVictim)
        {
            continue;
        }
        zx_signals_t observed{};
        acks[static_cast<std::size_t>(i)].wait_one(signal, zx::time::infinite_past(), &observed);
        if ((observed & signal) == 0U)
        {
            return false;
        }
    }
    return true;
}

int RunSkeleton()
{
    zx::eventpair ctl{zx_take_startup_handle(kCtlHandle)};
    std::vector<zx::eventpair> acks;
    std::vector<zx::eventpair> sub_acks;
    for (int i = 0; i < kSubscribers; ++i)
    {
        acks.emplace_back(zx_take_startup_handle(AckDuplicateHandleId(i)));
        sub_acks.emplace_back(zx_take_startup_handle(AckHandleId(i)));
        CHECK(acks.back().is_valid());
        CHECK(sub_acks.back().is_valid());
    }
    CHECK(ctl.is_valid());
    if (failures != 0)
    {
        return 1;
    }

    async::Loop loop{&kAsyncLoopConfigNeverAttachToThread};
    CHECK(loop.StartThread("attach-server") == ZX_OK);
    auto runtime = std::make_unique<VmoShmRuntime>(
        loop.dispatcher(), []() -> std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> { return std::nullopt; });
    harness::SkeletonSide skeleton{false};
    harness::SampleData initial{};
    initial.magic = harness::kSampleMagic;
    CHECK(skeleton.CreateAndPublish(initial));
    auto decorator = MakeDecorator();

    std::vector<zx::process> subscribers;
    for (int i = 0; i < kSubscribers; ++i)
    {
        auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
        CHECK(endpoints.is_ok());
        if (!endpoints.is_ok())
        {
            return 2;
        }
        runtime->server().CreateHandler(kApplicationId)(std::move(endpoints->server));
        std::vector<fdio_spawn_action_t> actions;
        actions.push_back(AddHandle(kEndpointHandleId, zx::handle{endpoints->client.TakeChannel().release()}));
        actions.push_back(AddHandle(kSubAckHandleId, zx::handle{sub_acks[static_cast<std::size_t>(i)].release()}));
        auto process = Spawn("--sub", std::to_string(i), actions);
        if (!process.has_value())
        {
            return 3;
        }
        subscribers.push_back(std::move(*process));
    }

    for (int i = 0; i < 3000 && runtime->server().SubscriberCount() < static_cast<std::size_t>(kSubscribers); ++i)
    {
        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    }
    CHECK(runtime->server().SubscriberCount() == static_cast<std::size_t>(kSubscribers));
    std::printf("SKEL subscribers attached: %zu\n", runtime->server().SubscriberCount());

    // Phase A: burst up to kPhaseA, then a heartbeat per 5 ms until every subscriber confirmed (a subscriber that
    // registered its handler after the burst is served by the next publication).
    std::uint64_t counter{0U};
    const zx::time deadline = zx::deadline_after(zx::sec(kTimeoutSeconds));
    while ((counter < kPhaseA || !AllAcked(acks, kPhaseASignal, true)) && zx::clock::get_monotonic() < deadline)
    {
        CHECK(Publish(skeleton, decorator, counter));
        if (counter >= kPhaseA)
        {
            zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));
        }
    }
    CHECK(AllAcked(acks, kPhaseASignal, true));
    std::printf("SKEL phase A done at counter=%lu\n", static_cast<unsigned long>(counter));

    // Subscriber death: the publisher continues; the dead session is pruned with the next notifications.
    CHECK(zx_task_kill(subscribers[kVictim].get()) == ZX_OK);
    CHECK(WaitProcessEnd(subscribers[kVictim]));
    bool pruned{false};
    while ((counter < kPhaseB || !AllAcked(acks, kPhaseBSignal, false)) && zx::clock::get_monotonic() < deadline)
    {
        CHECK(Publish(skeleton, decorator, counter));
        pruned = pruned || (runtime->server().SubscriberCount() == static_cast<std::size_t>(kSubscribers - 1));
        if (counter >= kPhaseB)
        {
            zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));
        }
    }
    CHECK(AllAcked(acks, kPhaseBSignal, false));
    CHECK(pruned);
    std::printf("SKEL phase B done at counter=%lu, dead subscriber pruned=%d subscribers=%zu\n",
                static_cast<unsigned long>(counter),
                pruned ? 1 : 0,
                runtime->server().SubscriberCount());

    // Phase C: a burst, then death of the skeleton (by the orchestrator).
    while (counter < kFinal)
    {
        CHECK(Publish(skeleton, decorator, counter));
    }
    std::printf("SKEL final counter=%lu published\n", static_cast<unsigned long>(counter));
    if (failures == 0)
    {
        CHECK(ctl.signal_peer(0U, kSkeletonChecksOk) == ZX_OK);
    }
    CHECK(ctl.signal_peer(0U, kFinalPublished) == ZX_OK);
    zx_nanosleep(zx_deadline_after(ZX_SEC(120)));
    return 4;
}

int RunOrchestrator()
{
    std::vector<zx::eventpair> acks;
    std::vector<fdio_spawn_action_t> actions;
    for (int i = 0; i < kSubscribers; ++i)
    {
        zx::eventpair mine;
        zx::eventpair theirs;
        CHECK(zx::eventpair::create(0U, &mine, &theirs) == ZX_OK);
        zx::eventpair duplicate;
        CHECK(mine.duplicate(ZX_RIGHT_SAME_RIGHTS, &duplicate) == ZX_OK);
        actions.push_back(AddHandle(AckHandleId(i), zx::handle{theirs.release()}));
        actions.push_back(AddHandle(AckDuplicateHandleId(i), zx::handle{duplicate.release()}));
        acks.push_back(std::move(mine));
    }
    zx::eventpair ctl;
    zx::eventpair ctl_skeleton;
    CHECK(zx::eventpair::create(0U, &ctl, &ctl_skeleton) == ZX_OK);
    actions.push_back(AddHandle(kCtlHandle, zx::handle{ctl_skeleton.release()}));
    if (failures != 0)
    {
        return 1;
    }

    auto skeleton = Spawn("--skeleton", "0", actions);
    CHECK(skeleton.has_value());
    if (!skeleton.has_value())
    {
        return 2;
    }

    for (int i = 0; i < kSubscribers; ++i)
    {
        CHECK(WaitSignals(acks[static_cast<std::size_t>(i)], kPhaseASignal));
    }
    std::printf("ORCH all subscribers received phase A\n");
    zx_signals_t observed{};
    CHECK(acks[kVictim].wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::deadline_after(zx::sec(kTimeoutSeconds)), &observed) ==
          ZX_OK);
    std::printf("ORCH subscriber %d died\n", kVictim);
    for (int i = 0; i < kSubscribers; ++i)
    {
        if (i != kVictim)
        {
            CHECK(WaitSignals(acks[static_cast<std::size_t>(i)], kPhaseBSignal));
        }
    }
    std::printf("ORCH remaining subscribers received phase B\n");
    CHECK(WaitSignals(ctl, kFinalPublished));
    CHECK(WaitSignals(ctl, kSkeletonChecksOk, 1));
    CHECK(zx_task_kill(skeleton->get()) == ZX_OK);
    CHECK(WaitProcessEnd(*skeleton));
    std::printf("ORCH skeleton killed\n");
    for (int i = 0; i < kSubscribers; ++i)
    {
        if (i != kVictim)
        {
            CHECK(WaitSignals(acks[static_cast<std::size_t>(i)], kDrainedSignal));
        }
    }
    std::printf("ORCH subscribers drained the final counter after the skeleton's death\n");
    return failures == 0 ? 0 : 5;
}
}  // namespace

int main(int argc, char** argv)
{
    const char* const mode = argc > 1 ? argv[1] : "";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int result{};
    if (std::strcmp(mode, "--sub") == 0)
    {
        result = RunSubscriber(argc > 2 ? std::atoi(argv[2]) : 0);
    }
    else if (std::strcmp(mode, "--skeleton") == 0)
    {
        result = RunSkeleton();
    }
    else
    {
        result = RunOrchestrator();
    }
    std::printf("LOLA_EVENTS %s %s (result=%d, failures=%d)\n",
                mode[0] == '\0' ? "orchestrator" : mode,
                result == 0 ? "PASS" : "FAIL",
                result,
                failures);
    return result;
}
