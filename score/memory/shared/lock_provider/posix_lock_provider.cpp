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
#include "score/memory/shared/lock_provider/posix_lock_provider.h"

#include "score/os/stat.h"
#include "score/os/unistd.h"

#include <score/utility.hpp>

namespace score::memory::shared
{

namespace
{

using Mode = ::score::os::Stat::Mode;
constexpr auto kReadAccessForAll = Mode::kReadUser | Mode::kReadGroup | Mode::kReadOthers;

score::cpp::expected<ILockProvider::Descriptor, score::os::Error> OpenAndChmod(const std::string& path,
                                                                                const score::os::Fcntl::Open flags)
{
    // The reason for banning is, because it's error-prone to use. One should use abstractions e.g. provided by
    // the C++ standard library. But these abstraction do not support exclusive access, which is why we created
    // this abstraction library.
    // NOLINTNEXTLINE(score-banned-function): See above.
    const auto open_result = ::score::os::Fcntl::instance().open(path.data(), flags, kReadAccessForAll);
    if (!open_result.has_value())
    {
        return score::cpp::make_unexpected(open_result.error());
    }
    const auto chmod_result = score::os::Stat::instance().chmod(path.data(), kReadAccessForAll);
    if (!chmod_result.has_value())
    {
        return score::cpp::make_unexpected(chmod_result.error());
    }
    return open_result.value();
}

}  // namespace

auto PosixLockProvider::Create(const std::string& path) noexcept -> score::cpp::expected<Descriptor, score::os::Error>
{
    using Open = ::score::os::Fcntl::Open;
    return OpenAndChmod(path, Open::kCreate | Open::kExclusive | Open::kReadOnly);
}

auto PosixLockProvider::CreateOrOpen(const std::string& path) noexcept
    -> score::cpp::expected<Descriptor, score::os::Error>
{
    using Open = ::score::os::Fcntl::Open;
    return OpenAndChmod(path, Open::kCreate | Open::kReadOnly);
}

auto PosixLockProvider::Open(const std::string& path) noexcept -> score::cpp::expected<Descriptor, score::os::Error>
{
    // NOLINTNEXTLINE(score-banned-function): See OpenAndChmod.
    return ::score::os::Fcntl::instance().open(path.data(), ::score::os::Fcntl::Open::kReadOnly, kReadAccessForAll);
}

void PosixLockProvider::Close(const Descriptor descriptor) noexcept
{
    // NOLINTNEXTLINE(score-banned-function): See OpenAndChmod.
    score::cpp::ignore = ::score::os::Unistd::instance().close(descriptor);
}

void PosixLockProvider::Remove(const std::string& path) noexcept
{
    // NOLINTNEXTLINE(score-banned-function): See OpenAndChmod.
    score::cpp::ignore = ::score::os::Unistd::instance().unlink(path.data());
}

auto PosixLockProvider::Flock(const Descriptor descriptor, const score::os::Fcntl::Operation operation) noexcept
    -> score::cpp::expected_blank<score::os::Error>
{
    return ::score::os::Fcntl::instance().flock(descriptor, operation);
}

}  // namespace score::memory::shared
