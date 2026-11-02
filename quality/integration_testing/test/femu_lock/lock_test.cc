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
#include "score/memory/shared/flock/shared_flock_mutex.h"
#include "score/memory/shared/fuchsia/lock_registry_channel.h"
#include "score/memory/shared/lock_file.h"
#include "score/memory/shared/lock_provider/in_process_lock_registry.h"
#include "score/memory/shared/lock_provider/lock_provider.h"
#include "score/memory/shared/lock_provider/registry_lock_provider.h"

#include <lib/fdio/spawn.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>

// The LoLa markers (existence marker: one skeleton per instance, usage marker: proxies shared / restarting skeleton
// exclusive) on Fuchsia, where flock(2) fails with ENOTSUP. The parent hosts the lock registry (stand-in for the
// broker) and serves it over channels to child processes of this binary.
//   parent mode:         runs the scenarios
//   --probe:             remote client, checks exclusion against locks held by the parent
//   --hold-and-die:      remote client, takes the usage lock, reports, waits for the parent, then dies without
//                        releasing (arg: "exit" or "trap")

namespace
{
using score::memory::shared::ExclusiveFlockMutex;
using score::memory::shared::LockFile;
using score::memory::shared::LockProvider;
using score::memory::shared::RegistryLockProvider;
using score::memory::shared::SharedFlockMutex;
using score::memory::shared::fuchsia::ChannelLockRegistryClient;
using score::memory::shared::fuchsia::LockRegistryServer;

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

constexpr const char* kExistence = "/lola/existence-1";
constexpr const char* kUsage = "/lola/usage-1";
constexpr const char* kSharedOnly = "/lola/usage-2";

constexpr std::uint32_t kRegistryHandle = PA_HND(PA_USER0, 0);
constexpr std::uint32_t kControlHandle = PA_HND(PA_USER0, 1);

struct Child
{
    zx::process process;
    zx::channel control;
    std::unique_ptr<LockRegistryServer> server;
};

std::optional<Child> Spawn(const std::shared_ptr<score::memory::shared::ILockRegistry>& registry, const char* const mode, const char* const arg = "")
{
    zx::channel server_end;
    zx::channel client_end;
    zx::channel control_parent;
    zx::channel control_child;
    if (zx::channel::create(0U, &server_end, &client_end) != ZX_OK || zx::channel::create(0U, &control_parent, &control_child) != ZX_OK)
    {
        return std::nullopt;
    }
    Child child;
    child.server = std::make_unique<LockRegistryServer>(registry, std::move(server_end));
    child.control = std::move(control_parent);

    fdio_spawn_action_t actions[2] = {};
    actions[0].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    actions[0].h.id = kRegistryHandle;
    actions[0].h.handle = client_end.release();
    actions[1].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    actions[1].h.id = kControlHandle;
    actions[1].h.handle = control_child.release();

    const char* const argv[] = {"/pkg/bin/lock_test", mode, arg, nullptr};
    zx_handle_t process = ZX_HANDLE_INVALID;
    char error[FDIO_SPAWN_ERR_MSG_MAX_LENGTH] = {};
    const zx_status_t status =
        fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, argv[0], argv, nullptr, 2U, actions, &process, error);
    if (status != ZX_OK)
    {
        std::printf("spawn failed %d %s\n", status, error);
        return std::nullopt;
    }
    child.process.reset(process);
    return child;
}

bool WaitForTerminationAndGetCode(zx::process& process, std::int64_t& code)
{
    if (process.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(40)), nullptr) != ZX_OK)
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

// Polls because the release after a death is driven by the kernel closing the handle and the server thread reacting.
template <typename Fn>
bool EventuallyTrue(Fn&& fn)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (fn())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::unique_ptr<RegistryLockProvider> MakeRemoteProvider()
{
    zx::channel client_end{zx_take_startup_handle(kRegistryHandle)};
    if (!client_end.is_valid())
    {
        return nullptr;
    }
    return std::make_unique<RegistryLockProvider>(std::make_shared<ChannelLockRegistryClient>(std::move(client_end)));
}

int RunProbe()
{
    auto provider = MakeRemoteProvider();
    CHECK(provider != nullptr);
    if (provider == nullptr)
    {
        return 1;
    }
    LockProvider::Inject(provider.get());

    // The parent skeleton holds the existence marker exclusively: a second skeleton must not get it.
    auto existence = LockFile::CreateOrOpen(kExistence, false);
    CHECK(existence.has_value());
    ExclusiveFlockMutex existence_mutex{existence.value()};
    CHECK(!existence_mutex.try_lock());

    // The parent skeleton holds the usage marker exclusively: a proxy cannot take it shared either.
    auto usage = LockFile::Open(kUsage);
    CHECK(usage.has_value());
    SharedFlockMutex usage_shared{usage.value()};
    CHECK(!usage_shared.try_lock());

    // The parent holds a shared lock: another shared one works, exclusive does not.
    auto shared_only = LockFile::Open(kSharedOnly);
    CHECK(shared_only.has_value());
    SharedFlockMutex shared_ok{shared_only.value()};
    CHECK(shared_ok.try_lock());
    ExclusiveFlockMutex shared_blocked{shared_only.value()};
    CHECK(!shared_blocked.try_lock());
    shared_ok.unlock();

    // Opening a marker nobody created fails (proxy before skeleton).
    CHECK(!LockFile::Open("/lola/does-not-exist").has_value());

    LockProvider::Inject(nullptr);
    return failures == 0 ? 0 : 2;
}

