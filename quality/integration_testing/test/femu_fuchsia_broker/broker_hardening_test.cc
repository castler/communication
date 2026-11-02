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
#include "score/mw/com/impl/bindings/lola/fuchsia/incarnation_header.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/process.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Hardening test of the lease broker. The test process spawns the broker (so that it can be killed and restarted) and
// plays every holder itself: VMOs, incarnation headers and liveness eventpairs are the test's, while the *processes*
// the broker is told to watch are real child processes (sleeper_main) that the test kills. Because the test keeps the
// local end of each liveness eventpair, it decides which of the two death signals the broker can see:
//   process-only : liveness end held by the test (stays open), child process killed   -> ZX_PROCESS_TERMINATED path
//   liveness-only: child process stays alive, test closes its liveness end            -> ZX_EVENTPAIR_PEER_CLOSED path
//   both         : the child holds the liveness end, killing the child fires both
// Scenarios: 1 proxy death, 2 skeleton death per signal, 3 broker restart with re-announce/adoption, 4 stress with
// repeated kill/restart cycles (epoch monotonicity, no leaked leases, VMOs, eventpairs or mappings in the broker).

namespace
{
namespace fl = score::mw::com::impl::lola::fuchsia;

constexpr std::uint64_t kServiceId = 0x7777U;
constexpr std::uint64_t kRegionSize = 4096U;
constexpr const char* kBrokerPath = "/pkg/bin/broker_main";
constexpr const char* kSleeperPath = "/pkg/bin/sleeper_main";

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        ++checks;                                                       \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

using Broker = fidl::SyncClient<score_mw_com::Broker>;
using BrokerErr = score_mw_com::BrokerError;

enum class Mode
{
    kProcessOnly,
    kLivenessOnly,
    kBoth,
};

const char* ModeName(const Mode mode)
{
    return mode == Mode::kProcessOnly ? "process-only" : (mode == Mode::kLivenessOnly ? "liveness-only" : "both");
}

bool WaitFor(const std::function<bool()>& predicate, const int timeout_ms = 5000)
{
    for (int waited = 0; waited <= timeout_ms; waited += 5)
    {
        if (predicate())
        {
            return true;
        }
        zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));
    }
    return predicate();
}

score_mw_com::ServiceInstanceId InstanceId(const std::uint32_t instance)
{
    score_mw_com::ServiceInstanceId id;
    id.service_id() = kServiceId;
    id.instance_id() = instance;
    id.major_version() = 1U;
    id.minor_version() = 0U;
    return id;
}

zx::process SpawnChild(const char* path, zx::channel directory_request, zx::eventpair held_end)
{
    fdio_spawn_action_t actions[2] = {};
    std::size_t count = 0U;
    if (directory_request.is_valid())
    {
        actions[count].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
        actions[count].h.id = PA_DIRECTORY_REQUEST;
        actions[count].h.handle = directory_request.release();
        ++count;
    }
    if (held_end.is_valid())
    {
        actions[count].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
        actions[count].h.id = PA_HND(PA_USER0, 0);
        actions[count].h.handle = held_end.release();
        ++count;
    }
    const char* const argv[] = {path, nullptr};
    zx_handle_t process = ZX_HANDLE_INVALID;
    char error[FDIO_SPAWN_ERR_MSG_MAX_LENGTH] = {};
    const zx_status_t status =
        fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, path, argv, nullptr, count, actions, &process, error);
    if (status != ZX_OK)
    {
        std::printf("FAIL spawn %s: %d %s\n", path, status, error);
        ++failures;
    }
    return zx::process{process};
}

void KillAndWait(zx::process& process)
{
    if (process.is_valid())
    {
        process.kill();
        process.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(10)), nullptr);
    }
}

struct HandleStats
{
    std::uint32_t total{0U};
    std::uint32_t vmos{0U};
    std::uint32_t eventpairs{0U};
    std::uint32_t processes{0U};
    std::size_t mappings{0U};

    bool operator==(const HandleStats& o) const
    {
        return total == o.total && vmos == o.vmos && eventpairs == o.eventpairs && processes == o.processes &&
               mappings == o.mappings;
    }
};

void Print(const char* when, const HandleStats& s)
{
    std::printf("broker handles[%s]: total=%u vmo=%u eventpair=%u process=%u mappings=%zu\n",
                when,
                s.total,
                s.vmos,
                s.eventpairs,
                s.processes,
                s.mappings);
}

