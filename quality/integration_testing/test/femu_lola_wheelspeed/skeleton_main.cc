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

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <zircon/syscalls.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Skeleton component (ASIL-QM): offers wheelspeed/WheelSpeed through the public mw::com API and publishes one
// WheelSpeed sample every 10 ms (100 Hz). After --count samples it withdraws the offer and exits, so that the driver
// can restart the component (a new incarnation) and check that the proxy finds and subscribes again.
// The Fuchsia glue the application has to provide today: VmoShmRuntime (VMO regions instead of shm_open, FIDL Attach,
// notification eventpair) and the per-consumer Endpoint capability in its outgoing directory.

namespace
{
using namespace wheelspeed;
using score::mw::com::impl::lola::fuchsia::VmoShmRuntime;

constexpr auto kPeriod = std::chrono::milliseconds{10};

std::int64_t NowNs()
{
    return zx_clock_get_monotonic();
}
}  // namespace

int main(int argc, char** argv)
{
    // stderr is what the elf runner forwards at WARN level and the ITF output shows.
    std::fflush(stdout);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("SKEL main start\n");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::uint32_t count = 500U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--count=", 8U) == 0)
        {
            count = static_cast<std::uint32_t>(std::strtoul(argv[i] + 8, nullptr, 0));
        }
    }
    const auto incarnation = static_cast<std::uint32_t>(NowNs() / 1000000);

    async::Loop loop{&kAsyncLoopConfigNeverAttachToThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};

    VmoShmRuntime vmo_runtime{loop.dispatcher()};
    vmo_runtime.server().SetEpoch(incarnation);
    vmo_runtime.server().SetAllowedApplications(std::set<std::uint64_t>{kProxyAppId});
    if (outgoing
            .AddUnmanagedProtocol<score_mw_com::Endpoint>(vmo_runtime.server().CreateHandler(kProxyAppId),
                                                          "score.mw.com.Endpoint.for_app_" + std::to_string(kProxyAppId))
            .is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        std::printf("SKEL outgoing directory failed\n");
        return 2;
    }

    // The outgoing directory is thread-unsafe: it is served from this thread, the work runs on a worker.
    auto work = [&]() -> int {

    std::vector<std::string> storage{"wheelspeed_skeleton", "--service_instance_manifest", "/pkg/data/skeleton_config.json"};
    std::vector<score::safecpp::zstring_view> args;
    for (const auto& s : storage)
    {
        args.emplace_back(s.c_str(), s.size());
    }
    score::mw::com::runtime::InitializeRuntime(score::cpp::span<score::safecpp::zstring_view>{args});

    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{kInstanceSpecifier});
    if (!specifier.has_value())
    {
        std::printf("SKEL instance specifier failed\n");
        return 3;
    }
    auto skeleton = WheelSpeedSkeleton::Create(specifier.value());
    if (!skeleton.has_value())
    {
        std::printf("SKEL Skeleton::Create failed: %s\n", skeleton.error().Message().data());
        return 4;
    }
    auto offered = skeleton.value().OfferService();
    if (!offered.has_value())
    {
        std::printf("SKEL OfferService failed: %s\n", offered.error().Message().data());
        return 5;
    }
    std::printf("SKEL offered incarnation=%u count=%u\n", incarnation, count);

    auto next = std::chrono::steady_clock::now();
    for (std::uint32_t sequence = 0U; sequence < count; ++sequence)
    {
        next += kPeriod;
        std::this_thread::sleep_until(next);
        auto allocated = skeleton.value().wheel_speed.Allocate();
        if (!allocated.has_value())
        {
            std::printf("SKEL Allocate failed at %u: %s\n", sequence, allocated.error().Message().data());
            return 6;
        }
        auto& sample = allocated.value();
        sample->incarnation = incarnation;
        sample->sequence = sequence;
        for (std::size_t wheel = 0U; wheel < sample->wheel_rpm.size(); ++wheel)
        {
            sample->wheel_rpm[wheel] = ExpectedRpm(sequence, wheel);
        }
        sample->checksum = ExpectedChecksum(*sample);
        sample->publish_time_ns = NowNs();
        auto sent = skeleton.value().wheel_speed.Send(std::move(allocated).value());
        if (!sent.has_value())
        {
            std::printf("SKEL Send failed at %u: %s\n", sequence, sent.error().Message().data());
            return 7;
        }
    }
    std::printf("SKEL published %u samples, withdrawing the offer\n", count);
    skeleton.value().StopOfferService();
    std::fflush(stdout);
    return 0;
    };
    std::thread worker{[&work] { std::_Exit(work()); }};
    worker.detach();
    loop.Run();
    return 0;
}
