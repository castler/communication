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
#ifndef SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_REGISTRY_LOCK_PROVIDER_H
#define SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_REGISTRY_LOCK_PROVIDER_H

#include "score/memory/shared/lock_provider/i_lock_provider.h"
#include "score/memory/shared/lock_provider/i_lock_registry.h"

#include <map>
#include <memory>
#include <mutex>

namespace score::memory::shared
{

/// \brief ILockProvider without files and without flock(2): names and locks live in an ILockRegistry.
/// \details Descriptors are small integers handed out by this object. Each descriptor holds at most one lock token.
///          Closing a descriptor, or losing the token (e.g. because the process holding it died and the registry
///          transport notices), releases the lock. A blocking lock is a retry loop on the non-blocking acquisition.
class RegistryLockProvider final : public ILockProvider
{
  public:
    explicit RegistryLockProvider(std::shared_ptr<ILockRegistry> registry) noexcept;

    score::cpp::expected<Descriptor, score::os::Error> Create(const std::string& path) noexcept override;
    score::cpp::expected<Descriptor, score::os::Error> CreateOrOpen(const std::string& path) noexcept override;
    score::cpp::expected<Descriptor, score::os::Error> Open(const std::string& path) noexcept override;
    void Close(Descriptor descriptor) noexcept override;
    void Remove(const std::string& path) noexcept override;
    score::cpp::expected_blank<score::os::Error> Flock(Descriptor descriptor,
                                                      score::os::Fcntl::Operation operation) noexcept override;

  private:
    struct Slot
    {
        std::string name;
        std::unique_ptr<ILockToken> token;
    };

    Descriptor AddSlot(const std::string& name);

    std::shared_ptr<ILockRegistry> registry_;
    std::mutex mutex_;
    std::map<Descriptor, Slot> slots_;
    Descriptor next_descriptor_;
};

}  // namespace score::memory::shared

#endif  // SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_REGISTRY_LOCK_PROVIDER_H