// The broker process is the ground truth for leaks: every retained VMO, proxy `fenced` eventpair and watched liveness
// eventpair is a handle in its table, every retained control region a mapping.
HandleStats ReadHandleStats(const zx::process& broker)
{
    HandleStats out;
    zx_info_process_handle_stats_t stats{};
    if (broker.get_info(ZX_INFO_PROCESS_HANDLE_STATS, &stats, sizeof(stats), nullptr, nullptr) == ZX_OK)
    {
        for (std::size_t type = 0U; type < ZX_OBJ_TYPE_UPPER_BOUND; ++type)
        {
            out.total += stats.handle_count[type];
        }
        out.vmos = stats.handle_count[ZX_OBJ_TYPE_VMO];
        out.eventpairs = stats.handle_count[ZX_OBJ_TYPE_EVENTPAIR];
        out.processes = stats.handle_count[ZX_OBJ_TYPE_PROCESS];
    }
    else
    {
        std::printf("FAIL cannot read handle stats\n");
        ++failures;
    }
    std::size_t actual = 0U;
    std::size_t available = 0U;
    broker.get_info(ZX_INFO_PROCESS_MAPS, nullptr, 0U, &actual, &available);
    out.mappings = available;
    return out;
}

// The broker as a child process of the test: killable, restartable, inspectable.
class BrokerProcess
{
  public:
    bool Start()
    {
        zx::channel client;
        zx::channel server;
        if (zx::channel::create(0U, &client, &server) != ZX_OK)
        {
            return false;
        }
        process_ = SpawnChild(kBrokerPath, std::move(server), {});
        directory_ = std::move(client);
        return process_.is_valid();
    }

    void Kill()
    {
        KillAndWait(process_);
        directory_.reset();
    }

    Broker Connect() const
    {
        auto endpoints = fidl::CreateEndpoints<score_mw_com::Broker>();
        fdio_service_connect_at(directory_.get(), "svc/score.mw.com.Broker", endpoints->server.TakeChannel().release());
        return Broker{std::move(endpoints->client)};
    }

    const zx::process& process() const
    {
        return process_;
    }

  private:
    zx::process process_;
    zx::channel directory_;
};

score_mw_com::BrokerStats Inspect(Broker& broker, const std::uint32_t instance)
{
    auto result = broker->Inspect({InstanceId(instance)});
    if (result.is_error())
    {
        std::printf("FAIL Inspect: %s\n", result.error_value().FormatDescription().c_str());
        ++failures;
        return {};
    }
    return result->stats();
}

