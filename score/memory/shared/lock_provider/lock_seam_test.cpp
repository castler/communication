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
#include "score/memory/shared/flock/exclusive_flock_mutex.h"
#include "score/memory/shared/flock/shared_flock_mutex.h"
#include "score/memory/shared/lock_file.h"
#include "score/memory/shared/lock_provider/in_process_lock_registry.h"
#include "score/memory/shared/lock_provider/lock_provider.h"
#include "score/memory/shared/lock_provider/registry_lock_provider.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace score::memory::shared
{
namespace
{

/// Records what LockFile and FlockMutex ask of the seam; there is no file system and no flock behind it.
class RecordingLockProvider final : public ILockProvider
{
  public:
    score::cpp::expected<Descriptor, score::os::Error> Create(const std::string& path) noexcept override
    {
        calls.push_back("Create " + path);
        return 7;
    }
    score::cpp::expected<Descriptor, score::os::Error> CreateOrOpen(const std::string& path) noexcept override
    {
        calls.push_back("CreateOrOpen " + path);
        return 8;
    }
    score::cpp::expected<Descriptor, score::os::Error> Open(const std::string& path) noexcept override
    {
        calls.push_back("Open " + path);
        if (fail_open)
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
        }
        return 9;
    }
    void Close(const Descriptor descriptor) noexcept override
    {
        calls.push_back("Close " + std::to_string(descriptor));
    }
    void Remove(const std::string& path) noexcept override
    {
        calls.push_back("Remove " + path);
    }
    score::cpp::expected_blank<score::os::Error> Flock(const Descriptor descriptor,
                                                      const score::os::Fcntl::Operation operation) noexcept override
    {
        calls.push_back("Flock " + std::to_string(descriptor) + " " +
                        std::to_string(static_cast<std::uint32_t>(operation)));
        if (flock_would_block)
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrnoFlockSpecific(EWOULDBLOCK));
        }
        return {};
    }

    std::vector<std::string> calls;
    bool fail_open{false};
    bool flock_would_block{false};
};

class LockSeamTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        LockProvider::Inject(nullptr);
    }
};

TEST_F(LockSeamTest, LockFileRoutesCreateOpenCloseAndRemoveThroughProvider)
{
    RecordingLockProvider provider;
    LockProvider::Inject(&provider);
    {
        auto created = LockFile::Create("/created");
        ASSERT_TRUE(created.has_value());
        auto created_or_opened = LockFile::CreateOrOpen("/cooo", false);
        ASSERT_TRUE(created_or_opened.has_value());
        auto opened = LockFile::Open("/opened");
        ASSERT_TRUE(opened.has_value());
    }
    // Destruction order is reverse of construction; only the owning LockFile removes its name.
    const std::vector<std::string> expected{
        "Create /created", "CreateOrOpen /cooo", "Open /opened", "Close 9", "Close 8", "Close 7", "Remove /created"};
    EXPECT_EQ(provider.calls, expected);
}

TEST_F(LockSeamTest, LockFileReportsProviderError)
{
    RecordingLockProvider provider;
    provider.fail_open = true;
    LockProvider::Inject(&provider);
    EXPECT_FALSE(LockFile::Open("/missing").has_value());
}

TEST_F(LockSeamTest, MutexesIssueFlockOperationsOnTheLockFileDescriptor)
{
    RecordingLockProvider provider;
    LockProvider::Inject(&provider);
    auto file = LockFile::CreateOrOpen("/marker", false);
    ASSERT_TRUE(file.has_value());
    provider.calls.clear();

    ExclusiveFlockMutex exclusive{file.value()};
    EXPECT_TRUE(exclusive.try_lock());
    exclusive.unlock();
    const std::vector<std::string> expected{
        "Flock 8 " + std::to_string(static_cast<std::uint32_t>(score::os::Fcntl::Operation::kLockExclusive |
                                                               score::os::Fcntl::Operation::kLockNB)),
        "Flock 8 " + std::to_string(static_cast<std::uint32_t>(score::os::Fcntl::Operation::kUnLock))};
    EXPECT_EQ(provider.calls, expected);
}

TEST_F(LockSeamTest, TryLockReturnsFalseOnWouldBlock)
{
    RecordingLockProvider provider;
    provider.flock_would_block = true;
    LockProvider::Inject(&provider);
    auto file = LockFile::CreateOrOpen("/marker", false);
    ASSERT_TRUE(file.has_value());
    SharedFlockMutex shared{file.value()};
    EXPECT_FALSE(shared.try_lock());
}

TEST_F(LockSeamTest, InjectReturnsPreviousOverride)
{
    RecordingLockProvider first;
    RecordingLockProvider second;
    EXPECT_EQ(LockProvider::Inject(&first), nullptr);
    EXPECT_EQ(&LockProvider::Instance(), &first);
    EXPECT_EQ(LockProvider::Inject(&second), &first);
    EXPECT_EQ(LockProvider::Inject(nullptr), &second);
}

// LoLa scenario on the registry provider (no flock anywhere): one skeleton per instance via the existence marker, the
// usage marker is shared by proxies and exclusively taken by a restarting skeleton.
TEST_F(LockSeamTest, SkeletonProxyMarkerScenarioOnRegistryProvider)
{
    RegistryLockProvider provider{std::make_shared<InProcessLockRegistry>()};
    LockProvider::Inject(&provider);

    std::optional<LockFile> skeleton_existence = LockFile::CreateOrOpen("/existence", false);
    ASSERT_TRUE(skeleton_existence.has_value());
    std::optional<ExclusiveFlockMutex> skeleton_mutex{ExclusiveFlockMutex{skeleton_existence.value()}};
    ASSERT_TRUE(skeleton_mutex->try_lock());
    skeleton_existence.value().TakeOwnership();

    auto second_existence = LockFile::CreateOrOpen("/existence", false);
    ASSERT_TRUE(second_existence.has_value());
    ExclusiveFlockMutex second_mutex{second_existence.value()};
    EXPECT_FALSE(second_mutex.try_lock());

    auto usage_skeleton = LockFile::CreateOrOpen("/usage", false);
    ASSERT_TRUE(usage_skeleton.has_value());
    auto usage_proxy = LockFile::Open("/usage");
    ASSERT_TRUE(usage_proxy.has_value());
    SharedFlockMutex proxy_mutex{usage_proxy.value()};
    ASSERT_TRUE(proxy_mutex.try_lock());
    ExclusiveFlockMutex restarting_skeleton_mutex{usage_skeleton.value()};
    EXPECT_FALSE(restarting_skeleton_mutex.try_lock());
    proxy_mutex.unlock();
    EXPECT_TRUE(restarting_skeleton_mutex.try_lock());

    // The existence marker owner goes away: the name disappears and a new skeleton can start.
    skeleton_mutex.reset();
    skeleton_existence.reset();
    EXPECT_FALSE(LockFile::Open("/existence").has_value());
    auto next = LockFile::Create("/existence");
    EXPECT_TRUE(next.has_value());
}

TEST_F(LockSeamTest, LockFileCreateFailsIfMarkerExists)
{
    RegistryLockProvider provider{std::make_shared<InProcessLockRegistry>()};
    LockProvider::Inject(&provider);
    auto first = LockFile::Create("/marker");
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(LockFile::Create("/marker").has_value());
}

}  // namespace
}  // namespace score::memory::shared
