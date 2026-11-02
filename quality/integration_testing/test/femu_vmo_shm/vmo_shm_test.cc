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
#include "score/memory/shared/fuchsia/i_vmo_handle_provider.h"
#include "score/memory/shared/fuchsia/vmo_shared_memory_factory.h"
#include "score/memory/shared/fuchsia/vmo_shared_memory_resource.h"
#include "score/memory/shared/shared_memory_factory.h"
#include "score/memory/shared/vector.h"

#include <lib/fdio/spawn.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>
#include <lib/zx/vmo.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>

// Skeleton side (parent) creates a VMO backed shared memory resource, puts a struct, an atomic and an OffsetPtr based
// vector into it and hands two reduced-rights handles to a second process (child, same binary, argument "--child"),
// which maps the region read-only and checks that it cannot escalate its rights.

namespace
{
using score::memory::shared::ISharedMemoryResource;
using score::memory::shared::fuchsia::IVmoHandleProvider;
using score::memory::shared::fuchsia::VmoSharedMemoryFactory;
using score::memory::shared::fuchsia::VmoSharedMemoryResource;

constexpr const char* kPath = "/vmo_shm_test";
constexpr std::uint32_t kMagic = 0x564d4f31U;
constexpr std::uint64_t kInitialCounter = 42U;
constexpr std::uint64_t kUpdatedCounter = 100U;

struct Root
{
    explicit Root(score::memory::shared::ManagedMemoryResource& resource)
        : magic{kMagic},
          counter{kInitialCounter},
          values{score::memory::shared::PolymorphicOffsetPtrAllocator<std::uint32_t>{resource}}
    {
    }
    std::uint32_t magic;
    std::atomic<std::uint64_t> counter;
    score::memory::shared::Vector<std::uint32_t> values;
};

class MapHandleProvider final : public IVmoHandleProvider
{
  public:
    score::cpp::expected<zx::vmo, score::os::Error> Acquire(const std::string& key) noexcept override
    {
        const auto it = handles_.find(key);
        if (it == handles_.end())
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
        }
        zx::vmo handle = std::move(it->second);
        handles_.erase(it);
        return handle;
    }
    score::cpp::expected_blank<score::os::Error> Publish(const std::string& key, zx::vmo consumer_handle) noexcept override
    {
        handles_[key] = std::move(consumer_handle);
        return {};
    }
    void Withdraw(const std::string& key) noexcept override
    {
        handles_.erase(key);
    }

  private:
    std::map<std::string, zx::vmo> handles_;
};

int failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

#define CHECK_STATUS(expr, expected)                                                                     \
    do                                                                                                   \
    {                                                                                                    \
        const zx_status_t s_ = (expr);                                                                   \
        std::printf("CHECK %s -> %d\n", #expr, s_);                                                      \
        if (s_ != (expected))                                                                            \
        {                                                                                                \
            std::printf("FAIL %s:%d: %s returned %d, expected %d\n", __FILE__, __LINE__, #expr, s_, expected); \
            ++failures;                                                                                  \
        }                                                                                                \
    } while (false)

int RunChild()
{
    zx::vmo resource_handle{zx_take_startup_handle(PA_HND(PA_USER0, 0))};
    zx::vmo raw_handle{zx_take_startup_handle(PA_HND(PA_USER0, 1))};
    CHECK(resource_handle.is_valid());
    CHECK(raw_handle.is_valid());

    // The handle arrives with TRANSFER (needed to send it) ...
    zx_info_handle_basic_t info{};
    CHECK_STATUS(raw_handle.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr), ZX_OK);
    CHECK(info.rights == VmoSharedMemoryResource::kTransferRights);
    // ... which the receiver drops: what remains is exactly the consumer rights.
    CHECK(VmoSharedMemoryResource::RestrictRights(raw_handle, false).has_value());
    CHECK_STATUS(raw_handle.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr), ZX_OK);
    std::printf("child handle rights=0x%x expected=0x%x\n", info.rights, VmoSharedMemoryResource::kConsumerRights);
    CHECK(info.rights == VmoSharedMemoryResource::kConsumerRights);
    CHECK((info.rights & (ZX_RIGHT_WRITE | ZX_RIGHT_TRANSFER | ZX_RIGHT_DUPLICATE | ZX_RIGHT_EXECUTE)) == 0U);

    // A write through the read-only handle fails, in every way one can write.
    const std::uint32_t garbage = 0xdeadbeefU;
    CHECK_STATUS(raw_handle.write(&garbage, 0U, sizeof(garbage)), ZX_ERR_ACCESS_DENIED);
    zx_vaddr_t address{};
    CHECK_STATUS(zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0U, raw_handle.get(), 0U, 4096U, &address),
                 ZX_ERR_ACCESS_DENIED);
    CHECK_STATUS(zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_EXECUTE, 0U, raw_handle.get(), 0U, 4096U, &address),
                 ZX_ERR_ACCESS_DENIED);
    // Not resizable (and no WRITE right anyway).
    CHECK(raw_handle.set_size(1U << 20U) != ZX_OK);

    // Re-duplicating with TRANSFER (or any other right) fails: the handle has no DUPLICATE right.
    zx::vmo copy;
    CHECK_STATUS(raw_handle.duplicate(VmoSharedMemoryResource::kConsumerRights | ZX_RIGHT_TRANSFER, &copy), ZX_ERR_ACCESS_DENIED);
    CHECK_STATUS(raw_handle.duplicate(ZX_RIGHT_SAME_RIGHTS, &copy), ZX_ERR_ACCESS_DENIED);
    CHECK_STATUS(raw_handle.duplicate(ZX_RIGHT_TRANSFER, &copy), ZX_ERR_ACCESS_DENIED);

    // Opening read/write through the library fails as well.
    auto rw = VmoSharedMemoryResource::Open(std::string{kPath}, std::move(raw_handle), true);
    CHECK(!rw.has_value());

    // Read-only open through the factory seam.
    auto provider = std::make_shared<MapHandleProvider>();
    CHECK(provider->Publish(kPath, std::move(resource_handle)).has_value());
    VmoSharedMemoryFactory factory{provider};
    auto resource = factory.Open(kPath, false, std::nullopt);
    CHECK(resource != nullptr);
    if (resource == nullptr)
    {
        return 10;
    }
    auto* const vmo_resource = dynamic_cast<VmoSharedMemoryResource*>(resource.get());
    CHECK(vmo_resource != nullptr && !vmo_resource->IsWritable());
    // The proxy cannot hand the handle on either.
    CHECK(vmo_resource != nullptr && !vmo_resource->DuplicateConsumerHandle().has_value());

    const auto* const root = static_cast<const Root*>(resource->getUsableBaseAddress());
    CHECK(root->magic == kMagic);
    CHECK(root->counter.load() >= kInitialCounter);
    CHECK(root->values.size() == 4U);
    CHECK(root->values.size() == 4U && root->values[0] == 1U && root->values[3] == 4U);
    CHECK(resource->GetUserAllocatedBytes() >= sizeof(Root) + 4U * sizeof(std::uint32_t));

    // The parent updates the atomic after spawning: the child observes it through the shared mapping.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (root->counter.load() != kUpdatedCounter && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(root->counter.load() == kUpdatedCounter);

    std::printf("child done, failures=%d\n", failures);
    return failures == 0 ? 0 : 20;
}