void PrintStats(const char* when, const std::uint32_t instance, const score_mw_com::BrokerStats& s)
{
    std::printf(
        "broker[%s inst=%u]: active=%lu last=%lu retained=%u draining=%u proxy_leases=%u fenced=%u released=%u "
        "by_process=%u by_liveness=%u\n",
        when,
        instance,
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

// A skeleton as the broker sees it: a lease, two regions with an incarnation header, a process and a liveness
// eventpair. The regions stay mapped in the test, so the tombstone written by the broker is observable.
struct Skeleton
{
    std::uint32_t instance{0U};
    Mode mode{Mode::kBoth};
    std::uint64_t epoch{0U};
    std::uint64_t lease{0U};
    zx::vmo data;
    zx::vmo control;
    zx_vaddr_t data_map{0U};
    zx_vaddr_t control_map{0U};
    zx::eventpair liveness_local;
    zx::process child;

    Skeleton() = default;
    Skeleton(const Skeleton&) = delete;
    Skeleton& operator=(const Skeleton&) = delete;
    ~Skeleton()
    {
        Unmap();
        KillAndWait(child);
    }

    void Unmap()
    {
        if (data_map != 0U)
        {
            zx::vmar::root_self()->unmap(data_map, kRegionSize);
            data_map = 0U;
        }
        if (control_map != 0U)
        {
            zx::vmar::root_self()->unmap(control_map, kRegionSize);
            control_map = 0U;
        }
    }

    fl::IncarnationHeader& ControlHeader() const
    {
        return *reinterpret_cast<fl::IncarnationHeader*>(control_map);
    }

    bool MakeRegions(const std::uint64_t header_epoch)
    {
        if (zx::vmo::create(kRegionSize, 0U, &data) != ZX_OK || zx::vmo::create(kRegionSize, 0U, &control) != ZX_OK ||
            zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0U, data, 0U, kRegionSize, &data_map) != ZX_OK ||
            zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0U, control, 0U, kRegionSize, &control_map) !=
                ZX_OK)
        {
            return false;
        }
        fl::PublishIncarnation(*reinterpret_cast<fl::IncarnationHeader*>(data_map), header_epoch);
        fl::PublishIncarnation(ControlHeader(), header_epoch);
        return true;
    }

    // Creates the child process and the liveness pair; the broker's ends are returned.
    bool NewHolder(zx::eventpair& remote_for_broker, zx::process& process_for_broker)
    {
        zx::eventpair local;
        zx::eventpair remote;
        if (zx::eventpair::create(0U, &local, &remote) != ZX_OK)
        {
            return false;
        }
        if (mode == Mode::kBoth)
        {
            child = SpawnChild(kSleeperPath, {}, std::move(local));
        }
        else
        {
            child = SpawnChild(kSleeperPath, {}, {});
            liveness_local = std::move(local);
        }
        remote_for_broker = std::move(remote);
        return child.is_valid() && child.duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &process_for_broker) == ZX_OK;
    }

    bool Begin(Broker& broker, const std::uint32_t instance_id, const Mode skeleton_mode)
    {
        instance = instance_id;
        mode = skeleton_mode;
        zx::eventpair remote;
        zx::process process;
        if (!NewHolder(remote, process))
        {
            return false;
        }
        auto begun = broker->BeginIncarnation({InstanceId(instance), std::move(remote), std::move(process)});
        if (begun.is_error())
        {
            std::printf("FAIL BeginIncarnation: %s\n", begun.error_value().FormatDescription().c_str());
            return false;
        }
        epoch = begun->epoch();
        lease = begun->skeleton_lease();
        return MakeRegions(epoch) && Publish(broker);
    }

    bool Publish(Broker& broker)
    {
        zx::vmo data_dup;
        zx::vmo control_dup;
        if (data.duplicate(ZX_RIGHT_SAME_RIGHTS, &data_dup) != ZX_OK ||
            control.duplicate(ZX_RIGHT_SAME_RIGHTS, &control_dup) != ZX_OK)
        {
            return false;
        }
        auto published = broker->Publish({InstanceId(instance), epoch, std::move(data_dup), std::move(control_dup), 0U, 0U});
        if (published.is_error())
        {
            std::printf("FAIL Publish: %s\n", published.error_value().FormatDescription().c_str());
        }
        return published.is_ok();
    }

    // Re-announce at a restarted broker with fresh liveness/process handles of the same, still running holder.
    std::optional<std::uint64_t> Reannounce(Broker& broker, std::optional<std::uint64_t> epoch_override, BrokerErr* error)
    {
        zx::vmo data_dup;
        zx::vmo control_dup;
        zx::eventpair local;
        zx::eventpair remote;
        zx::process process;
        if (data.duplicate(ZX_RIGHT_SAME_RIGHTS, &data_dup) != ZX_OK ||
            control.duplicate(ZX_RIGHT_SAME_RIGHTS, &control_dup) != ZX_OK ||
            zx::eventpair::create(0U, &local, &remote) != ZX_OK ||
            child.duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &process) != ZX_OK)
        {
            return std::nullopt;
        }
        auto reply = broker->Reannounce({InstanceId(instance),
                                         epoch_override.value_or(epoch),
                                         std::move(data_dup),
                                         std::move(control_dup),
                                         0U,
                                         0U,
                                         std::move(remote),
                                         std::move(process)});
        if (reply.is_error())
        {
            if (error != nullptr && reply.error_value().is_domain_error())
            {
                *error = reply.error_value().domain_error();
            }
            return std::nullopt;
        }
        // The old pair died with the old broker; the new one is held by the test (process-only from now on).
        liveness_local = std::move(local);
        mode = Mode::kProcessOnly;
        lease = reply->skeleton_lease();
        return lease;
    }

    // Takes the skeleton down the way `mode` prescribes.
    void Die()
    {
        if (mode == Mode::kLivenessOnly)
        {
            liveness_local.reset();
        }
        else
        {
            KillAndWait(child);
        }
    }
};

