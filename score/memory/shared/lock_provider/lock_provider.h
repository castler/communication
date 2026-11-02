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
#ifndef SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_LOCK_PROVIDER_H
#define SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_LOCK_PROVIDER_H

#include "score/memory/shared/lock_provider/i_lock_provider.h"

namespace score::memory::shared
{

/// \brief Process wide access to the ILockProvider used by LockFile and FlockMutex.
/// \details Default: PosixLockProvider (files + flock) on Linux and QNX, a RegistryLockProvider backed by an
///          InProcessLockRegistry on Fuchsia. Inject another provider (e.g. one whose registry is served by another
///          process) before the first LockFile is created and keep it alive until the last LockFile is gone.
class LockProvider
{
  public:
    static ILockProvider& Instance() noexcept;

    /// \brief Replaces the provider; nullptr restores the default. Returns the previous override.
    static ILockProvider* Inject(ILockProvider* provider) noexcept;
};

}  // namespace score::memory::shared

#endif  // SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_LOCK_PROVIDER_H
