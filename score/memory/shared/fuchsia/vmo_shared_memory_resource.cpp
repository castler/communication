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
#include "score/memory/shared/fuchsia/vmo_shared_memory_resource.h"

#include "score/memory/shared/memory_resource_registry.h"
#include "score/memory/shared/pointer_arithmetic_util.h"
#include "score/memory/shared/shared_memory_resource.h"

#include "score/mw/log/logging.h"

#include <score/assert.hpp>
#include <score/hash.hpp>

#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/rights.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <cerrno>
#include <exception>
#include <new>
#include <utility>

namespace score::memory::shared::fuchsia
{

namespace
{

score::os::Error ErrorFromStatus(const zx_status_t status) noexcept
{
    switch (status)
    {
        case ZX_ERR_ACCESS_DENIED:
            return score::os::Error::createFromErrno(EACCES);
        case ZX_ERR_NOT_FOUND:
            return score::os::Error::createFromErrno(ENOENT);
        case ZX_ERR_NO_MEMORY:
            return score::os::Error::createFromErrno(ENOMEM);
        case ZX_ERR_BAD_HANDLE:
            return score::os::Error::createFromErrno(EBADF);
        default:
            return score::os::Error::createFromErrno(EINVAL);
    }
}

std::string MakeLogIdentification(const VmoSharedMemoryResource::Identifier& identifier)
{
    return std::holds_alternative<std::string>(identifier)
               ? "file: " + std::get<std::string>(identifier)
               : "id: " + std::to_string(std::get<std::uint64_t>(identifier));
}

std::uint64_t MakeMemoryIdentifier(const VmoSharedMemoryResource::Identifier& identifier) noexcept
{
    return std::holds_alternative<std::string>(identifier)
               ? score::cpp::hash_bytes(std::get<std::string>(identifier).data(),
                                        std::get<std::string>(identifier).size())
               : std::get<std::uint64_t>(identifier);
}

std::size_t RoundUpToPage(const std::size_t size) noexcept
{
    const std::size_t page_size = zx_system_get_page_size();
    return ((size + page_size - 1U) / page_size) * page_size;
}

}  // namespace

std::size_t VmoSharedMemoryResource::GetNeededManagementSpace() noexcept
{
    return CalculateAlignedSize(sizeof(ControlBlock), alignof(std::max_align_t));
}

VmoSharedMemoryResource::VmoSharedMemoryResource(Identifier identifier, zx::vmo vmo, const bool writable)
    : vmo_{std::move(vmo)},
      writable_{writable},
      identifier_{std::move(identifier)},
      log_identification_{MakeLogIdentification(identifier_)},
      memory_identifier_{MakeMemoryIdentifier(identifier_)}
{
    // 0 is the "no memory resource" sentinel of OffsetPtr
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD_MESSAGE(memory_identifier_ != 0U, "");
}

VmoSharedMemoryResource::~VmoSharedMemoryResource()
{
    if (registered_)
    {
        MemoryResourceRegistry::getInstance().remove_resource(memory_identifier_);
    }
    if (base_address_ != nullptr)
    {
        zx_vmar_unmap(zx_vmar_root_self(), reinterpret_cast<zx_vaddr_t>(base_address_), mapped_size_);
    }
}

auto VmoSharedMemoryResource::Create(Identifier identifier,
                                     const std::size_t user_space_to_reserve,
                                     InitializeCallback initialize_callback)
    -> score::cpp::expected<std::shared_ptr<VmoSharedMemoryResource>, score::os::Error>
{
    const std::size_t total = RoundUpToPage(user_space_to_reserve + GetNeededManagementSpace());

    // options 0: not resizable, so the size can never change under a consumer's mapping
    zx::vmo vmo;
    const zx_status_t status = zx::vmo::create(total, 0U, &vmo);
    if (status != ZX_OK)
    {
        score::mw::log::LogError("shm") << "zx_vmo_create failed for " << MakeLogIdentification(identifier) << ": "
                                        << status;
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }

    std::shared_ptr<VmoSharedMemoryResource> resource{new VmoSharedMemoryResource{std::move(identifier), std::move(vmo), true}};
    resource->mapped_size_ = total;
    auto mapped = resource->MapAndRegister();
    if (!mapped.has_value())
    {
        return score::cpp::make_unexpected(mapped.error());
    }
    resource->InitializeControlBlock();
    initialize_callback(resource);
    return resource;
}

auto VmoSharedMemoryResource::RestrictRights(zx::vmo& vmo, const bool keep_write) noexcept
    -> score::cpp::expected_blank<score::os::Error>
{
    zx_info_handle_basic_t info{};
    zx_status_t status = vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    const zx_rights_t mask = kConsumerRights | (keep_write ? ZX_RIGHT_WRITE : 0U);
    zx::vmo restricted;
    status = vmo.replace(info.rights & mask, &restricted);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    vmo = std::move(restricted);
    return {};
}

auto VmoSharedMemoryResource::Open(Identifier identifier, zx::vmo vmo, const bool is_read_write)
    -> score::cpp::expected<std::shared_ptr<VmoSharedMemoryResource>, score::os::Error>
{
    auto restricted = RestrictRights(vmo, is_read_write);
    if (!restricted.has_value())
    {
        return score::cpp::make_unexpected(restricted.error());
    }
    zx_info_handle_basic_t info{};
    zx_status_t status = vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    if (info.type != ZX_OBJ_TYPE_VMO || (info.rights & (ZX_RIGHT_READ | ZX_RIGHT_MAP)) != (ZX_RIGHT_READ | ZX_RIGHT_MAP))
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EACCES));
    }
    if (is_read_write && ((info.rights & ZX_RIGHT_WRITE) == 0U))
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EACCES));
    }

    std::uint64_t size{};
    status = vmo.get_size(&size);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    if (size < GetNeededManagementSpace())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EINVAL));
    }

    std::shared_ptr<VmoSharedMemoryResource> resource{
        new VmoSharedMemoryResource{std::move(identifier), std::move(vmo), is_read_write}};
    resource->mapped_size_ = static_cast<std::size_t>(size);
    auto mapped = resource->MapAndRegister();
    if (!mapped.has_value())
    {
        return score::cpp::make_unexpected(mapped.error());
    }
    resource->control_block_ = static_cast<ControlBlock*>(resource->base_address_);
    resource->start_ = AddOffsetToPointer(resource->base_address_, GetNeededManagementSpace());
    return resource;
}

