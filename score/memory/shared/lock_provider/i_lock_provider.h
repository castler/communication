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
#ifndef SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_I_LOCK_PROVIDER_H
#define SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_I_LOCK_PROVIDER_H

#include "score/os/errno.h"
#include "score/os/fcntl.h"

#include <score/expected.hpp>

#include <cstdint>
#include <string>

namespace score::memory::shared
{

/// \brief OS seam below LockFile and FlockMutex: named marker "files" plus flock(2) style locks on them.
/// \details The semantics mirror open(2)/close(2)/unlink(2)/flock(2) because LockFile and FlockMutex (and with them the
///          LoLa existence/usage markers) are written against exactly that model:
///          - A name is created/opened and yields a descriptor, which is only meaningful for this provider.
///          - Locks belong to a descriptor, are shared or exclusive and are dropped when the descriptor is closed.
///          - A lock that cannot be taken without blocking is reported with
///            Error::createFromErrnoFlockSpecific(EWOULDBLOCK), exactly like Fcntl::flock.
class ILockProvider
{
  public:
    using Descriptor = std::int32_t;

    ILockProvider() noexcept = default;
    virtual ~ILockProvider() = default;

    /// \brief Creates the name; fails if it exists already (O_CREAT | O_EXCL).
    virtual score::cpp::expected<Descriptor, score::os::Error> Create(const std::string& path) noexcept = 0;

    /// \brief Creates the name if it does not exist yet and opens it (O_CREAT).
    virtual score::cpp::expected<Descriptor, score::os::Error> CreateOrOpen(const std::string& path) noexcept = 0;

    /// \brief Opens an existing name; fails if it does not exist.
    virtual score::cpp::expected<Descriptor, score::os::Error> Open(const std::string& path) noexcept = 0;

    /// \brief Releases the descriptor and every lock it holds. The name stays.
    virtual void Close(Descriptor descriptor) noexcept = 0;

    /// \brief Removes the name (unlink). Open descriptors and their locks stay valid.
    virtual void Remove(const std::string& path) noexcept = 0;

    /// \brief Same contract as Fcntl::flock (kLockShared / kLockExclusive, optionally | kLockNB, or kUnLock).
    virtual score::cpp::expected_blank<score::os::Error> Flock(Descriptor descriptor,
                                                              score::os::Fcntl::Operation operation) noexcept = 0;

  protected:
    ILockProvider(const ILockProvider&) noexcept = default;
    ILockProvider(ILockProvider&&) noexcept = default;
    ILockProvider& operator=(const ILockProvider&) noexcept = default;
    ILockProvider& operator=(ILockProvider&&) noexcept = default;
};

}  // namespace score::memory::shared

#endif  // SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_I_LOCK_PROVIDER_H
