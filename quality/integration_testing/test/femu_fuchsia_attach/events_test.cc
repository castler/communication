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
#include "score/memory/shared/fuchsia/i_vmo_handle_provider.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/process.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Event notification: one skeleton (this process: FidlVmoHandleServer + a published slot in the data region) and several
// subscribers in separate processes (this binary re-spawned with "--subscriber"). The skeleton stores the slot, release-
// stores the sequence and calls NotifySubscribers(); the subscribers block in WaitForNotification() (eventpair
// USER_0), clear it and re-read the sequence. Subscriber kinds:
//   notify  blocks on the notification, records the receive time per sequence in its area of the control region
//   slow    like notify but needs 20 ms per event (publisher period is 1 ms): must never delay the publisher
//   poll    opt-in polling: releases the notification handle and spins on the sequence
//   victim  like notify, killed by the test (zx_task_kill) in the middle of the run
// Latency = receive timestamp (child, CLOCK_MONOTONIC) - publish timestamp (skeleton, taken before the store).

namespace
{
using namespace attach_test;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleClient;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleServer;

constexpr std::uint32_t kMaxSamples = 600U;
constexpr std::uint32_t kSamples = 500U;
constexpr std::uint32_t kWarmup = 50U;
constexpr std::uint64_t kPeriodNs = 1'000'000U;
constexpr std::uint64_t kRegion = 128U * 1024U;
constexpr std::uint64_t kArea = 8192U;
constexpr std::int64_t kSlowWorkNs = 20'000'000;

struct DataHeader
{
    std::atomic<std::uint64_t> seq;
    std::atomic<std::uint32_t> done;
    std::uint32_t pad[13];
    std::uint64_t publish_ns[kMaxSamples + 1U];
};

struct SubArea
{
    std::atomic<std::uint32_t> ready;
    std::atomic<std::uint32_t> exited_ok;
    std::atomic<std::uint64_t> wakeups;
    std::atomic<std::uint64_t> last_seq;
    std::uint64_t pad[5];
    std::uint64_t receive_ns[kMaxSamples + 1U];
};
static_assert(sizeof(SubArea) <= kArea);

enum class Kind
{
    kNotify,
    kSlow,
    kPoll,
    kVictim,
};

const char* const kKindNames[] = {"notify", "slow", "poll", "victim"};

std::uint64_t Now()
{
    return static_cast<std::uint64_t>(zx_clock_get_monotonic());
}

void* MapRegion(const zx::vmo& vmo, const bool writable)
{
    zx_vaddr_t address = 0U;
    const zx_vm_option_t perm = ZX_VM_PERM_READ | (writable ? ZX_VM_PERM_WRITE : 0U);
    if (zx_vmar_map(zx_vmar_root_self(), perm, 0U, vmo.get(), 0U, kRegion, &address) != ZX_OK)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(address);
}

int RunSubscriber(const Kind kind, const std::uint32_t index)
{
    zx::channel channel{zx_take_startup_handle(PA_HND(PA_USER0, 0))};
    if (!channel.is_valid())
    {
        return 10;
    }
    FidlVmoHandleClient client{fidl::ClientEnd<score_mw_com::Endpoint>{std::move(channel)}};
    auto data = client.Acquire(kDataPath);
    auto control = client.Acquire(kControlPath);
    if (!data.has_value() || !control.has_value())
    {
        return 11;
    }
    const auto* header = static_cast<const DataHeader*>(MapRegion(*data, false));
    auto* area = reinterpret_cast<SubArea*>(static_cast<std::uint8_t*>(MapRegion(*control, true)) + index * kArea);
    if (header == nullptr || area == nullptr)
    {
        return 12;
    }
    if (kind == Kind::kPoll)
    {
        client.ReleaseNotification();
    }
    area->ready.store(1U);

    std::uint64_t last = 0U;
    while (true)
    {
        if (kind == Kind::kPoll)
        {
            // pure polling, no syscall on the path
        }
        else
        {
            const zx_status_t status = client.WaitForNotification(zx::deadline_after(zx::sec(20)));
            if (status != ZX_OK)
            {
                return 20 + static_cast<int>(-status);
            }
            area->wakeups.fetch_add(1U);
        }
        const std::uint64_t seq = header->seq.load(std::memory_order_acquire);
        if (seq != last)
        {
            if (seq <= kMaxSamples)
            {
                area->receive_ns[seq] = Now();
            }
            last = seq;
            area->last_seq.store(seq);
            if (kind == Kind::kSlow)
            {
                zx_nanosleep(zx_deadline_after(kSlowWorkNs));
            }
        }
        if (header->done.load(std::memory_order_acquire) != 0U && last == header->seq.load(std::memory_order_acquire))
        {
            break;
        }
    }
    area->exited_ok.store(1U);
    return 0;
}

struct Child
{
    Kind kind;
    zx::process process;
};

bool Spawn(Child& child, const Kind kind, const std::uint32_t index, zx::channel endpoint)
{
    const std::string index_arg = std::to_string(index);
    const char* argv[] = {"/pkg/bin/events_test", "--subscriber", kKindNames[static_cast<int>(kind)], index_arg.c_str(), nullptr};
    fdio_spawn_action_t actions[1]{};
    actions[0].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    actions[0].h.id = PA_HND(PA_USER0, 0);
    actions[0].h.handle = endpoint.release();
    char error[FDIO_SPAWN_ERR_MSG_MAX_LENGTH]{};
    child.kind = kind;
    const zx_status_t status = fdio_spawn_etc(ZX_HANDLE_INVALID,
                                              FDIO_SPAWN_CLONE_ALL,
                                              argv[0],
                                              argv,
                                              nullptr,
                                              1U,
                                              actions,
                                              child.process.reset_and_get_address(),
                                              error);
    if (status != ZX_OK)
    {
        std::printf("spawn failed: %d %s\n", status, error);
        return false;
    }
    return true;
}

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

std::uint64_t Percentile(std::vector<std::uint64_t>& values, const double fraction)
{
    if (values.empty())
    {
        return 0U;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1U, static_cast<std::size_t>(fraction * static_cast<double>(values.size())))];
}

