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
#include <fidl/score.mw.com.wheelspeed/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

// Test driver: starts the skeleton (fuchsia.component.Binder), waits until the proxy component received WheelSpeed
// samples through the public mw::com API, then keeps (re)starting the skeleton until the proxy re-found it, and
// prints the numbers that the ITF test asserts on.

namespace
{
namespace wsfidl = score_mw_com_wheelspeed;

constexpr std::uint32_t kSamples = 100U;

void StartSkeleton()
{
    zx::channel client;
    zx::channel server;
    if (zx::channel::create(0, &client, &server) != ZX_OK)
    {
        return;
    }
    if (fdio_service_connect("/svc/skeleton_binder", server.release()) != ZX_OK)
    {
        std::printf("DRIVER cannot connect to the skeleton binder\n");
        return;
    }
    // Starting is triggered by the connection; give component manager time to act on it before closing.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

void Print(const char* label, const wsfidl::PhaseResult& r)
{
    std::printf(
        "DRIVER %s: found=%d subscribed=%d samples=%u order_errors=%u payload_errors=%u skipped=%u "
        "receive_handler_calls=%u find_handler_calls=%u incarnation=%u first_seq=%u last_seq=%u\n",
        label, r.found(), r.subscribed(), r.samples(), r.order_errors(), r.payload_errors(), r.skipped(),
        r.receive_handler_calls(), r.find_handler_calls(), r.incarnation(), r.first_sequence(), r.last_sequence());
    std::printf("DRIVER %s latency_us: min=%ld p50=%ld p99=%ld max=%ld find_ms=%ld first_sample_ms=%ld\n", label,
                static_cast<long>(r.latency_min_us()), static_cast<long>(r.latency_p50_us()),
                static_cast<long>(r.latency_p99_us()), static_cast<long>(r.latency_max_us()),
                static_cast<long>(r.find_ms()), static_cast<long>(r.first_sample_ms()));
}

bool Good(const wsfidl::PhaseResult& r)
{
    return r.found() && r.subscribed() && (r.samples() >= kSamples) && (r.order_errors() == 0U) &&
           (r.payload_errors() == 0U);
}
}  // namespace

int main()
{
    auto client_end = component::Connect<wsfidl::Probe>("/svc/probe");
    if (client_end.is_error())
    {
        std::printf("DRIVER cannot connect to the probe\n");
        return 1;
    }
    fidl::SyncClient<wsfidl::Probe> probe{std::move(*client_end)};

    StartSkeleton();
    auto first = probe->RunFirst({kSamples});
    if (first.is_error())
    {
        std::printf("DRIVER RunFirst failed: %s\n", first.error_value().FormatDescription().c_str());
        return 2;
    }
    Print("first", first->result());

    std::atomic<bool> done{false};
    std::thread restarter{[&done] {
        // Polling is robust against not knowing when the skeleton exits: binding a running component is a no-op.
        while (!done.load())
        {
            StartSkeleton();
            std::this_thread::sleep_for(std::chrono::milliseconds{400});
        }
    }};
    auto restart = probe->RunRestart({kSamples});
    done = true;
    restarter.join();
    if (restart.is_error())
    {
        std::printf("DRIVER RunRestart failed: %s\n", restart.error_value().FormatDescription().c_str());
        return 3;
    }
    Print("restart", restart->result());

    const bool ok = Good(first->result()) && Good(restart->result()) &&
                    (first->result().incarnation() != restart->result().incarnation());
    std::printf(ok ? "WHEELSPEED PASS\n" : "WHEELSPEED FAIL\n");
    return ok ? 0 : 4;
}