auto VmoSharedMemoryResource::MapAndRegister() noexcept -> score::cpp::expected_blank<score::os::Error>
{
    zx_vm_option_t options = ZX_VM_PERM_READ;
    if (writable_)
    {
        options |= ZX_VM_PERM_WRITE;
    }
    zx_vaddr_t address{};
    const zx_status_t status = zx_vmar_map(zx_vmar_root_self(), options, 0U, vmo_.get(), 0U, mapped_size_, &address);
    if (status != ZX_OK)
    {
        score::mw::log::LogError("shm") << "zx_vmar_map failed for " << log_identification_ << ": " << status;
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    base_address_ = reinterpret_cast<void*>(address);

    if (!MemoryResourceRegistry::getInstance().insert_resource({memory_identifier_, this}))
    {
        score::mw::log::LogFatal("shm") << "Inserting " << log_identification_
                                        << " into MemoryResourceRegistry failed (duplicate id or hash collision).";
        std::terminate();
    }
    registered_ = true;
    return {};
}

auto VmoSharedMemoryResource::InitializeControlBlock() noexcept -> void
{
    // NOLINTNEXTLINE(score-no-dynamic-raw-memory): placement new into the mapping, which is page aligned
    control_block_ = new (base_address_) ControlBlock(memory_identifier_);
    start_ = AddOffsetToPointer(base_address_, GetNeededManagementSpace());
    control_block_->alreadyAllocatedBytes = GetNeededManagementSpace();
}

auto VmoSharedMemoryResource::DuplicateConsumerHandle() const noexcept
    -> score::cpp::expected<zx::vmo, score::os::Error>
{
    zx::vmo duplicate;
    const zx_status_t status = vmo_.duplicate(kConsumerRights, &duplicate);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    return duplicate;
}

auto VmoSharedMemoryResource::DuplicateTransferHandle() const noexcept
    -> score::cpp::expected<zx::vmo, score::os::Error>
{
    zx::vmo duplicate;
    const zx_status_t status = vmo_.duplicate(kTransferRights, &duplicate);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    return duplicate;
}

auto VmoSharedMemoryResource::DuplicatePublishHandle(const bool consumer_writable) const noexcept
    -> score::cpp::expected<zx::vmo, score::os::Error>
{
    zx::vmo duplicate;
    const zx_rights_t rights =
        kTransferRights | ZX_RIGHT_DUPLICATE | (consumer_writable ? static_cast<zx_rights_t>(ZX_RIGHT_WRITE) : 0U);
    const zx_status_t status = vmo_.duplicate(rights, &duplicate);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(ErrorFromStatus(status));
    }
    return duplicate;
}

