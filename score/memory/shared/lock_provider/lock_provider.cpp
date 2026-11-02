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
#include "score/memory/shared/lock_provider/lock_provider.h"

#include "score/memory/shared/lock_provider/in_process_lock_registry.h"
#include "score/memory/shared/lock_provider/posix_lock_provider.h"
#include "score/memory/shared/lock_provider/registry_lock_provider.h"

#include <atomic>
#include <memory>

namespace score::memory::shared
{

namespace
{

std::atomic<ILockProvider*> injected_provider{nullptr};

ILockProvider& DefaultProvider() noexcept
{
#if defined(__Fuchsia__)
    static RegistryLockProvider provider{std::make_shared<InProcessLockRegistry>()};
#else
    static PosixLockProvider provider{};
#endif
    return provider;
}

}  // namespace

ILockProvider& LockProvider::Instance() noexcept
{
    ILockProvider* const injected = injected_provider.load();
    return injected != nullptr ? *injected : DefaultProvider();
}

ILockProvider* LockProvider::Inject(ILockProvider* const provider) noexcept
{
    return injected_provider.exchange(provider);
}

}  // namespace score::memory::shared