// A proxy lease. `fenced` is the broker's per-proxy fence signal.
struct ProxyLease
{
    std::uint64_t lease{0U};
    zx::eventpair fenced;
    zx::eventpair liveness_local;
    zx::process child;

    ~ProxyLease()
    {
        KillAndWait(child);
    }

    bool Register(Broker& broker, const std::uint32_t instance, const std::uint64_t epoch, const bool with_process,
                  BrokerErr* error = nullptr)
    {
        zx::eventpair local;
        zx::eventpair remote;
        zx::process process;
        if (zx::eventpair::create(0U, &local, &remote) != ZX_OK)
        {
            return false;
        }
        if (with_process)
        {
            child = SpawnChild(kSleeperPath, {}, {});
            if (!child.is_valid() || child.duplicate(ZX_RIGHT_WAIT | ZX_RIGHT_TRANSFER, &process) != ZX_OK)
            {
                return false;
            }
        }
        liveness_local = std::move(local);
        auto reply = broker->RegisterProxy({InstanceId(instance), epoch, 4002U, std::move(remote), std::move(process)});
        if (reply.is_error())
        {
            if (error != nullptr && reply.error_value().is_domain_error())
            {
                *error = reply.error_value().domain_error();
            }
            return false;
        }
        lease = reply->proxy_lease();
        fenced = std::move(reply->fenced());
        return true;
    }

    bool FencedSignalled(const int timeout_ms) const
    {
        zx_signals_t observed = 0U;
        fenced.wait_one(ZX_USER_SIGNAL_0 | ZX_EVENTPAIR_PEER_CLOSED, zx::deadline_after(zx::msec(timeout_ms)), &observed);
        return (observed & ZX_USER_SIGNAL_0) != 0U;
    }
};

bool LeasesAre(Broker& broker, const std::uint32_t instance, const std::uint32_t expected)
{
    return WaitFor([&] { return Inspect(broker, instance).live_proxy_leases() == expected; });
}

// --- 1: proxy death --------------------------------------------------------------------------------------------------

void ProxyDeathScenario(BrokerProcess& broker_process)
{
    std::printf("=== scenario 1: proxy death detection ===\n");
    constexpr std::uint32_t kInst = 11U;
    auto broker = broker_process.Connect();
    Skeleton skeleton;
    CHECK(skeleton.Begin(broker, kInst, Mode::kProcessOnly));

    ProxyLease released;
    ProxyLease liveness_closed;
    ProxyLease process_killed;
    ProxyLease survivor;
    CHECK(released.Register(broker, kInst, skeleton.epoch, false));
    CHECK(liveness_closed.Register(broker, kInst, skeleton.epoch, true));  // process stays alive
    CHECK(process_killed.Register(broker, kInst, skeleton.epoch, true));   // liveness end stays open in the test
    CHECK(survivor.Register(broker, kInst, skeleton.epoch, false));
    PrintStats("4 proxies", kInst, Inspect(broker, kInst));
    CHECK(Inspect(broker, kInst).live_proxy_leases() == 4U);

    broker->ReleaseLease({released.lease});
    CHECK(LeasesAre(broker, kInst, 3U));
    std::printf("proxy#1 explicit ReleaseLease -> leases=3\n");

    liveness_closed.liveness_local.reset();
    const bool by_liveness = LeasesAre(broker, kInst, 2U);
    std::printf("proxy#2 liveness eventpair closed (process alive=%d) -> lease dropped=%d\n",
                liveness_closed.child.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite_past(), nullptr) != ZX_OK,
                by_liveness);
    CHECK(by_liveness);

    KillAndWait(process_killed.child);
    const bool by_process = LeasesAre(broker, kInst, 1U);
    std::printf("proxy#3 process killed (liveness end still open=%d) -> lease dropped=%d\n",
                process_killed.liveness_local.is_valid(),
                by_process);
    CHECK(by_process);

    // Repeated release is harmless; the live proxy and the active incarnation are untouched.
    broker->ReleaseLease({released.lease});
    auto stats = Inspect(broker, kInst);
    CHECK(stats.live_proxy_leases() == 1U);
    CHECK(stats.active_epoch() == skeleton.epoch);
    CHECK(stats.retained_incarnations() == 1U);
    CHECK(!survivor.FencedSignalled(0));

    // Skeleton dies while the survivor still pins the incarnation: draining; its death then frees everything.
    skeleton.Die();
    CHECK(survivor.FencedSignalled(5000));
    stats = Inspect(broker, kInst);
    PrintStats("skeleton dead, survivor pins", kInst, stats);
    CHECK(stats.draining_incarnations() == 1U);
    CHECK(stats.retained_incarnations() == 1U);
    survivor.liveness_local.reset();
    CHECK(WaitFor([&] {
        const auto s = Inspect(broker, kInst);
        return s.retained_incarnations() == 0U && s.live_proxy_leases() == 0U && s.released_total() == 1U;
    }));
    PrintStats("survivor died", kInst, Inspect(broker, kInst));
}

