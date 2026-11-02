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
#ifndef SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_IN_PROCESS_LOCK_REGISTRY_H
#define SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_IN_PROCESS_LOCK_REGISTRY_H

#include "score/memory/shared/lock_provider/i_lock_registry.h"

#include <memory>

namespace score::memory::shared
{

/// \brief Mutex protected name table and shared/exclusive lock arbiter, valid within one process.
/// \details Tokens keep the state alive, so they may outlive the registry object.
class InProcessLockRegistry final : public ILockRegistry
{
  public:
    InProcessLockRegistry();

    score::cpp::expected_blank<score::os::Error> CreateName(const std::string& name,
                                                            bool fail_if_exists) noexcept override;
    bool NameExists(const std::string& name) noexcept override;
    void RemoveName(const std::string& name) noexcept override;
    score::cpp::expected<std::unique_ptr<ILockToken>, score::os::Error> TryAcquire(const std::string& name,
                                                                                   LockKind kind) noexcept override;

    struct State;

  private:
    std::shared_ptr<State> state_;
};

}  // namespace score::memory::shared

#endif  // SCORE_LIB_MEMORY_SHARED_LOCK_PROVIDER_IN_PROCESS_LOCK_REGISTRY_H