auto VmoSharedMemoryResource::do_allocate(const std::size_t bytes, const std::size_t alignment) -> void*
{
    if (!writable_)
    {
        score::mw::log::LogFatal("shm") << "Allocation requested on read-only mapping " << log_identification_;
        std::terminate();
    }

    constexpr std::size_t max_retries = 32U;
    void* const allocation_end = AddOffsetToPointer(base_address_, mapped_size_);
    std::size_t already_allocated = control_block_->alreadyAllocatedBytes.load(std::memory_order_acquire);

    for (std::size_t retry = 0U; retry < max_retries; ++retry)
    {
        void* const allocation_start = AddOffsetToPointer(base_address_, already_allocated);
        void* const aligned = detail::do_allocation_algorithm(allocation_start, allocation_end, bytes, alignment);
        if (aligned == nullptr)
        {
            score::mw::log::LogFatal("shm") << "Cannot allocate " << bytes << " bytes (alignment " << alignment
                                            << ") in " << log_identification_ << ". Already allocated: "
                                            << already_allocated << " of " << mapped_size_;
            std::terminate();
        }
        const auto padding = static_cast<std::size_t>(SubtractPointersBytes(aligned, allocation_start));
        const std::size_t new_allocated = already_allocated + padding + bytes;
        if (control_block_->alreadyAllocatedBytes.compare_exchange_weak(
                already_allocated, new_allocated, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return aligned;
        }
    }
    score::mw::log::LogFatal("shm") << "Failed to reserve shared memory after " << max_retries << " attempts.";
    std::terminate();
}

auto VmoSharedMemoryResource::do_is_equal(const memory_resource& other) const noexcept -> bool
{
    return this == &other;
}

auto VmoSharedMemoryResource::getMemoryResourceProxy() noexcept -> const MemoryResourceProxy*
{
    return &control_block_->memoryResourceProxy;
}

auto VmoSharedMemoryResource::getBaseAddress() const noexcept -> void*
{
    return base_address_;
}

auto VmoSharedMemoryResource::getUsableBaseAddress() const noexcept -> void*
{
    return start_;
}

auto VmoSharedMemoryResource::GetUserAllocatedBytes() const noexcept -> std::size_t
{
    return control_block_->alreadyAllocatedBytes.load() - GetNeededManagementSpace();
}

auto VmoSharedMemoryResource::getEndAddress() const noexcept -> const void*
{
    return AddOffsetToPointer(base_address_, mapped_size_);
}

auto VmoSharedMemoryResource::getPath() const noexcept -> const std::string*
{
    return std::get_if<std::string>(&identifier_);
}

auto VmoSharedMemoryResource::GetIdentifier() const noexcept -> std::string_view
{
    return log_identification_;
}

}  // namespace score::memory::shared::fuchsia