// --- 2: skeleton death per detection signal --------------------------------------------------------------------------

void SkeletonDeathScenario(BrokerProcess& broker_process)
{
    std::printf("=== scenario 2: skeleton death detected by process handle / eventpair ===\n");
    constexpr std::uint32_t kInst = 21U;
    auto broker = broker_process.Connect();
    std::uint64_t previous_epoch = 0U;
    std::uint32_t expect_process = 0U;
    std::uint32_t expect_liveness = 0U;
    for (const Mode mode : {Mode::kProcessOnly, Mode::kLivenessOnly, Mode::kBoth})
    {
        Skeleton skeleton;
        CHECK(skeleton.Begin(broker, kInst, mode));
        CHECK(skeleton.epoch > previous_epoch);
        previous_epoch = skeleton.epoch;
        ProxyLease proxy;
        CHECK(proxy.Register(broker, kInst, skeleton.epoch, false));
        const auto before = Inspect(broker, kInst);

        skeleton.Die();
        const bool signalled = proxy.FencedSignalled(5000);
        const bool tombstoned = fl::IsIncarnationFenced(skeleton.ControlHeader());
        const auto after = Inspect(broker, kInst);
        PrintStats(ModeName(mode), kInst, after);
        std::printf("skeleton death %-13s: fenced_signal=%d tombstone_in_control_vmo=%d by_process %u->%u by_liveness %u->%u\n",
                    ModeName(mode),
                    signalled,
                    tombstoned,
                    before.fenced_by_process(),
                    after.fenced_by_process(),
                    before.fenced_by_liveness(),
                    after.fenced_by_liveness());
        CHECK(signalled);
        CHECK(tombstoned);
        CHECK(after.fenced_total() == before.fenced_total() + 1U);
        if (mode == Mode::kProcessOnly)
        {
            ++expect_process;
            CHECK(after.last_fence_reason() == score_mw_com::FenceReason::kProcessTerminated);
        }
        else if (mode == Mode::kLivenessOnly)
        {
            ++expect_liveness;
            CHECK(after.last_fence_reason() == score_mw_com::FenceReason::kLivenessClosed);
        }
        if (mode != Mode::kBoth)
        {
            CHECK(after.fenced_by_process() == expect_process);
            CHECK(after.fenced_by_liveness() == expect_liveness);
        }
        else
        {
            CHECK(after.fenced_by_process() + after.fenced_by_liveness() == expect_process + expect_liveness + 1U);
        }
        broker->ReleaseLease({proxy.lease});
        CHECK(WaitFor([&] { return Inspect(broker, kInst).retained_incarnations() == 0U; }));
    }
}

// --- 3: broker restart, re-announce, adoption ------------------------------------------------------------------------

