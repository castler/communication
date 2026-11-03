/*******************************************************************************
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
#include "quality/integration_testing/test/femu_lola_wheelspeed/wheel_speed_interface.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_shm_runtime.h"
#include "score/mw/com/runtime.h"

#include <fidl/score.mw.com.wheelspeed/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <zircon/syscalls.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Proxy component: finds wheelspeed/WheelSpeed (StartFindService), creates the proxy, subscribes, receives through the
// receive handler and GetNewSamples, and verifies sequence and payload. When the offer disappears (skeleton gone) it
// drops the proxy and starts over with the next offer. The driver reads the outcome through the Probe protocol.

namespace
{
using namespace wheelspeed;
using score::mw::com::impl::lola::fuchsia::VmoShmRuntime;
namespace wsfidl = score_mw_com_wheelspeed;

constexpr std::size_t kMaxSampleCount = 10U;

std::int64_t NowNs()
{
    return zx_clock_get_monotonic();
}

struct Accumulator
{
    bool used{false};
    std::uint32_t incarnation{0U};
    std::uint32_t samples{0U};
    std::uint32_t order_errors{0U};
    std::uint32_t payload_errors{0U};
    std::uint32_t skipped{0U};
    std::uint32_t first_sequence{0U};
    std::uint32_t last_sequence{0U};
    std::int64_t find_ms{-1};
    std::int64_t first_sample_ms{-1};
    std::vector<std::int64_t> latency_us;
};

class State
{
  public:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<WheelSpeedProxy::HandleType> handles;  // result of the last find handler call
    std::uint32_t find_handler_calls{0U};
    std::uint32_t receive_handler_calls{0U};
    std::array<Accumulator, 2> phases;  // 0: first incarnation, 1: any later one
    std::int64_t last_lost_ns{0};
    std::int64_t subscribe_ns{0};
    std::int64_t process_start_ns{NowNs()};

    wsfidl::PhaseResult Snapshot(const std::size_t phase)
    {
        const auto& acc = phases[phase];
        wsfidl::PhaseResult result{};
        result.found() = acc.used;
        result.subscribed() = acc.used;
        result.samples() = acc.samples;
        result.order_errors() = acc.order_errors;
        result.payload_errors() = acc.payload_errors;
        result.skipped() = acc.skipped;
        result.receive_handler_calls() = receive_handler_calls;
        result.find_handler_calls() = find_handler_calls;
        result.incarnation() = acc.incarnation;
        result.first_sequence() = acc.first_sequence;
        result.last_sequence() = acc.last_sequence;
        result.find_ms() = acc.find_ms;
        result.first_sample_ms() = acc.first_sample_ms;
        auto sorted = acc.latency_us;
        std::sort(sorted.begin(), sorted.end());
        if (!sorted.empty())
        {
            result.latency_min_us() = sorted.front();
            result.latency_p50_us() = sorted[sorted.size() / 2U];
            result.latency_p99_us() = sorted[std::min(sorted.size() - 1U, (sorted.size() * 99U) / 100U)];
            result.latency_max_us() = sorted.back();
        }
        return result;
    }
};

State g_state;

void Record(const WheelSpeed& sample, const std::int64_t now_ns)
{
    std::lock_guard<std::mutex> lock{g_state.mutex};
    std::size_t phase = 0U;
    if (g_state.phases[0].used && g_state.phases[0].incarnation != sample.incarnation)
    {
        phase = 1U;
    }
    auto& acc = g_state.phases[phase];
    if (!acc.used)
    {
        acc.used = true;
        acc.incarnation = sample.incarnation;
        acc.first_sequence = sample.sequence;
        acc.find_ms = ((phase == 0U) ? (g_state.subscribe_ns - g_state.process_start_ns)
                                     : (g_state.subscribe_ns - g_state.last_lost_ns)) /
                      1000000;
        acc.first_sample_ms = (now_ns - g_state.subscribe_ns) / 1000000;
    }
    else if (acc.incarnation != sample.incarnation)
    {
        ++acc.order_errors;  // a third incarnation is not expected
        return;
    }
    else if (sample.sequence <= acc.last_sequence)
    {
        ++acc.order_errors;
    }
    else
    {
        acc.skipped += sample.sequence - acc.last_sequence - 1U;
    }
    acc.last_sequence = std::max(acc.last_sequence, sample.sequence);

    bool payload_ok = sample.checksum == ExpectedChecksum(sample);
    for (std::size_t wheel = 0U; wheel < sample.wheel_rpm.size(); ++wheel)
    {
        payload_ok = payload_ok && (sample.wheel_rpm[wheel] == ExpectedRpm(sample.sequence, wheel));
    }
    if (!payload_ok)
    {
        ++acc.payload_errors;
    }
    ++acc.samples;
    acc.latency_us.push_back((now_ns - sample.publish_time_ns) / 1000);
    g_state.cv.notify_all();
}

class Semaphore
{
  public:
    void Post()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        ++count_;
        cv_.notify_one();
    }
    bool WaitFor(const std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!cv_.wait_for(lock, timeout, [this] { return count_ > 0U; }))
        {
            return false;
        }
        count_ = 0U;
        return true;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::uint32_t count_{0U};
};

void Worker()
{
    std::printf("PROXY worker start\n");
    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{kInstanceSpecifier});
    if (!specifier.has_value())
    {
        std::printf("PROXY instance specifier failed\n");
        return;
    }
    auto find = WheelSpeedProxy::StartFindService(
        [](auto handles, auto) {
            std::lock_guard<std::mutex> lock{g_state.mutex};
            ++g_state.find_handler_calls;
            g_state.handles = std::move(handles);
            std::printf("PROXY find handler: %zu handle(s)\n", g_state.handles.size());
            g_state.cv.notify_all();
        },
        specifier.value());
    if (!find.has_value())
    {
        std::printf("PROXY StartFindService failed: %s\n", find.error().Message().data());
        return;
    }

    while (true)
    {
        std::optional<WheelSpeedProxy::HandleType> handle;
        {
            std::unique_lock<std::mutex> lock{g_state.mutex};
            g_state.cv.wait(lock, [] { return !g_state.handles.empty(); });
            handle = g_state.handles.front();
        }
        auto proxy = WheelSpeedProxy::Create(*handle);
        if (!proxy.has_value())
        {
            std::printf("PROXY Proxy::Create failed: %s\n", proxy.error().Message().data());
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            continue;
        }
        Semaphore wakeup;
        auto& event = proxy.value().wheel_speed;
        {
            std::lock_guard<std::mutex> lock{g_state.mutex};
            g_state.subscribe_ns = NowNs();
        }
        auto subscribed = event.Subscribe(kMaxSampleCount);
        if (!subscribed.has_value())
        {
            std::printf("PROXY Subscribe failed: %s\n", subscribed.error().Message().data());
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            continue;
        }
        auto handler_set = event.SetReceiveHandler([&wakeup] {
            {
                std::lock_guard<std::mutex> lock{g_state.mutex};
                ++g_state.receive_handler_calls;
            }
            wakeup.Post();
        });
        if (!handler_set.has_value())
        {
            std::printf("PROXY SetReceiveHandler failed: %s\n", handler_set.error().Message().data());
        }
        std::printf("PROXY subscribed\n");

        while (true)
        {
            std::ignore = wakeup.WaitFor(std::chrono::milliseconds{100});
            auto received = event.GetNewSamples(
                [](auto sample) {
                    Record(*sample, NowNs());
                },
                kMaxSampleCount);
            if (!received.has_value())
            {
                std::printf("PROXY GetNewSamples failed: %s\n", received.error().Message().data());
            }
            std::lock_guard<std::mutex> lock{g_state.mutex};
            if (g_state.handles.empty())
            {
                break;
            }
        }
        std::printf("PROXY offer disappeared, dropping the proxy\n");
        std::ignore = event.UnsetReceiveHandler();
        event.Unsubscribe();
        {
            std::lock_guard<std::mutex> lock{g_state.mutex};
            g_state.last_lost_ns = NowNs();
        }
        // The proxy object is destroyed at the end of this iteration; the next offer starts the cycle again.
    }
}

class Probe final : public fidl::Server<wsfidl::Probe>
{
  public:
    void RunFirst(RunFirstRequest& request, RunFirstCompleter::Sync& completer) override
    {
        completer.Reply(Wait(0U, request.samples(), std::chrono::seconds{60}));
    }
    void RunRestart(RunRestartRequest& request, RunRestartCompleter::Sync& completer) override
    {
        completer.Reply(Wait(1U, request.samples(), std::chrono::seconds{90}));
    }

  private:
    static wsfidl::PhaseResult Wait(const std::size_t phase, const std::uint32_t samples, const std::chrono::seconds timeout)
    {
        std::unique_lock<std::mutex> lock{g_state.mutex};
        std::ignore =
            g_state.cv.wait_for(lock, timeout, [&] { return g_state.phases[phase].samples >= samples; });
        return g_state.Snapshot(phase);
    }
};
}  // namespace

int main()
{
    std::fflush(stdout);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("PROXY main start\n");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    async::Loop shm_loop{&kAsyncLoopConfigNeverAttachToThread};
    shm_loop.StartThread("proxy-shm");
    VmoShmRuntime vmo_runtime{shm_loop.dispatcher()};

    std::vector<std::string> storage{"wheelspeed_proxy", "--service_instance_manifest", "/pkg/data/proxy_config.json"};
    std::vector<score::safecpp::zstring_view> args;
    for (const auto& s : storage)
    {
        args.emplace_back(s.c_str(), s.size());
    }
    score::mw::com::runtime::InitializeRuntime(score::cpp::span<score::safecpp::zstring_view>{args});

    std::printf("PROXY runtime initialized\n");
    std::thread worker{Worker};
    worker.detach();

    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};
    Probe probe;
    fidl::ServerBindingGroup<wsfidl::Probe> bindings;
    if (outgoing
            .AddUnmanagedProtocol<wsfidl::Probe>(
                [&](fidl::ServerEnd<wsfidl::Probe> server_end) {
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
