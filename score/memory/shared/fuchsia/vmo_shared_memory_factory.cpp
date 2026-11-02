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
#include "score/memory/shared/fuchsia/vmo_shared_memory_factory.h"

#include "score/memory/shared/fuchsia/vmo_shared_memory_resource.h"

#include "score/mw/log/logging.h"

#include <zircon/rights.h>
#include <zircon/syscalls.h>

#include <utility>

namespace score::memory::shared::fuchsia
{

VmoSharedMemoryFactory::VmoSharedMemoryFactory(std::shared_ptr<IVmoHandleProvider> handle_provider) noexcept
    : ISharedMemoryFactory{}, handle_provider_{std::move(handle_provider)}, mutex_{}, resources_{}
{
}

auto VmoSharedMemoryFactory::KeyForAnonymous(const std::uint64_t id) -> std::string
{
    return "anon:" + std::to_string(id);
}

auto VmoSharedMemoryFactory::Lookup(const std::string& key) noexcept -> std::shared_ptr<ISharedMemoryResource>
{
    const auto it = resources_.find(key);
    if (it == resources_.end())
    {
        return nullptr;
    }
    auto resource = it->second.lock();
    if (resource == nullptr)
    {
        resources_.erase(it);
    }
    return resource;
}

auto VmoSharedMemoryFactory::CreateImpl(const std::string& key,
                                        std::variant<std::string, std::uint64_t> identifier,
                                        InitializeCallback cb,
                                        const std::size_t user_space_to_reserve) noexcept
    -> std::shared_ptr<ISharedMemoryResource>
{
    const std::lock_guard<std::recursive_mutex> lock{mutex_};
    if (Lookup(key) != nullptr)
    {
        score::mw::log::LogError("shm") << "Shared memory " << key << " already exists in this process.";
        return nullptr;
    }
    auto resource = VmoSharedMemoryResource::Create(std::move(identifier), user_space_to_reserve, std::move(cb));
    if (!resource.has_value())
    {
        return nullptr;
    }
    if (handle_provider_ != nullptr)
    {
        auto consumer_handle = resource.value()->DuplicateTransferHandle();
        if (!consumer_handle.has_value() ||
            !handle_provider_->Publish(key, std::move(consumer_handle).value()).has_value())
        {
            score::mw::log::LogError("shm") << "Publishing the consumer handle of " << key << " failed.";
            return nullptr;
        }
    }
    resources_[key] = resource.value();
    return resource.value();
}

auto VmoSharedMemoryFactory::OpenImpl(const std::string& key,
                                      std::variant<std::string, std::uint64_t> identifier,
                                      const std::optional<bool>& is_read_write) noexcept
    -> std::shared_ptr<ISharedMemoryResource>
{
    const std::lock_guard<std::recursive_mutex> lock{mutex_};
    if (auto existing = Lookup(key))
    {
        return existing;
    }
    if (handle_provider_ == nullptr)
    {
        return nullptr;
    }
    auto handle = handle_provider_->Acquire(key);
    if (!handle.has_value())
    {
        return nullptr;
    }
    bool read_write = is_read_write.value_or(false);
    if (!is_read_write.has_value())
    {
        zx_info_handle_basic_t info{};
        if (handle.value().get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr) == ZX_OK)
        {
            read_write = (info.rights & ZX_RIGHT_WRITE) != 0U;
        }
    }
    auto resource = VmoSharedMemoryResource::Open(std::move(identifier), std::move(handle).value(), read_write);
    if (!resource.has_value())
    {
        score::mw::log::LogError("shm") << "Opening shared memory " << key << " failed: " << resource.error().ToString();
        return nullptr;
    }
    resources_[key] = resource.value();
    return resource.value();
}

auto VmoSharedMemoryFactory::Open(const std::string& path,
                                  const bool is_read_write,
                                  const std::optional<score::cpp::span<const uid_t>>&) noexcept
    -> std::shared_ptr<ISharedMemoryResource>
{
    return OpenImpl(path, path, is_read_write);
}

auto VmoSharedMemoryFactory::Create(std::string path,
                                    InitializeCallback cb,
                                    const std::size_t user_space_to_reserve,
                                    const UserPermissions&,
                                    const bool) noexcept -> std::shared_ptr<ISharedMemoryResource>
{
    const std::string key = path;
    return CreateImpl(key, std::move(path), std::move(cb), user_space_to_reserve);
}

auto VmoSharedMemoryFactory::CreateAnonymous(std::uint64_t shared_memory_resource_id,
                                             InitializeCallback cb,
                                             const std::size_t user_space_to_reserve,
                                             const UserPermissions&,
                                             const bool) noexcept -> std::shared_ptr<ISharedMemoryResource>
{
    return CreateImpl(KeyForAnonymous(shared_memory_resource_id),
                      shared_memory_resource_id,
                      std::move(cb),
                      user_space_to_reserve);
}

auto VmoSharedMemoryFactory::CreateOrOpen(std::string path,
                                          InitializeCallback cb,
                                          const std::size_t user_space_to_reserve,
                                          const ISharedMemoryFactory::AccessControl,
                                          const bool) noexcept -> std::shared_ptr<ISharedMemoryResource>
{
    const std::lock_guard<std::recursive_mutex> lock{mutex_};
    if (auto opened = OpenImpl(path, path, std::nullopt))
    {
        return opened;
    }
    const std::string key = path;
    return CreateImpl(key, std::move(path), std::move(cb), user_space_to_reserve);
}

auto VmoSharedMemoryFactory::Remove(const std::string& path) noexcept -> void
{
    const std::lock_guard<std::recursive_mutex> lock{mutex_};
    if (handle_provider_ != nullptr)
    {
        handle_provider_->Withdraw(path);
    }
    resources_.erase(path);
}

auto VmoSharedMemoryFactory::GetControlBlockSize() noexcept -> std::size_t
{
    return VmoSharedMemoryResource::GetNeededManagementSpace();
}

auto VmoSharedMemoryFactory::Clear() noexcept -> void
{
    const std::lock_guard<std::recursive_mutex> lock{mutex_};
    resources_.clear();
}

}  // namespace score::memory::shared::fuchsia
