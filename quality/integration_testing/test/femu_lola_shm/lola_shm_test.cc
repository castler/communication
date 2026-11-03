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
#include "score/memory/shared/flock/exclusive_flock_mutex.h"
#include "score/memory/shared/lock_file.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/lola_shm_harness.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_shm_runtime.h"
#include "score/os/utils/interprocess/interprocess_conditional_variable.h"
#include "score/os/utils/interprocess/interprocess_mutex.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/process.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <vector>

// Two-process test of LoLa's runtime shared-memory path on Fuchsia.
//   parent:   hosts the skeleton side (real lola::SkeletonMemoryManager creating data / QM control / ASIL-B control
//             through SharedMemoryFactory -> VmoSharedMemoryFactory) and serves the Attach protocol; then the
//             InterprocessMutex / InterprocessConditionalVariable scenario as the "ping" side.
//   --proxy:  opens the regions through VmoShmRuntime (FidlVmoHandleClient), reads data, subscribes in the controls.
//   --sync:   "pong" side of the mutex/condvar scenario in a VMO mapped by both processes.

namespace
{
namespace harness = score::mw::com::impl::lola::fuchsia::harness;
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

constexpr std::uint64_t kApplicationId{77U};
constexpr std::uint32_t kFirstEndpointHandle = PA_HND(PA_USER0, 0);
constexpr int kEndpointCount{4};
constexpr std::uint32_t kSyncVmoHandle = PA_HND(PA_USER0, 10);
constexpr int kRounds{300};
constexpr int kIncrements{4000};

struct SyncBlock
{
    SyncBlock() : mutex{}, cv{}, turn{0U}, rounds_done{0U}, counter{0U} {}

    score::os::InterprocessMutex mutex;
    score::os::InterprocessConditionalVariable cv;
    std::uint32_t turn;        // 0: parent moves, 1: child moves (guarded by mutex)
    std::uint32_t rounds_done;  // guarded by mutex
    std::uint64_t counter;     // plain increments under the mutex, with a yield in the critical section
};

bool WaitForExit(zx::process& process, std::int64_t& code)
{
    if (process.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(90)), nullptr) != ZX_OK)
    {
        return false;
    }
    zx_info_process_t info{};
    if (process.get_info(ZX_INFO_PROCESS, &info, sizeof(info), nullptr, nullptr) != ZX_OK)
    {
        return false;
    }
    code = info.return_code;
    return true;
}

std::optional<zx::process> Spawn(const char* const mode,
                                 std::vector<fdio_spawn_action_t>& actions)
{
    const char* const argv[] = {"/pkg/bin/lola_shm_test", mode, nullptr};
    zx_handle_t process = ZX_HANDLE_INVALID;
    char error[FDIO_SPAWN_ERR_MSG_MAX_LENGTH] = {};
    const zx_status_t status = fdio_spawn_etc(ZX_HANDLE_INVALID,
                                              FDIO_SPAWN_CLONE_ALL,
                                              argv[0],
                                              argv,
                                              nullptr,
                                              actions.size(),
                                              actions.data(),
                                              &process,
                                              error);
    if (status != ZX_OK)
    {
        std::printf("FAIL spawn %s: %d %s\n", mode, status, error);
        ++failures;
        return std::nullopt;
    }
    return zx::process{process};
}

SyncBlock* MapBlock(const zx::vmo& vmo)
{
    zx_vaddr_t address = 0;
    if (zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0U, vmo, 0U, sizeof(SyncBlock), &address) !=
        ZX_OK)
    {
        return nullptr;
    }
    return reinterpret_cast<SyncBlock*>(address);
}

void IncrementUnderLock(SyncBlock& block)
{
    for (int i = 0; i < kIncrements; ++i)
    {
        std::lock_guard<score::os::InterprocessMutex> guard{block.mutex};
        const std::uint64_t value = block.counter;
        if ((i % 8) == 0)
        {
            zx_thread_legacy_yield(0U);
        }
        block.counter = value + 1U;
    }
}

// Both sides run the same loop; `me` is the side that moves when turn == me.
void PingPong(SyncBlock& block, const std::uint32_t me)
{
    for (int i = 0; i < kRounds; ++i)
    {
        std::unique_lock<score::os::InterprocessMutex> lock{block.mutex};
        while (block.turn != me)
        {
            block.cv.wait(lock);
        }
        ++block.rounds_done;
        block.turn = 1U - me;
        block.cv.notify_all();
    }
}