void RestartScenario(BrokerProcess& broker_process)
{
    std::printf("=== scenario 3: broker restart, re-announce and adoption ===\n");
    constexpr std::uint32_t kA = 31U;
    constexpr std::uint32_t kB = 32U;
    constexpr std::uint32_t kC = 33U;
    auto broker = broker_process.Connect();
    Skeleton sa;
    Skeleton sb;
    Skeleton sc;
    CHECK(sa.Begin(broker, kA, Mode::kBoth));
    CHECK(sb.Begin(broker, kB, Mode::kProcessOnly));
    CHECK(sc.Begin(broker, kC, Mode::kProcessOnly));
    ProxyLease pa1;
    ProxyLease pa2;
    ProxyLease pb;
    CHECK(pa1.Register(broker, kA, sa.epoch, false));
    CHECK(pa2.Register(broker, kA, sa.epoch, false));
    CHECK(pb.Register(broker, kB, sb.epoch, false));
    std::printf("epochs before restart: A=%lu B=%lu C=%lu\n",
                static_cast<unsigned long>(sa.epoch),
                static_cast<unsigned long>(sb.epoch),
                static_cast<unsigned long>(sc.epoch));

    broker_process.Kill();
    std::printf("broker killed\n");
    // C's skeleton dies while there is no broker: nobody notices, the new broker never hears of it.
    KillAndWait(sc.child);

    // The proxies learn "broker lost, not skeleton lost": fenced eventpair peer-closed without USER_0, headers intact.
    zx_signals_t observed = 0U;
    pa1.fenced.wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::deadline_after(zx::sec(5)), &observed);
    const bool broker_lost = (observed & ZX_EVENTPAIR_PEER_CLOSED) != 0U && (observed & ZX_USER_SIGNAL_0) == 0U;
    std::printf("proxy sees broker loss (peer closed, no fence signal)=%d, control header still live=%d\n",
                broker_lost,
                fl::IsIncarnationLive(sa.ControlHeader(), sa.epoch));
    CHECK(broker_lost);
    CHECK(fl::IsIncarnationLive(sa.ControlHeader(), sa.epoch));

    CHECK(broker_process.Start());
    broker = broker_process.Connect();
    for (const std::uint32_t inst : {kA, kB, kC})
    {
        const auto stats = Inspect(broker, inst);
        CHECK(stats.active_epoch() == 0U && stats.retained_incarnations() == 0U && stats.live_proxy_leases() == 0U);
    }
    std::printf("new broker is empty (soft state lost)\n");

    // A proxy cannot register before its skeleton re-announced.
    ProxyLease premature;
    BrokerErr error{};
    CHECK(!premature.Register(broker, kA, sa.epoch, false, &error));
    CHECK(error == BrokerErr::kNotActive);

    // Re-announce: a wrong epoch (header mismatch) is refused, the right one adopted, a second one refused.
    std::optional<std::uint64_t> lease = sa.Reannounce(broker, sa.epoch + 1U, &error);
    CHECK(!lease.has_value() && error == BrokerErr::kInvalidRegion);
    lease = sa.Reannounce(broker, std::nullopt, &error);
    CHECK(lease.has_value());
    lease = sa.Reannounce(broker, std::nullopt, &error);
    CHECK(!lease.has_value() && error == BrokerErr::kAlreadyProvisioned);
    CHECK(sb.Reannounce(broker, std::nullopt, &error).has_value());
    std::printf("adopted: A epoch %lu, B epoch %lu\n", static_cast<unsigned long>(sa.epoch), static_cast<unsigned long>(sb.epoch));

    auto stats = Inspect(broker, kA);
    PrintStats("after adoption", kA, stats);
    CHECK(stats.active_epoch() == sa.epoch);
    CHECK(stats.last_epoch() == sa.epoch);
    CHECK(Inspect(broker, kB).active_epoch() == sb.epoch);

    // Proxies register again on the adopted incarnations.
    ProxyLease pa1b;
    ProxyLease pa2b;
    ProxyLease pbb;
    CHECK(pa1b.Register(broker, kA, sa.epoch, false));
    CHECK(pa2b.Register(broker, kA, sa.epoch, false));
    CHECK(pbb.Register(broker, kB, sb.epoch, false));
    CHECK(Inspect(broker, kA).live_proxy_leases() == 2U);
    CHECK(Inspect(broker, kB).live_proxy_leases() == 1U);

    // The new broker watches the adopted skeleton: its death is fenced, tombstoned and signalled like any other.
    KillAndWait(sb.child);
    CHECK(pbb.FencedSignalled(5000));
    CHECK(fl::IsIncarnationFenced(sb.ControlHeader()));
    CHECK(!pa1b.FencedSignalled(0));  // instance A is unaffected
    stats = Inspect(broker, kB);
    PrintStats("adopted skeleton B killed", kB, stats);
    CHECK(stats.fenced_by_process() == 1U || stats.fenced_by_liveness() == 1U);
    CHECK(stats.draining_incarnations() == 1U);

    // The skeleton of C died during the outage: a new incarnation gets a greater epoch than the lost one.
    Skeleton sc2;
    CHECK(sc2.Begin(broker, kC, Mode::kProcessOnly));
    std::printf("C: epoch before outage %lu, after restart %lu\n",
                static_cast<unsigned long>(sc.epoch),
                static_cast<unsigned long>(sc2.epoch));
    CHECK(sc2.epoch > sc.epoch);

    // A re-announced instance whose skeleton died also restarts with a greater epoch.
    broker->ReleaseLease({pbb.lease});
    CHECK(WaitFor([&] { return Inspect(broker, kB).retained_incarnations() == 0U; }));
    Skeleton sb2;
    CHECK(sb2.Begin(broker, kB, Mode::kLivenessOnly));
    std::printf("B: adopted epoch %lu, restarted epoch %lu\n", static_cast<unsigned long>(sb.epoch), static_cast<unsigned long>(sb2.epoch));
    CHECK(sb2.epoch > sb.epoch);
    CHECK(pa1b.lease != 0U && pa2b.lease != 0U);
}

