/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
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
#include "score/memory/shared/lock_file.h"

#include "score/memory/shared/lock_provider/lock_provider.h"
#include "score/mw/log/logging.h"

namespace score::memory::shared
{

std::optional<LockFile> LockFile::Create(std::string path) noexcept
{
    const auto create_result = LockProvider::Instance().Create(path);
    if (!create_result.has_value())
    {
        score::mw::log::LogError("shm") << "LockFile::Create failed to open file: " << path
                                        << " | Error: " << create_result.error().ToString();
        return {};
    }
    constexpr bool owns_file{true};
    return LockFile{std::move(path), create_result.value(), owns_file};
}

std::optional<LockFile> LockFile::CreateOrOpen(std::string path, bool take_ownership) noexcept
{
    const auto create_result = LockProvider::Instance().CreateOrOpen(path);
    if (!create_result.has_value())
    {
        score::mw::log::LogError("shm") << "LockFile::CreateOrOpen failed to open file: " << path
                                        << " | Error: " << create_result.error().ToString();
        return {};
    }
    return LockFile{std::move(path), create_result.value(), take_ownership};
}

std::optional<LockFile> LockFile::Open(std::string path) noexcept
{
    const auto create_result = LockProvider::Instance().Open(path);
    if (!create_result.has_value())
    {
        score::mw::log::LogError("shm") << "LockFile::Open failed to open file: " << path
                                        << " | Error: " << create_result.error().ToString();
        return {};
    }
    constexpr bool owns_file{false};
    return LockFile{std::move(path), create_result.value(), owns_file};
}

LockFile::LockFile(std::string path, const std::int32_t file_descriptor, bool owns_file) noexcept
    : lock_file_path_{std::move(path)}, file_descriptor_{file_descriptor}, lock_file_owns_file_{owns_file}
{
}

void LockFile::CleanupFile() noexcept
{
    if (file_descriptor_ != -1)
    {
        LockProvider::Instance().Close(file_descriptor_);
        if (lock_file_owns_file_)
        {
            LockProvider::Instance().Remove(lock_file_path_);
        }
    }
}

LockFile::~LockFile() noexcept
{
    CleanupFile();
}

LockFile::LockFile(LockFile&& other) noexcept
    : lock_file_path_{std::move(other.lock_file_path_)},
      file_descriptor_{other.file_descriptor_},
      lock_file_owns_file_{other.lock_file_owns_file_}
{
    // Prevent the moved-from LockFile from cleaning up the LockFile whose ownership was transferred
    other.file_descriptor_ = -1;
}

// Suppress AUTOSAR C++14 A12-6-1" rule finding. The rule states "Move and copy assignment operators shall either move
// or respectively copy base classes and data members of a class, without any side effects.".
// Rationale: A LockFile is an RAII wrapper around a file. Therefore, it's required that a clean up of the moved-to
// LockFile is done during move assignment.
// coverity[autosar_cpp14_a6_2_1_violation]
LockFile& LockFile::operator=(LockFile&& other) & noexcept
{
    if (this != &other)
    {
        // Cleanup the moved-to LockFile
        CleanupFile();
        lock_file_path_ = std::move(other.lock_file_path_);
        file_descriptor_ = other.file_descriptor_;
        lock_file_owns_file_ = other.lock_file_owns_file_;

        // Prevent the moved-from LockFile from cleaning up the LockFile whose ownership was transferred
        other.file_descriptor_ = -1;
    }
    return *this;
}

}  // namespace score::memory::shared