int RunParent()
{
    auto provider = std::make_shared<MapHandleProvider>();
    VmoSharedMemoryFactory factory{provider};
    score::memory::shared::SharedMemoryFactory::InjectMock(&factory);

    Root* root = nullptr;
    auto resource = score::memory::shared::SharedMemoryFactory::Create(
        kPath,
        [&root](std::shared_ptr<ISharedMemoryResource> r) {
            root = r->construct<Root>(*r);
            for (std::uint32_t i = 1U; i <= 4U; ++i)
            {
                root->values.push_back(i);
            }
        },
        4096U);
    CHECK(resource != nullptr);
    if (resource == nullptr)
    {
        return 1;
    }
    CHECK(root == resource->getUsableBaseAddress());
    CHECK(root->values.size() == 4U);

    auto* const vmo_resource = dynamic_cast<VmoSharedMemoryResource*>(resource.get());
    CHECK(vmo_resource != nullptr);
    CHECK(score::memory::shared::SharedMemoryFactory::Create(
              kPath, [](std::shared_ptr<ISharedMemoryResource>) {}, 4096U) == nullptr);

    auto resource_handle = provider->Acquire(kPath);
    auto raw_handle = vmo_resource->DuplicateTransferHandle();
    CHECK(resource_handle.has_value());
    CHECK(raw_handle.has_value());
    if (!resource_handle.has_value() || !raw_handle.has_value())
    {
        return 2;
    }

    // A handle without TRANSFER cannot be sent over a channel at all.
    {
        auto local = vmo_resource->DuplicateConsumerHandle();
        CHECK(local.has_value());
        zx::channel a;
        zx::channel b;
        CHECK_STATUS(zx::channel::create(0U, &a, &b), ZX_OK);
        zx_handle_t h = local.value().release();
        CHECK_STATUS(a.write(0U, nullptr, 0U, &h, 1U), ZX_ERR_ACCESS_DENIED);
        zx_handle_close(h);
    }

    fdio_spawn_action_t actions[2] = {};
    actions[0].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    actions[0].h.id = PA_HND(PA_USER0, 0);
    actions[0].h.handle = resource_handle.value().release();
    actions[1].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    actions[1].h.id = PA_HND(PA_USER0, 1);
    actions[1].h.handle = raw_handle.value().release();

    const char* const argv[] = {"/pkg/bin/vmo_shm_test", "--child", nullptr};
    zx_handle_t process = ZX_HANDLE_INVALID;
    char error[FDIO_SPAWN_ERR_MSG_MAX_LENGTH] = {};
    const zx_status_t spawn_status = fdio_spawn_etc(
        ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, argv[0], argv, nullptr, 2U, actions, &process, error);
    std::printf("spawn status=%d %s\n", spawn_status, error);
    CHECK(spawn_status == ZX_OK);
    if (spawn_status != ZX_OK)
    {
        return 3;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    root->counter.store(kUpdatedCounter);

    zx::process child{process};
    CHECK_STATUS(child.wait_one(ZX_PROCESS_TERMINATED, zx::deadline_after(zx::sec(40)), nullptr), ZX_OK);
    zx_info_process_t child_info{};
    CHECK_STATUS(child.get_info(ZX_INFO_PROCESS, &child_info, sizeof(child_info), nullptr, nullptr), ZX_OK);
    std::printf("child return code=%ld\n", static_cast<long>(child_info.return_code));
    CHECK(child_info.return_code == 0);

    score::memory::shared::SharedMemoryFactory::Remove(kPath);
    score::memory::shared::SharedMemoryFactory::InjectMock(nullptr);
    return failures == 0 ? 0 : 4;
}
}  // namespace

int main(int argc, char** argv)
{
    const bool is_child = argc > 1 && std::strcmp(argv[1], "--child") == 0;
    const int result = is_child ? RunChild() : RunParent();
    std::printf("VMO_SHM_TEST %s %s (result=%d, failures=%d)\n", is_child ? "child" : "parent", result == 0 ? "PASS" : "FAIL", result, failures);
    return result;
}
