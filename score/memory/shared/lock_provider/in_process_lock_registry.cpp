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
#include "score/memory/shared/lock_provider/in_process_lock_registry.h"

#include <cerrno>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace score::memory::shared
{

struct InProcessLockRegistry::State
{
    struct LockState
    {
        bool exclusive{false};
        std::uint32_t shared_holders{0U};
    };

    std::mutex mutex;
    std::set<std::string> names;
    std::map<std::string, LockState> locks;
};

namespace
{

class Token final : public ILockToken
{
  public:
    Token(std::shared_ptr<InProcessLockRegistry::State> state, std::string name, const LockKind kind)
        : state_{std::move(state)}, name_{std::move(name)}, kind_{kind}
    {
    }

    ~Token() override
    {
        const std::lock_guard<std::mutex> guard{state_->mutex};
        const auto it = state_->locks.find(name_);
        if (it == state_->locks.end())
        {
            return;
        }
        if (kind_ == LockKind::kExclusive)
        {
            it->second.exclusive = false;
        }
        else if (it->second.shared_holders > 0U)
        {
            --it->second.shared_holders;
        }
        if (!it->second.exclusive && it->second.shared_holders == 0U)
        {
            state_->locks.erase(it);
        }
    }

  private:
    std::shared_ptr<InProcessLockRegistry::State> state_;
    std::string name_;
    LockKind kind_;
};

}  // namespace

InProcessLockRegistry::InProcessLockRegistry() : state_{std::make_shared<State>()} {}

auto InProcessLockRegistry::CreateName(const std::string& name, const bool fail_if_exists) noexcept
    -> score::cpp::expected_blank<score::os::Error>
{
    const std::lock_guard<std::mutex> guard{state_->mutex};
    const bool inserted = state_->names.insert(name).second;
    if (!inserted && fail_if_exists)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EEXIST));
    }
    return {};
}

bool InProcessLockRegistry::NameExists(const std::string& name) noexcept
{
    const std::lock_guard<std::mutex> guard{state_->mutex};
    return state_->names.count(name) != 0U;
}

void InProcessLockRegistry::RemoveName(const std::string& name) noexcept
{
    const std::lock_guard<std::mutex> guard{state_->mutex};
    state_->names.erase(name);
}

auto InProcessLockRegistry::TryAcquire(const std::string& name, const LockKind kind) noexcept
    -> score::cpp::expected<std::unique_ptr<ILockToken>, score::os::Error>
{
    {
        const std::lock_guard<std::mutex> guard{state_->mutex};
        auto& lock = state_->locks[name];
        const bool conflict = lock.exclusive || (kind == LockKind::kExclusive && lock.shared_holders != 0U);
        if (conflict)
        {
            return std::unique_ptr<ILockToken>{};
        }
        if (kind == LockKind::kExclusive)
        {
            lock.exclusive = true;
        }
        else
        {
            ++lock.shared_holders;
        }
    }
    return std::unique_ptr<ILockToken>{std::make_unique<Token>(state_, name, kind)};
}

}  // namespace score::memory::shared
