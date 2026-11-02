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

#include <fidl/score.mw.com.test.broker/cpp/fidl.h>
#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>

#include <zircon/syscalls.h>

#include <cstdio>
#include <optional>

// Driver (realm root). Children: discovery, broker, skeleton (lazy: every start is a new incarnation) and proxy. Two
// rounds, one death each: the skeleton is killed by zx_process_exit, then by a trap. After each death the proxy must
// notice (liveness peer-closed, fence signal, tombstone in the control region), the restarted skeleton must have a
// greater epoch and new regions, and the proxy must reattach; the broker must release the old regions once the proxy
// gave up its lease.

namespace
{
using namespace broker_test;
namespace tb = score_mw_com_test_broker;

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

score_mw_com::ServiceInstanceId InstanceId()
{
    score_mw_com::ServiceInstanceId id;
    id.service_id() = kServiceId;
    id.instance_id() = kInstanceId;
    id.major_version() = 1U;
    id.minor_version() = 0U;
    return id;
}

template <typename Protocol>
std::optional<fidl::SyncClient<Protocol>> Connect(const char* path)
{
    auto end = component::Connect<Protocol>(path);
    if (end.is_error())
    {
        std::printf("FAIL cannot connect %s\n", path);
        ++failures;
        return std::nullopt;
    }
    return fidl::SyncClient<Protocol>{std::move(*end)};
}

score_mw_com::BrokerStats Inspect(fidl::SyncClient<score_mw_com::Broker>& broker)
{
    auto result = broker->Inspect({InstanceId()});
    if (result.is_error())
    {
        std::printf("FAIL Inspect: %s\n", result.error_value().FormatDescription().c_str());
        ++failures;
        return {};
    }
    return result->stats();
}

// The channel dies with the skeleton process: connect anew (this also starts the lazy component) and retry while the
// old process is still going away.
std::optional<std::uint64_t> SkeletonEpoch()
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        auto end = component::Connect<tb::Skeleton>("/svc/score.mw.com.test.broker.Skeleton");
        if (end.is_ok())
        {
            fidl::SyncClient<tb::Skeleton> client{std::move(*end)};
            auto reply = client->Epoch();
            if (reply.is_ok())
            {
                return reply->epoch();
            }
        }
        zx_nanosleep(zx_deadline_after(ZX_MSEC(100)));
    }
    return std::nullopt;
}

void PrintStats(const char* when, const score_mw_com::BrokerStats& s)
{
    std::printf(
        "broker[%s]: active=%lu last=%lu retained=%u draining=%u proxy_leases=%u fenced=%u released=%u by_process=%u "
        "by_liveness=%u\n",
        when,
        static_cast<unsigned long>(s.active_epoch()),
        static_cast<unsigned long>(s.last_epoch()),
        s.retained_incarnations(),
        s.draining_incarnations(),
        s.live_proxy_leases(),
        s.fenced_total(),
        s.released_total(),
        s.fenced_by_process(),
        s.fenced_by_liveness());
}

void CheckAttach(const tb::AttachResult& r, const std::uint64_t epoch, const char* name)
{
    std::printf("%s: ok=%d epoch=%lu discovery=%lu data_hdr=%lu ctl_hdr=%lu value=0x%lx lease=%lu released_lease=%lu us=%lu\n",
                name,
                r.ok(),
                static_cast<unsigned long>(r.epoch()),
                static_cast<unsigned long>(r.discovery_epoch()),
                static_cast<unsigned long>(r.data_header_epoch()),
                static_cast<unsigned long>(r.control_header_epoch()),
                static_cast<unsigned long>(r.data_value()),
                static_cast<unsigned long>(r.proxy_lease()),
                static_cast<unsigned long>(r.released_lease()),
                static_cast<unsigned long>(r.attach_us()));
    CHECK(r.ok());
    CHECK(r.epoch() == epoch);
    CHECK(r.discovery_epoch() == epoch);
    CHECK(r.data_header_epoch() == epoch);
    CHECK(r.control_header_epoch() == epoch);
    CHECK(r.data_value() == (epoch ^ kValueMask));
}
}  // namespace