// --- 4: stress --------------------------------------------------------------------------------------------------------

void StressScenario(BrokerProcess& broker_process, const int cycles)
{
    std::printf("=== scenario 4: stress, %d kill/restart cycles, 2 instances x 2 proxies ===\n", cycles);
    constexpr std::uint32_t kX = 41U;
    constexpr std::uint32_t kY = 42U;
    auto broker = broker_process.Connect();
    std::uint64_t last_epoch[2] = {0U, 0U};
    std::optional<HandleStats> baseline;
    int monotonic_violations = 0;
    int signal_failures = 0;
    int drain_failures = 0;
    std::uint32_t by_mode[3] = {0U, 0U, 0U};

    for (int cycle = 0; cycle < cycles; ++cycle)
    {
        const Mode mode = static_cast<Mode>(cycle % 3);
        ++by_mode[cycle % 3];
        Skeleton skeletons[2];
        ProxyLease proxies[2][2];
        const std::uint32_t ids[2] = {kX, kY};
        for (int i = 0; i < 2; ++i)
        {
            if (!skeletons[i].Begin(broker, ids[i], mode))
            {
                CHECK(false);
                return;
            }
            if (skeletons[i].epoch <= last_epoch[i])
            {
                ++monotonic_violations;
            }
            last_epoch[i] = skeletons[i].epoch;
            for (int p = 0; p < 2; ++p)
            {
                // Every 10th cycle one proxy is a real process that gets killed instead of releasing its lease.
                if (!proxies[i][p].Register(broker, ids[i], skeletons[i].epoch, cycle % 10 == 9 && p == 1))
                {
                    CHECK(false);
                    return;
                }
            }
        }
        // Kill one instance first: the other must stay live and un-fenced meanwhile.
        const int first = cycle % 2;
        const int second = 1 - first;
        skeletons[first].Die();
        for (auto& proxy : proxies[first])
        {
            if (!proxy.FencedSignalled(5000))
            {
                ++signal_failures;
            }
        }
        if (!fl::IsIncarnationFenced(skeletons[first].ControlHeader()))
        {
            ++signal_failures;
        }
        if (proxies[second][0].FencedSignalled(0) || !fl::IsIncarnationLive(skeletons[second].ControlHeader(), skeletons[second].epoch) ||
            Inspect(broker, ids[second]).active_epoch() != skeletons[second].epoch)
        {
            ++signal_failures;
        }
        skeletons[second].Die();
        for (auto& proxy : proxies[second])
        {
            if (!proxy.FencedSignalled(5000))
            {
                ++signal_failures;
            }
        }
        // Proxies go away: one by ReleaseLease, one by its liveness end / killed process (alternating order).
        for (int i : {first, second})
        {
            broker->ReleaseLease({proxies[i][0].lease});
            if (proxies[i][1].child.is_valid())
            {
                KillAndWait(proxies[i][1].child);
            }
            else
            {
                proxies[i][1].liveness_local.reset();
            }
        }
        for (int i = 0; i < 2; ++i)
        {
            if (!WaitFor([&] {
                    const auto s = Inspect(broker, ids[i]);
                    return s.retained_incarnations() == 0U && s.live_proxy_leases() == 0U && s.draining_incarnations() == 0U &&
                           s.active_epoch() == 0U;
                }))
            {
                ++drain_failures;
            }
        }
        if (cycle == 4)
        {
            zx_nanosleep(zx_deadline_after(ZX_MSEC(300)));  // let the broker finish the previous scenario's cleanup
            baseline = ReadHandleStats(broker_process.process());
            Print("baseline after 5 cycles", *baseline);
        }
        if (cycle % 20 == 0 || cycle == cycles - 1)
        {
            std::printf("cycle %d (%s): X epoch=%lu Y epoch=%lu\n",
                        cycle,
                        ModeName(mode),
                        static_cast<unsigned long>(last_epoch[0]),
                        static_cast<unsigned long>(last_epoch[1]));
        }
    }
    CHECK(monotonic_violations == 0);
    CHECK(signal_failures == 0);
    CHECK(drain_failures == 0);
    std::printf("cycles=%d monotonic_violations=%d signal_failures=%d drain_failures=%d modes(process/liveness/both)=%u/%u/%u\n",
                cycles,
                monotonic_violations,
                signal_failures,
                drain_failures,
                by_mode[0],
                by_mode[1],
                by_mode[2]);

    for (const std::uint32_t id : {kX, kY})
    {
        const auto s = Inspect(broker, id);
        PrintStats("stress end", id, s);
        CHECK(s.fenced_total() == static_cast<std::uint32_t>(cycles));
        CHECK(s.released_total() == static_cast<std::uint32_t>(cycles));
        CHECK(s.retained_incarnations() == 0U && s.live_proxy_leases() == 0U && s.draining_incarnations() == 0U);
        CHECK(s.fenced_by_process() + s.fenced_by_liveness() == static_cast<std::uint32_t>(cycles));
        CHECK(s.fenced_by_liveness() >= static_cast<std::uint32_t>(cycles / 3));
        CHECK(s.fenced_by_process() >= static_cast<std::uint32_t>(cycles / 3));
    }

    // Long-lived incarnation, many short-lived proxies: nothing may accumulate per proxy.
    constexpr std::uint32_t kZ = 43U;
    Skeleton z;
    CHECK(z.Begin(broker, kZ, Mode::kProcessOnly));
    for (int i = 0; i < 5; ++i)
    {
        ProxyLease warm;
        CHECK(warm.Register(broker, kZ, z.epoch, false));
        broker->ReleaseLease({warm.lease});
    }
    CHECK(LeasesAre(broker, kZ, 0U));
    const HandleStats churn_base = ReadHandleStats(broker_process.process());
    Print("churn baseline (Z live)", churn_base);
    for (int i = 0; i < 300; ++i)
    {
        ProxyLease proxy;
        CHECK(proxy.Register(broker, kZ, z.epoch, false));
        if (i % 2 == 0)
        {
            broker->ReleaseLease({proxy.lease});
        }
        else
        {
            proxy.liveness_local.reset();
        }
        if (i % 50 == 49)
        {
            CHECK(LeasesAre(broker, kZ, 0U));
        }
    }
    CHECK(LeasesAre(broker, kZ, 0U));
    HandleStats churn_end;
    const bool churn_clean = WaitFor([&] {
        churn_end = ReadHandleStats(broker_process.process());
        return churn_end == churn_base;
    });
    Print("after 300 proxies on a live incarnation", churn_end);
    CHECK(churn_clean);
    z.Die();
    CHECK(WaitFor([&] { return Inspect(broker, kZ).retained_incarnations() == 0U; }));

    // Everything released: the broker holds exactly what it held after the warm-up (no leaked VMOs, eventpairs,
    // process handles or mappings across the whole run).
    HandleStats end;
    const bool no_leak = WaitFor([&] {
        end = ReadHandleStats(broker_process.process());
        return baseline.has_value() && (end == *baseline);
    });
    Print("end of stress", end);
    std::printf("no leaked handles/mappings vs baseline: %d\n", no_leak);
    CHECK(no_leak);
}
}  // namespace

int main()
{
    BrokerProcess broker;
    if (!broker.Start())
    {
        std::printf("HARDENING_TEST FAIL cannot start broker\n");
        return 1;
    }
    Print("fresh broker", ReadHandleStats(broker.process()));
    ProxyDeathScenario(broker);
    SkeletonDeathScenario(broker);
    RestartScenario(broker);
    StressScenario(broker, 100);
    broker.Kill();
    std::printf("HARDENING_TEST %s (checks=%d failures=%d)\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