struct PhaseResult
{
    std::vector<std::uint64_t> publish_cost_ns;
    std::vector<std::vector<std::uint64_t>> latency_ns;  // per child
    std::vector<std::uint64_t> delivered;                // per child, distinct sequences received
};

struct Plan
{
    const char* name;
    std::vector<Kind> children;
    std::uint32_t kill_at;  // sample after which the (first) victim is killed, 0: none
};

void RunPhase(async::Loop& loop, const Plan& plan)
{
    std::printf("--- phase %s: %zu subscribers\n", plan.name, plan.children.size());
    async_dispatcher_t* dispatcher = loop.dispatcher();
    auto server = std::make_unique<FidlVmoHandleServer>(dispatcher);

    zx::vmo data_vmo;
    zx::vmo control_vmo;
    CHECK(zx::vmo::create(kRegion, 0U, &data_vmo) == ZX_OK);
    CHECK(zx::vmo::create(kRegion, 0U, &control_vmo) == ZX_OK);
    auto* header = static_cast<DataHeader*>(MapRegion(data_vmo, true));
    auto* areas = static_cast<std::uint8_t*>(MapRegion(control_vmo, true));
    CHECK(header != nullptr && areas != nullptr);
    zx::vmo data_copy;
    zx::vmo control_copy;
    CHECK(data_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &data_copy) == ZX_OK);
    CHECK(control_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &control_copy) == ZX_OK);
    CHECK(server->Publish(kDataPath, std::move(data_copy)).has_value());
    CHECK(server->Publish(kControlPath, std::move(control_copy)).has_value());
    server->SetEpoch(kEpoch);

    std::vector<Child> children(plan.children.size());
    for (std::uint32_t i = 0U; i < plan.children.size(); ++i)
    {
        auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
        server->CreateHandler(kProxyAppId)(std::move(endpoints->server));
        CHECK(Spawn(children[i], plan.children[i], i, std::move(endpoints->client).TakeChannel()));
    }

    // All subscribers attached; the pollers dropped their notification handle, so they are not subscribed any more.
    std::size_t pollers = 0U;
    for (const Kind kind : plan.children)
    {
        pollers += (kind == Kind::kPoll) ? 1U : 0U;
    }
    const std::uint64_t ready_deadline = Now() + 30'000'000'000ULL;
    while (Now() < ready_deadline)
    {
        std::size_t ready = 0U;
        for (std::uint32_t i = 0U; i < plan.children.size(); ++i)
        {
            ready += reinterpret_cast<SubArea*>(areas + i * kArea)->ready.load();
        }
        if (ready == plan.children.size())
        {
            break;
        }
        zx_nanosleep(zx_deadline_after(ZX_MSEC(2)));
    }
    for (std::uint32_t i = 0U; i < plan.children.size(); ++i)
    {
        CHECK(reinterpret_cast<SubArea*>(areas + i * kArea)->ready.load() == 1U);
    }
    CHECK(server->SubscriberCount() == plan.children.size() - pollers);
    std::printf("subscribers after attach: %zu (pollers opted out: %zu)\n", server->SubscriberCount(), pollers);

    PhaseResult result;
    std::size_t victim = plan.children.size();
    for (std::size_t i = 0U; i < plan.children.size(); ++i)
    {
        if (plan.children[i] == Kind::kVictim)
        {
            victim = i;
            break;
        }
    }
    std::size_t subscribers_before_kill = 0U;
    std::size_t subscribers_after_kill = 0U;

    std::uint64_t next = Now() + 5'000'000ULL;
    for (std::uint32_t seq = 1U; seq <= kSamples; ++seq)
    {
        while (Now() < next)
        {
        }
        next += kPeriodNs;
        if (seq == plan.kill_at)
        {
            subscribers_before_kill = server->SubscriberCount();
        }
        const std::uint64_t start = Now();
        header->publish_ns[seq] = start;
        header->seq.store(seq, std::memory_order_release);
        server->NotifySubscribers();
        result.publish_cost_ns.push_back(Now() - start);
        if (victim < children.size() && seq == plan.kill_at)
        {
            CHECK(children[victim].process.kill() == ZX_OK);
            zx_signals_t observed;
            children[victim].process.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(5)), &observed);
            std::printf("victim killed after sample %u: subscribers %zu -> ", seq, subscribers_before_kill);
        }
        if (victim < children.size() && seq == plan.kill_at + 5U)
        {
            subscribers_after_kill = server->SubscriberCount();
            std::printf("%zu\n", subscribers_after_kill);
        }
    }
    header->done.store(1U, std::memory_order_release);
    server->NotifySubscribers();

    for (std::uint32_t i = 0U; i < children.size(); ++i)
    {
        if (i == victim)
        {
            continue;
        }
        zx_signals_t observed = 0U;
        children[i].process.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(30)), &observed);
        zx_info_process_t info{};
        children[i].process.get_info(ZX_INFO_PROCESS, &info, sizeof(info), nullptr, nullptr);
        std::printf("subscriber %u (%s) exit code %ld\n", i, kKindNames[static_cast<int>(children[i].kind)], info.return_code);
        CHECK((observed & ZX_PROCESS_TERMINATED) != 0U);
        CHECK(info.return_code == 0);
    }

    // Statistics.
    std::vector<std::uint64_t> cost{result.publish_cost_ns.begin() + kWarmup, result.publish_cost_ns.end()};
    const std::uint64_t cost_max = *std::max_element(cost.begin(), cost.end());
    std::printf("RESULT phase=%s publish_cost_us p50=%.1f p99=%.1f max=%.1f\n",
                plan.name,
                static_cast<double>(Percentile(cost, 0.5)) / 1000.0,
                static_cast<double>(Percentile(cost, 0.99)) / 1000.0,
                static_cast<double>(cost_max) / 1000.0);

    for (std::uint32_t i = 0U; i < children.size(); ++i)
    {
        const auto* area = reinterpret_cast<const SubArea*>(areas + i * kArea);
        std::vector<std::uint64_t> latency;
        std::uint32_t delivered = 0U;
        for (std::uint32_t seq = 1U; seq <= kSamples; ++seq)
        {
            if (area->receive_ns[seq] != 0U)
            {
                ++delivered;
                if (seq > kWarmup)
                {
                    latency.push_back(area->receive_ns[seq] - header->publish_ns[seq]);
                }
            }
        }
        const char* kind = kKindNames[static_cast<int>(children[i].kind)];
        std::printf("RESULT phase=%s sub=%u kind=%s delivered=%u/%u (%.1f%%) wakeups=%lu last_seq=%lu latency_us p50=%.1f p99=%.1f\n",
                    plan.name,
                    i,
                    kind,
                    delivered,
                    kSamples,
                    100.0 * delivered / kSamples,
                    area->wakeups.load(),
                    area->last_seq.load(),
                    static_cast<double>(Percentile(latency, 0.5)) / 1000.0,
                    static_cast<double>(Percentile(latency, 0.99)) / 1000.0);
        if (i == victim)
        {
            CHECK(area->last_seq.load() < kSamples);
            CHECK(area->exited_ok.load() == 0U);
            continue;
        }
        // Whoever stayed alive has seen the final publication (no lost wakeup), whatever was coalesced on the way.
        CHECK(area->last_seq.load() == kSamples);
        if (children[i].kind == Kind::kSlow)
        {
            CHECK(delivered < kSamples / 2U);
            CHECK(area->wakeups.load() < kSamples);
        }
        else
        {
            CHECK(delivered >= kSamples * 8U / 10U);
        }
    }

    if (victim < children.size())
    {
        CHECK(subscribers_before_kill == plan.children.size() - pollers);
        CHECK(subscribers_after_kill == plan.children.size() - pollers - 1U);
    }
    // A blocked publisher would show the 20 ms of the slow subscriber; p99/max here are dominated by host scheduling
    // noise of the emulator (see report), so only the median and a generous bound on the maximum are asserted.
    CHECK(Percentile(cost, 0.5) < 200'000U);
    CHECK(cost_max < static_cast<std::uint64_t>(kSlowWorkNs) / 2U);
    for (auto& child : children)
    {
        child.process.kill();
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 4 && std::strcmp(argv[1], "--subscriber") == 0)
    {
        Kind kind = Kind::kNotify;
        for (int k = 0; k < 4; ++k)
        {
            if (std::strcmp(argv[2], kKindNames[k]) == 0)
            {
                kind = static_cast<Kind>(k);
            }
        }
        return RunSubscriber(kind, static_cast<std::uint32_t>(std::atoi(argv[3])));
    }

    async::Loop loop{&kAsyncLoopConfigNoAttachToCurrentThread};
    loop.StartThread("skeleton_dispatcher");

    const std::vector<Plan> plans{
        {"m1", {Kind::kNotify}, 0U},
        {"m2", {Kind::kNotify, Kind::kNotify}, 0U},
        {"m4", {Kind::kNotify, Kind::kNotify, Kind::kNotify, Kind::kNotify}, 0U},
        {"m8", std::vector<Kind>(8U, Kind::kNotify), 0U},
        {"mixed",
         {Kind::kNotify, Kind::kNotify, Kind::kNotify, Kind::kNotify, Kind::kSlow, Kind::kPoll, Kind::kVictim},
         250U},
    };
    for (const auto& plan : plans)
    {
        RunPhase(loop, plan);
    }

    loop.Quit();
    loop.JoinThreads();
    std::printf("EVENTS_TEST %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
