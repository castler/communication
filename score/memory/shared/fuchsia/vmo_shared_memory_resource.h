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
#ifndef SCORE_LIB_MEMORY_SHARED_FUCHSIA_VMO_SHARED_MEMORY_RESOURCE_H
#define SCORE_LIB_MEMORY_SHARED_FUCHSIA_VMO_SHARED_MEMORY_RESOURCE_H

#include "score/memory/shared/i_shared_memory_resource.h"
#include "score/memory/shared/memory_resource_proxy.h"

#include "score/os/errno.h"

#include <lib/zx/vmo.h>
#include <score/expected.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace score::memory::shared::fuchsia
{

/// \brief ISharedMemoryResource backed by a zircon VMO instead of a POSIX shm object.
/// \details Layout, allocation and OffsetPtr behaviour are identical to SharedMemoryResource: a ControlBlock (bump
///          pointer + MemoryResourceProxy) sits at the start of the mapping, user data follows at a max_align_t
///          aligned address, allocation is monotonic and lock-free.
///
///          Security model (see design/fuchsia/README.md, "shared memory"):
///          - Create() makes a new, non-resizable VMO per incarnation; the creator keeps the only full-rights handle.
///          - Consumers only ever get DuplicateConsumerHandle(): READ|MAP|GET_PROPERTY, i.e. no WRITE, no
///            TRANSFER, no DUPLICATE, no EXECUTE, no SET_PROPERTY. They can neither write, nor hand the handle on, nor
///            resize the region, nor map it executable.
///          - Open() maps whatever the received handle allows; a mapping is writable only if the handle has WRITE.
class VmoSharedMemoryResource final : public ISharedMemoryResource,
                                      public std::enable_shared_from_this<VmoSharedMemoryResource>
{
  public:
    /// Rights of a consumer handle. Deliberately a strict subset of the creator rights.
    static constexpr zx_rights_t kConsumerRights = ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_GET_PROPERTY;
    /// Rights a handle needs while it is *sent* (zx_channel_write / process args require TRANSFER). The receiver drops
    /// TRANSFER again in Open() / RestrictToConsumerRights(), mirroring the rights constraint of a FIDL handle.
    static constexpr zx_rights_t kTransferRights = kConsumerRights | ZX_RIGHT_TRANSFER;

    using Identifier = std::variant<std::string, std::uint64_t>;

    /// \brief Creates a fresh VMO of (at least) control block + user_space_to_reserve bytes, maps it read/write,
    ///        initialises the control block and calls initialize_callback.
    static score::cpp::expected<std::shared_ptr<VmoSharedMemoryResource>, score::os::Error> Create(
        Identifier identifier,
        const std::size_t user_space_to_reserve,
        InitializeCallback initialize_callback);

    /// \brief Maps an existing region from a handle received out-of-band. Takes ownership of the handle and strips all
    ///        rights beyond READ|MAP|GET_PROPERTY(|WRITE) from it first.
    /// \param is_read_write needs ZX_RIGHT_WRITE on the handle, else EACCES.
    static score::cpp::expected<std::shared_ptr<VmoSharedMemoryResource>, score::os::Error> Open(Identifier identifier,
                                                                                                 zx::vmo vmo,
                                                                                                 const bool is_read_write);

    ~VmoSharedMemoryResource() override;
    VmoSharedMemoryResource(const VmoSharedMemoryResource&) = delete;
    VmoSharedMemoryResource& operator=(const VmoSharedMemoryResource&) = delete;
    VmoSharedMemoryResource(VmoSharedMemoryResource&&) = delete;
    VmoSharedMemoryResource& operator=(VmoSharedMemoryResource&&) = delete;

    /// \brief Duplicates the VMO handle with kConsumerRights. Fails (ACCESS_DENIED) on a resource that was itself
    ///        opened from a reduced handle (no DUPLICATE right).
    score::cpp::expected<zx::vmo, score::os::Error> DuplicateConsumerHandle() const noexcept;

    /// \brief Like DuplicateConsumerHandle() but with kTransferRights, i.e. what the skeleton actually sends.
    score::cpp::expected<zx::vmo, score::os::Error> DuplicateTransferHandle() const noexcept;

    /// \brief What the factory publishes to an IVmoHandleProvider that stays in the creating process: kTransferRights
    ///        (plus WRITE if `consumer_writable`, e.g. the control region) and DUPLICATE, so the provider can hand out
    ///        one duplicate per consumer. DUPLICATE must be removed again from every handle that leaves the process.
    score::cpp::expected<zx::vmo, score::os::Error> DuplicatePublishHandle(const bool consumer_writable) const noexcept;

    /// \brief Replaces `vmo` by a handle with `rights & (kConsumerRights | WRITE if keep_write)`: strips TRANSFER,
    ///        DUPLICATE, EXECUTE, SET_PROPERTY. Called by Open() on every received handle.
    static score::cpp::expected_blank<score::os::Error> RestrictRights(zx::vmo& vmo, const bool keep_write) noexcept;

    /// \brief Size of the mapping (page granular).
    std::size_t GetMappedSize() const noexcept
    {
        return mapped_size_;
    }
    bool IsWritable() const noexcept
    {
        return writable_;
    }

    /// \brief Space the resource itself needs in front of the user data.
    static std::size_t GetNeededManagementSpace() noexcept;

    const MemoryResourceProxy* getMemoryResourceProxy() noexcept override;
    void* getBaseAddress() const noexcept override;
    void* getUsableBaseAddress() const noexcept override;
    std::size_t GetUserAllocatedBytes() const noexcept override;

    /// \brief nullptr for anonymous regions.
    const std::string* getPath() const noexcept override;
    std::string_view GetIdentifier() const noexcept override;

    /// \brief No file system entry exists on Fuchsia, nothing to unlink. Withdrawal from the handle provider is done by
    ///        the factory.
    void UnlinkFilesystemEntry() const noexcept override {}

    /// \brief There is no file descriptor, always -1.
    FileDescriptor GetFileDescriptor() const noexcept override
    {
        return -1;
    }
    bool IsShmInTypedMemory() const noexcept override
    {
        return false;
    }

  private:
    struct ControlBlock
    {
        explicit ControlBlock(const std::uint64_t id) noexcept : alreadyAllocatedBytes{}, memoryResourceProxy{id} {}

        std::atomic<std::size_t> alreadyAllocatedBytes;
        MemoryResourceProxy memoryResourceProxy;
    };

    VmoSharedMemoryResource(Identifier identifier, zx::vmo vmo, const bool writable);

    score::cpp::expected_blank<score::os::Error> MapAndRegister() noexcept;
    void InitializeControlBlock() noexcept;

    const void* getEndAddress() const noexcept override;
    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override;
    void do_deallocate(void*, std::size_t, std::size_t) override {}
    bool do_is_equal(const memory_resource& other) const noexcept override;

    zx::vmo vmo_;
    bool writable_;
    Identifier identifier_;
    std::string log_identification_;
    std::uint64_t memory_identifier_;
    std::size_t mapped_size_{0U};
    void* base_address_{nullptr};
    ControlBlock* control_block_{nullptr};
    void* start_{nullptr};
    bool registered_{false};
};

}  // namespace score::memory::shared::fuchsia

#endif  // SCORE_LIB_MEMORY_SHARED_FUCHSIA_VMO_SHARED_MEMORY_RESOURCE_H
