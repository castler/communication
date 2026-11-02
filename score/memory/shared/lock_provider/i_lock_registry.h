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
#ifndef SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_I_LOCK_REGISTRY_H
#define SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_I_LOCK_REGISTRY_H

#include "score/os/errno.h"

#include <score/expected.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace score::memory::shared
{

enum class LockKind : std::uint8_t
{
    kShared,
    kExclusive
};

/// \brief Proof of a held lock. Destroying it releases the lock.
class ILockToken
{
  public:
    ILockToken() noexcept = default;
    virtual ~ILockToken() = default;
    ILockToken(const ILockToken&) = delete;
    ILockToken& operator=(const ILockToken&) = delete;
    ILockToken(ILockToken&&) = delete;
    ILockToken& operator=(ILockToken&&) = delete;
};

/// \brief Name table and lock arbiter, the platform-neutral core of the non-flock lock provider.
/// \details This is the stand-in for the future broker: all state lives behind this interface, so it can be served
///          in-process (InProcessLockRegistry) or by another process (fuchsia::ChannelLockRegistryClient). Locks are
///          bound to the lifetime of a token, never to a file or a pid.
class ILockRegistry
{
  public:
    ILockRegistry() noexcept = default;
    virtual ~ILockRegistry() = default;

    /// \brief Registers `name`. With `fail_if_exists` an existing name is reported as EEXIST.
    virtual score::cpp::expected_blank<score::os::Error> CreateName(const std::string& name,
                                                                    bool fail_if_exists) noexcept = 0;
    virtual bool NameExists(const std::string& name) noexcept = 0;
    /// \brief Forgets `name`. Locks that are currently held on it stay valid until their tokens are destroyed.
    virtual void RemoveName(const std::string& name) noexcept = 0;

    /// \brief Tries to take a lock on `name` without blocking.
    /// \return A token on success, nullptr if the lock is held incompatibly by somebody else (the flock EWOULDBLOCK
    ///         case), an error if the registry itself failed.
    virtual score::cpp::expected<std::unique_ptr<ILockToken>, score::os::Error> TryAcquire(const std::string& name,
                                                                                           LockKind kind) noexcept = 0;

  protected:
    ILockRegistry(const ILockRegistry&) noexcept = default;
    ILockRegistry(ILockRegistry&&) noexcept = default;
    ILockRegistry& operator=(const ILockRegistry&) noexcept = default;
    ILockRegistry& operator=(ILockRegistry&&) noexcept = default;
};

}  // namespace score::memory::shared

#endif  // SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_I_LOCK_REGISTRY_H
