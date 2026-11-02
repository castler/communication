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
#ifndef SCORE_LIB_MEMORY_SHARED_FUCHSIA_VMO_SHARED_MEMORY_FACTORY_H
#define SCORE_LIB_MEMORY_SHARED_FUCHSIA_VMO_SHARED_MEMORY_FACTORY_H

#include "score/memory/shared/fuchsia/i_vmo_handle_provider.h"
#include "score/memory/shared/i_shared_memory_factory.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace score::memory::shared::fuchsia
{

/// \brief ISharedMemoryFactory creating/opening VmoSharedMemoryResource. Install it with
///        SharedMemoryFactory::InjectMock(&factory) to make all SharedMemoryFactory::* callers use VMOs.
/// \details Deviations from the POSIX factory, all because Fuchsia has neither a shm namespace nor uids/ACLs:
///          - Create/CreateOrOpen publish a reduced-rights handle to the IVmoHandleProvider, Open/CreateOrOpen fetch it
///            from there. Opening a path nobody published returns nullptr.
///          - UserPermissions, AccessControl::allowedProviders, prefer_typed_memory are ignored; access is governed by
///            who is given a handle (capability routing).
///          - There is no lock file; mutual exclusion of creators is limited to this process (own resource map).
class VmoSharedMemoryFactory : public ISharedMemoryFactory
{
  public:
    /// \brief Decides per key whether the published consumer handle carries WRITE (e.g. the control region of a LoLa
    ///        service instance, which proxies must write). Default: consumers are read-only.
    using ConsumerWritablePredicate = std::function<bool(const std::string& key)>;

    explicit VmoSharedMemoryFactory(std::shared_ptr<IVmoHandleProvider> handle_provider,
                                    ConsumerWritablePredicate consumer_writable = nullptr) noexcept;
    ~VmoSharedMemoryFactory() override = default;

    std::shared_ptr<ISharedMemoryResource> Open(const std::string& path,
                                                const bool is_read_write,
                                                const std::optional<score::cpp::span<const uid_t>>&) noexcept override;

    std::shared_ptr<ISharedMemoryResource> Create(std::string path,
                                                  InitializeCallback cb,
                                                  const std::size_t user_space_to_reserve,
                                                  const UserPermissions&,
                                                  const bool) noexcept override;

    std::shared_ptr<ISharedMemoryResource> CreateAnonymous(std::uint64_t shared_memory_resource_id,
                                                           InitializeCallback cb,
                                                           const std::size_t user_space_to_reserve,
                                                           const UserPermissions&,
                                                           const bool) noexcept override;

    std::shared_ptr<ISharedMemoryResource> CreateOrOpen(std::string path,
                                                        InitializeCallback cb,
                                                        const std::size_t user_space_to_reserve,
                                                        const ISharedMemoryFactory::AccessControl,
                                                        const bool) noexcept override;

    /// \brief Withdraws the path from the handle provider; existing mappings stay valid.
    void Remove(const std::string& path) noexcept override;
    void RemoveStaleArtefacts(const std::string&) noexcept override {}
    void SetTypedMemoryProvider(std::shared_ptr<score::memory::shared::TypedMemory>) noexcept override {}
    void SetInterVMMemoryProvider(std::shared_ptr<score::memory::shared::TypedMemory>) noexcept override {}
    std::size_t GetControlBlockSize() noexcept override;
    void Clear() noexcept override;

    static std::string KeyForAnonymous(const std::uint64_t id);

  private:
    std::shared_ptr<ISharedMemoryResource> CreateImpl(const std::string& key,
                                                      std::variant<std::string, std::uint64_t> identifier,
                                                      InitializeCallback cb,
                                                      const std::size_t user_space_to_reserve) noexcept;
    std::shared_ptr<ISharedMemoryResource> OpenImpl(const std::string& key,
                                                    std::variant<std::string, std::uint64_t> identifier,
                                                    const std::optional<bool>& is_read_write) noexcept;
    std::shared_ptr<ISharedMemoryResource> Lookup(const std::string& key) noexcept;

    std::shared_ptr<IVmoHandleProvider> handle_provider_;
    ConsumerWritablePredicate consumer_writable_;
    std::recursive_mutex mutex_;
    std::unordered_map<std::string, std::weak_ptr<ISharedMemoryResource>> resources_;
};

}  // namespace score::memory::shared::fuchsia

namespace score::memory::shared::fuchsia
{

/// \brief Factory the SharedMemoryFactory facade uses on Fuchsia when nothing was injected: regions are VMOs, handles
///        are published to a provider that keeps them in this process only and cannot acquire foreign ones. Cross
///        process sharing needs an injected factory whose provider transfers the handles (e.g. FidlVmoHandleServer).
class DefaultVmoSharedMemoryFactory final : public VmoSharedMemoryFactory
{
  public:
    DefaultVmoSharedMemoryFactory() noexcept;
};

}  // namespace score::memory::shared::fuchsia

#endif  // SCORE_LIB_MEMORY_SHARED_FUCHSIA_VMO_SHARED_MEMORY_FACTORY_H