int RunSync()
{
    zx::vmo vmo{zx_take_startup_handle(kSyncVmoHandle)};
    CHECK(vmo.is_valid());
    SyncBlock* const block = vmo.is_valid() ? MapBlock(vmo) : nullptr;
    CHECK(block != nullptr);
    if (block == nullptr)
    {
        return 1;
    }
    IncrementUnderLock(*block);
    PingPong(*block, 1U);
    return failures == 0 ? 0 : 2;
}

int RunProxy()
{
    std::vector<fidl::ClientEnd<score_mw_com::Endpoint>> endpoints;
    for (int i = 0; i < kEndpointCount; ++i)
    {
        zx::channel channel{zx_take_startup_handle(kFirstEndpointHandle + static_cast<std::uint32_t>(i) * 0x10000U)};
        CHECK(channel.is_valid());
        endpoints.emplace_back(std::move(channel));
    }
    // Every (re-)attach of the runtime connects once more; hand out the pre-created channels in order.
    std::size_t next = 0;
    async::Loop loop{&kAsyncLoopConfigNeverAttachToThread};
    VmoShmRuntime runtime{loop.dispatcher(), [&]() -> std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> {
                              if (next >= endpoints.size())
                              {
                                  return std::nullopt;
                              }
                              return std::move(endpoints[next++]);
                          }};

    harness::ProxySide qm;
    harness::ProxySide asil_b;
    CHECK(qm.Open(harness::Quality::kQm));
    CHECK(asil_b.Open(harness::Quality::kAsilB));
    if (failures != 0)
    {
        return 3;
    }

    CHECK(qm.SkeletonPid() == harness::kSkeletonPid);
    CHECK(asil_b.SkeletonPid() == harness::kSkeletonPid);
    harness::SampleData sample{};
    CHECK(qm.ReadSample(sample));
    CHECK(sample.magic == harness::kSampleMagic);
    CHECK(sample.counter == 99U);
    CHECK(std::strcmp(sample.text, "from the skeleton process") == 0);
    harness::SampleData sample_b{};
    CHECK(asil_b.ReadSample(sample_b));
    CHECK(sample_b.counter == 99U);

    // The proxy's data handle is read-only (checked on the handle itself: the factory serves repeated opens of a path
    // from its resource map, regardless of the requested mode).
    {
        score::mw::com::impl::lola::fuchsia::FidlVmoHandleClient client{std::move(endpoints.back())};
        auto data_vmo = client.Acquire(harness::DataPathOf());
        CHECK(data_vmo.has_value());
        if (data_vmo.has_value())
        {
            zx_info_handle_basic_t info{};
            CHECK(data_vmo.value().get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr) == ZX_OK);
            CHECK((info.rights & ZX_RIGHT_READ) != 0U);
            CHECK((info.rights & ZX_RIGHT_WRITE) == 0U);
        }
    }

    // Control regions are writable for the proxy: occupy all slots in both, the skeleton must see that.
    CHECK(qm.Subscribe(harness::kSlots) == 0);
    CHECK(asil_b.Subscribe(harness::kSlots) == 0);
    CHECK(qm.Subscribe(1U) == 2);

    return failures == 0 ? 0 : 4;
}