int RunHoldAndDie(const char* const how)
{
    auto provider = MakeRemoteProvider();
    zx::channel control{zx_take_startup_handle(kControlHandle)};
    CHECK(provider != nullptr);
    CHECK(control.is_valid());
    if (provider == nullptr || !control.is_valid())
    {
        return 1;
    }
    LockProvider::Inject(provider.get());

    auto usage = LockFile::CreateOrOpen(kUsage, false);
    CHECK(usage.has_value());
    ExclusiveFlockMutex mutex{usage.value()};
    CHECK(mutex.try_lock());
    if (failures != 0)
    {
        return 3;
    }
    const char held = 'H';
    CHECK(control.write(0U, &held, 1U, nullptr, 0U) == ZX_OK);
    CHECK(control.wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, zx::deadline_after(zx::sec(30)), nullptr) == ZX_OK);

    std::printf("child dying with the lock held (%s)\n", how);
    std::fflush(stdout);
    if (std::strcmp(how, "trap") == 0)
    {
        __builtin_trap();
    }
    // No destructors, no unlock: the process just goes away.
    zx_process_exit(42);
    return 42;
}

int RunParent()
{
    // 1. Without any injection the Fuchsia default provider works (flock itself would fail with ENOTSUP).
    {
        auto created = LockFile::Create("/lola/default-marker");
        CHECK(created.has_value());
        auto other = LockFile::CreateOrOpen("/lola/default-marker", false);
        CHECK(other.has_value());
        CHECK(!LockFile::Create("/lola/default-marker").has_value());
        ExclusiveFlockMutex a{created.value()};
        ExclusiveFlockMutex b{other.value()};
        CHECK(a.try_lock());
        CHECK(!b.try_lock());
        a.unlock();
        CHECK(b.try_lock());
    }

    // 2. Cross-process: the parent is the skeleton holding the markers, children are clients of its registry.
    auto registry = std::make_shared<score::memory::shared::InProcessLockRegistry>();
    RegistryLockProvider provider{registry};
    LockProvider::Inject(&provider);

    auto existence = LockFile::CreateOrOpen(kExistence, false);
    auto usage = LockFile::CreateOrOpen(kUsage, false);
    auto shared_only = LockFile::CreateOrOpen(kSharedOnly, false);
    CHECK(existence.has_value() && usage.has_value() && shared_only.has_value());
    if (failures != 0)
    {
        return 1;
    }
    ExclusiveFlockMutex existence_mutex{existence.value()};
    ExclusiveFlockMutex usage_mutex{usage.value()};
    SharedFlockMutex shared_mutex{shared_only.value()};
    CHECK(existence_mutex.try_lock());
    CHECK(usage_mutex.try_lock());
    CHECK(shared_mutex.try_lock());

    auto probe = Spawn(registry, "--probe");
    CHECK(probe.has_value());
    if (probe.has_value())
    {
        std::int64_t code = -1;
        CHECK(WaitForTerminationAndGetCode(probe->process, code));
        std::printf("probe return code=%ld\n", static_cast<long>(code));
        CHECK(code == 0);
    }

    // 3. The skeleton lets go of the usage marker, a child takes it and dies while holding it.
    usage_mutex.unlock();
    for (const char* const how : {"exit", "trap"})
    {
        auto holder = Spawn(registry, "--hold-and-die", how);
        CHECK(holder.has_value());
        if (!holder.has_value())
        {
            break;
        }
        CHECK(holder->control.wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, zx::deadline_after(zx::sec(30)), nullptr) == ZX_OK);
        char byte = 0;
        std::uint32_t actual = 0;
        CHECK(holder->control.read(0U, &byte, nullptr, 1U, 0U, &actual, nullptr) == ZX_OK && byte == 'H');

        // While the child holds it, neither exclusive nor shared works here.
        CHECK(!usage_mutex.try_lock());
        SharedFlockMutex proxy_mutex{usage.value()};
        CHECK(!proxy_mutex.try_lock());

        const char go = 'X';
        CHECK(holder->control.write(0U, &go, 1U, nullptr, 0U) == ZX_OK);
        std::int64_t code = -1;
        CHECK(WaitForTerminationAndGetCode(holder->process, code));
        std::printf("holder (%s) return code=%ld\n", how, static_cast<long>(code));
        CHECK(std::strcmp(how, "exit") != 0 || code == 42);

        // The dead holder's handle closed, so the lock is free again (like a flock after a crash).
        CHECK(EventuallyTrue([&] {
            const bool got = usage_mutex.try_lock();
            if (got)
            {
                usage_mutex.unlock();
            }
            return got;
        }));
    }

    LockProvider::Inject(nullptr);
    return failures == 0 ? 0 : 4;
}
}  // namespace

int main(int argc, char** argv)
{
    const char* const mode = argc > 1 ? argv[1] : "";
    int result{};
    if (std::strcmp(mode, "--probe") == 0)
    {
        result = RunProbe();
    }
    else if (std::strcmp(mode, "--hold-and-die") == 0)
    {
        result = RunHoldAndDie(argc > 2 ? argv[2] : "exit");
    }
    else
    {
        result = RunParent();
    }
    std::printf("LOCK_TEST %s %s (result=%d, failures=%d)\n", mode[0] == '\0' ? "parent" : mode, result == 0 ? "PASS" : "FAIL", result, failures);
    return result;
}
