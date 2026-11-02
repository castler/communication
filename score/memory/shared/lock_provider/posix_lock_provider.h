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
#ifndef SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_POSIX_LOCK_PROVIDER_H
#define SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_POSIX_LOCK_PROVIDER_H

#include "score/memory/shared/lock_provider/i_lock_provider.h"

namespace score::memory::shared
{

/// \brief Files plus flock(2) through the score::os::{Fcntl,Stat,Unistd} seams. This is the behaviour LockFile and
///        FlockMutex always had on Linux and QNX.
class PosixLockProvider final : public ILockProvider
{
  public:
    score::cpp::expected<Descriptor, score::os::Error> Create(const std::string& path) noexcept override;
    score::cpp::expected<Descriptor, score::os::Error> CreateOrOpen(const std::string& path) noexcept override;
    score::cpp::expected<Descriptor, score::os::Error> Open(const std::string& path) noexcept override;
    void Close(Descriptor descriptor) noexcept override;
    void Remove(const std::string& path) noexcept override;
    score::cpp::expected_blank<score::os::Error> Flock(Descriptor descriptor,
                                                      score::os::Fcntl::Operation operation) noexcept override;
};

}  // namespace score::memory::shared

#endif  // SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_POSIX_LOCK_PROVIDER_H