int RunParent()
{
    // 1. No flock(2): LockFile markers work through the Fuchsia default LockProvider.
    {
        auto created = score::memory::shared::LockFile::Create("/lola/lola_shm_test-marker");
        CHECK(created.has_value());
        CHECK(!score::memory::shared::LockFile::Create("/lola/lola_shm_test-marker").has_value());
        auto other = score::memory::shared::LockFile::CreateOrOpen("/lola/lola_shm_test-marker", false);
        CHECK(other.has_value());
        score::memory::shared::ExclusiveFlockMutex a{created.value()};
        score::memory::shared::ExclusiveFlockMutex b{other.value()};
        CHECK(a.try_lock());
        CHECK(!b.try_lock());
        a.unlock();
        CHECK(b.try_lock());
        b.unlock();
    }

    // 2. LoLa skeleton regions through SharedMemoryFactory -> VMO factory, served over Attach.
    async::Loop loop{&kAsyncLoopConfigNeverAttachToThread};
    CHECK(loop.StartThread("attach-server") == ZX_OK);
    auto runtime = std::make_unique<VmoShmRuntime>(
        loop.dispatcher(), []() -> std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> { return std::nullopt; });
    runtime->server().SetConsumerIntegrity(kApplicationId, score::mw::com::impl::lola::fuchsia::Integrity::kAsilB);
    {
        harness::SkeletonSide skeleton{true};
        harness::SampleData sample{};
        sample.magic = harness::kSampleMagic;
        sample.counter = 99U;
        std::strcpy(sample.text, "from the skeleton process");
        CHECK(skeleton.CreateAndPublish(sample));
        std::printf("regions: %s %s %s\n",
                    skeleton.DataPath().c_str(),
                    skeleton.ControlPath(harness::Quality::kQm).c_str(),
                    skeleton.ControlPath(harness::Quality::kAsilB).c_str());

        std::vector<fdio_spawn_action_t> actions;
        for (int i = 0; i < kEndpointCount; ++i)
        {
            auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
            CHECK(endpoints.is_ok());
            if (!endpoints.is_ok())
            {
                return 1;
            }
            runtime->server().CreateHandler(kApplicationId)(std::move(endpoints->server));
            fdio_spawn_action_t action{};
            action.action = FDIO_SPAWN_ACTION_ADD_HANDLE;
            action.h.id = kFirstEndpointHandle + static_cast<std::uint32_t>(i) * 0x10000U;
            action.h.handle = endpoints->client.TakeChannel().release();
            actions.push_back(action);
        }
        if (failures == 0)
        {
            auto proxy = Spawn("--proxy", actions);
            std::int64_t code = -1;
            CHECK(proxy.has_value() && WaitForExit(*proxy, code));
            std::printf("proxy return code=%ld\n", static_cast<long>(code));
            CHECK(code == 0);
        }

        // The proxy's subscriptions live in the shared control regions and are visible to the skeleton.
        CHECK(skeleton.ProbeSubscribe(harness::Quality::kQm, 1U) == 2);
        CHECK(skeleton.ProbeSubscribe(harness::Quality::kAsilB, 1U) == 2);
        skeleton.Remove();
    }
    runtime.reset();
    loop.Shutdown();

    // 3. InterprocessMutex / InterprocessConditionalVariable in a shared VMO mapping.
    zx::vmo vmo;
    CHECK(zx::vmo::create(zx_system_get_page_size(), 0U, &vmo) == ZX_OK);
    SyncBlock* const block = vmo.is_valid() ? MapBlock(vmo) : nullptr;
    CHECK(block != nullptr);
    if (block == nullptr)
    {
        return 2;
    }
    new (block) SyncBlock{};
    block->turn = 0U;

    zx::vmo child_vmo;
    CHECK(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &child_vmo) == ZX_OK);
    std::vector<fdio_spawn_action_t> sync_actions(1);
    sync_actions[0].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    sync_actions[0].h.id = kSyncVmoHandle;
    sync_actions[0].h.handle = child_vmo.release();
    auto sync = Spawn("--sync", sync_actions);
    CHECK(sync.has_value());
    if (sync.has_value())
    {
        IncrementUnderLock(*block);
        PingPong(*block, 0U);
        std::int64_t code = -1;
        CHECK(WaitForExit(*sync, code));
        std::printf("sync return code=%ld counter=%lu rounds=%u\n",
                    static_cast<long>(code),
                    static_cast<unsigned long>(block->counter),
                    block->rounds_done);
        CHECK(code == 0);
        CHECK(block->counter == 2U * kIncrements);
        CHECK(block->rounds_done == 2U * kRounds);
    }
    return failures == 0 ? 0 : 5;
}
}  // namespace

int main(int argc, char** argv)
{
    const char* const mode = argc > 1 ? argv[1] : "";
    int result{};
    if (std::strcmp(mode, "--proxy") == 0)
    {
        result = RunProxy();
    }
    else if (std::strcmp(mode, "--sync") == 0)
    {
        result = RunSync();
    }
    else
    {
        result = RunParent();
    }
    std::printf("LOLA_SHM %s %s (result=%d, failures=%d)\n",
                mode[0] == '\0' ? "parent" : mode,
                result == 0 ? "PASS" : "FAIL",
                result,
                failures);
    return result;
}
