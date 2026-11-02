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
#include "score/memory/shared/lock_provider/registry_lock_provider.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>

namespace score::memory::shared
{

namespace
{

using Operation = score::os::Fcntl::Operation;

constexpr std::chrono::milliseconds kMaxPollInterval{20};

bool HasFlag(const Operation operation, const Operation flag) noexcept
{
    return (static_cast<std::uint32_t>(operation) & static_cast<std::uint32_t>(flag)) != 0U;
}

}  // namespace

RegistryLockProvider::RegistryLockProvider(std::shared_ptr<ILockRegistry> registry) noexcept
    : ILockProvider{}, registry_{std::move(registry)}, mutex_{}, slots_{}, next_descriptor_{1}
{
}

auto RegistryLockProvider::AddSlot(const std::string& name) -> Descriptor
{
    const std::lock_guard<std::mutex> guard{mutex_};
    const Descriptor descriptor = next_descriptor_++;
    slots_[descriptor] = Slot{name, nullptr};
    return descriptor;
}

auto RegistryLockProvider::Create(const std::string& path) noexcept -> score::cpp::expected<Descriptor, score::os::Error>
{
    const auto result = registry_->CreateName(path, true);
    if (!result.has_value())
    {
        return score::cpp::make_unexpected(result.error());
    }
    return AddSlot(path);
}

auto RegistryLockProvider::CreateOrOpen(const std::string& path) noexcept
    -> score::cpp::expected<Descriptor, score::os::Error>
{
    const auto result = registry_->CreateName(path, false);
    if (!result.has_value())
    {
        return score::cpp::make_unexpected(result.error());
    }
    return AddSlot(path);
}

auto RegistryLockProvider::Open(const std::string& path) noexcept -> score::cpp::expected<Descriptor, score::os::Error>
{
    if (!registry_->NameExists(path))
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
    }
    return AddSlot(path);
}

void RegistryLockProvider::Close(const Descriptor descriptor) noexcept
{
    Slot removed;
    {
        const std::lock_guard<std::mutex> guard{mutex_};
        const auto it = slots_.find(descriptor);
        if (it == slots_.end())
        {
            return;
        }
        removed = std::move(it->second);
        slots_.erase(it);
    }
    // `removed` releases its token here, outside of the provider mutex.
}

void RegistryLockProvider::Remove(const std::string& path) noexcept
{
    registry_->RemoveName(path);
}

auto RegistryLockProvider::Flock(const Descriptor descriptor, const Operation operation) noexcept
    -> score::cpp::expected_blank<score::os::Error>
{
    std::string name;
    std::unique_ptr<ILockToken> previous;
    {
        const std::lock_guard<std::mutex> guard{mutex_};
        const auto it = slots_.find(descriptor);
        if (it == slots_.end())
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrnoFlockSpecific(EBADF));
        }
        name = it->second.name;
        // Like flock(2), a new request on a descriptor first drops what the descriptor holds (not atomic).
        previous = std::move(it->second.token);
    }
    previous.reset();

    if (HasFlag(operation, Operation::kUnLock))
    {
        return {};
    }
    const bool exclusive = HasFlag(operation, Operation::kLockExclusive);
    if (!exclusive && !HasFlag(operation, Operation::kLockShared))
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrnoFlockSpecific(EINVAL));
    }
    const auto kind = exclusive ? LockKind::kExclusive : LockKind::kShared;
    const bool blocking = !HasFlag(operation, Operation::kLockNB);

    std::chrono::milliseconds interval{1};
    while (true)
    {
        auto acquired = registry_->TryAcquire(name, kind);
        if (!acquired.has_value())
        {
            return score::cpp::make_unexpected(acquired.error());
        }
        if (acquired.value() != nullptr)
        {
            const std::lock_guard<std::mutex> guard{mutex_};
            const auto it = slots_.find(descriptor);
            if (it != slots_.end())
            {
                it->second.token = std::move(acquired).value();
            }
            return {};
        }
        if (!blocking)
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrnoFlockSpecific(EWOULDBLOCK));
        }
        std::this_thread::sleep_for(interval);
        interval = std::min(interval * 2, kMaxPollInterval);
    }
}

}  // namespace score::memory::shared