int main()
{
    auto broker = Connect<score_mw_com::Broker>("/svc/score.mw.com.Broker");
    auto proxy = Connect<tb::Proxy>("/svc/score.mw.com.test.broker.Proxy");
    if (!broker || !proxy)
    {
        std::printf("BROKER_TEST FAIL (failures=%d)\n", failures);
        return 1;
    }

    // Incarnation 1.
    std::uint64_t epoch = SkeletonEpoch().value_or(0U);
    CHECK(epoch != 0U);
    auto attached = (*proxy)->Attach();
    CHECK(attached.is_ok());
    if (attached.is_ok())
    {
        CheckAttach(attached->result(), epoch, "attach#1");
        CHECK(attached->result().released_lease() == 0U);
    }
    auto stats = Inspect(*broker);
    PrintStats("after attach#1", stats);
    CHECK(stats.active_epoch() == epoch);
    CHECK(stats.retained_incarnations() == 1U);
    CHECK(stats.live_proxy_leases() == 1U);
    CHECK(stats.fenced_total() == 0U);

    const tb::CrashMode modes[] = {tb::CrashMode::kExit, tb::CrashMode::kTrap};
    std::uint32_t round = 0U;
    for (const auto mode : modes)
    {
        ++round;
        std::printf("--- round %u: crash by %s ---\n", round, mode == tb::CrashMode::kExit ? "zx_process_exit" : "trap");
        const auto old_epoch = epoch;
        {
            auto end = component::Connect<tb::Skeleton>("/svc/score.mw.com.test.broker.Skeleton");
            CHECK(end.is_ok());
            if (end.is_ok())
            {
                fidl::SyncClient<tb::Skeleton> client{std::move(*end)};
                CHECK(client->Crash({mode}).is_ok());
            }
        }

        auto fence = (*proxy)->WaitFenced({10000U});
        CHECK(fence.is_ok());
        if (fence.is_ok())
        {
            const auto& f = fence->result();
            std::printf(
                "fence#%u: timed_out=%d liveness_closed=%d fenced_signalled=%d tombstoned=%d data_hdr_after=%lu "
                "value_after=0x%lx detect_us=%lu tombstone_us=%lu\n",
                round,
                f.timed_out(),
                f.skeleton_liveness_closed(),
                f.fenced_signalled(),
                f.control_tombstoned(),
                static_cast<unsigned long>(f.data_header_epoch_after()),
                static_cast<unsigned long>(f.data_value_after()),
                static_cast<unsigned long>(f.detect_us()),
                static_cast<unsigned long>(f.tombstone_us()));
            CHECK(!f.timed_out());
            CHECK(f.fenced_signalled());
            CHECK(f.control_tombstoned());
            CHECK(f.data_header_epoch_after() == old_epoch);
            CHECK(f.data_value_after() == (old_epoch ^ kValueMask));
        }
        stats = Inspect(*broker);
        PrintStats("after death", stats);
        CHECK(stats.active_epoch() == 0U);
        CHECK(stats.fenced_total() == round);
        // The proxy still holds its lease: the dead incarnation is retained (draining), nothing is active.
        CHECK(stats.draining_incarnations() == 1U);
        CHECK(stats.retained_incarnations() == 1U);
        CHECK(stats.live_proxy_leases() == 1U);
        CHECK(stats.fenced_by_process() + stats.fenced_by_liveness() == round);

        // Restart: any connection to the lazy skeleton starts a new process; the epoch must be greater.
        epoch = SkeletonEpoch().value_or(0U);
        std::printf("restarted: epoch %lu -> %lu\n", static_cast<unsigned long>(old_epoch), static_cast<unsigned long>(epoch));
        CHECK(epoch > old_epoch);

        stats = Inspect(*broker);
        PrintStats("after restart", stats);
        CHECK(stats.active_epoch() == epoch);
        CHECK(stats.retained_incarnations() == 2U);

        // Reattach: new regions of the new incarnation, the old lease is released and the old regions with it.
        auto again = (*proxy)->Attach();
        CHECK(again.is_ok());
        if (again.is_ok())
        {
            CheckAttach(again->result(), epoch, "reattach");
            CHECK(again->result().released_lease() != 0U);
        }
        stats = Inspect(*broker);
        PrintStats("after reattach", stats);
        CHECK(stats.active_epoch() == epoch);
        CHECK(stats.retained_incarnations() == 1U);
        CHECK(stats.draining_incarnations() == 0U);
        CHECK(stats.live_proxy_leases() == 1U);
        CHECK(stats.released_total() == round);
    }

    std::printf("BROKER_TEST %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
