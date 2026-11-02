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
#include "score/memory/shared/lock_provider/registry_lock_provider.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>

namespace score::memory::shared
{
namespace
{

using Operation = score::os::Fcntl::Operation;

constexpr auto kShared = Operation::kLockShared | Operation::kLockNB;
constexpr auto kExclusive = Operation::kLockExclusive | Operation::kLockNB;
const auto kWouldBlock = score::os::Error::createFromErrnoFlockSpecific(EWOULDBLOCK);

class RegistryLockProviderTest : public ::testing::Test
{
  protected:
    RegistryLockProvider provider_{std::make_shared<InProcessLockRegistry>()};
};

TEST_F(RegistryLockProviderTest, CreateFailsIfNameExists)
{
    const auto first = provider_.Create("/marker");
    ASSERT_TRUE(first.has_value());
    const auto second = provider_.Create("/marker");
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), score::os::Error::createFromErrno(EEXIST));
}

TEST_F(RegistryLockProviderTest, OpenFailsIfNameDoesNotExist)
{
    const auto result = provider_.Open("/missing");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), score::os::Error::createFromErrno(ENOENT));
}

TEST_F(RegistryLockProviderTest, CreateOrOpenAndOpenShareTheName)
{
    ASSERT_TRUE(provider_.CreateOrOpen("/marker").has_value());
    EXPECT_TRUE(provider_.CreateOrOpen("/marker").has_value());
    EXPECT_TRUE(provider_.Open("/marker").has_value());
    provider_.Remove("/marker");
    EXPECT_FALSE(provider_.Open("/marker").has_value());
}

TEST_F(RegistryLockProviderTest, DescriptorsAreDistinct)
{
    const auto a = provider_.CreateOrOpen("/marker");
    const auto b = provider_.CreateOrOpen("/marker");
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_NE(a.value(), b.value());
}

TEST_F(RegistryLockProviderTest, ExclusiveLockExcludesOtherDescriptors)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    const auto b = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kExclusive).has_value());

    const auto exclusive = provider_.Flock(b, kExclusive);
    ASSERT_FALSE(exclusive.has_value());
    EXPECT_EQ(exclusive.error(), kWouldBlock);
    const auto shared = provider_.Flock(b, kShared);
    ASSERT_FALSE(shared.has_value());
    EXPECT_EQ(shared.error(), kWouldBlock);
}

TEST_F(RegistryLockProviderTest, SharedLocksCoexistButExcludeExclusive)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    const auto b = provider_.CreateOrOpen("/marker").value();
    const auto c = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kShared).has_value());
    ASSERT_TRUE(provider_.Flock(b, kShared).has_value());
    EXPECT_EQ(provider_.Flock(c, kExclusive).error(), kWouldBlock);
}

TEST_F(RegistryLockProviderTest, LocksOnDifferentNamesAreIndependent)
{
    const auto a = provider_.CreateOrOpen("/a").value();
    const auto b = provider_.CreateOrOpen("/b").value();
    EXPECT_TRUE(provider_.Flock(a, kExclusive).has_value());
    EXPECT_TRUE(provider_.Flock(b, kExclusive).has_value());
}

TEST_F(RegistryLockProviderTest, UnlockReleases)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    const auto b = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kExclusive).has_value());
    ASSERT_TRUE(provider_.Flock(a, Operation::kUnLock).has_value());
    EXPECT_TRUE(provider_.Flock(b, kExclusive).has_value());
}

TEST_F(RegistryLockProviderTest, CloseReleases)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    const auto b = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kExclusive).has_value());
    provider_.Close(a);
    EXPECT_TRUE(provider_.Flock(b, kExclusive).has_value());
}

TEST_F(RegistryLockProviderTest, RelockOnSameDescriptorConverts)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    const auto b = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kExclusive).has_value());
    ASSERT_TRUE(provider_.Flock(a, kShared).has_value());
    EXPECT_TRUE(provider_.Flock(b, kShared).has_value());
    EXPECT_EQ(provider_.Flock(a, kExclusive).error(), kWouldBlock);
}

TEST_F(RegistryLockProviderTest, RemovingTheNameKeepsHeldLocks)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kExclusive).has_value());
    provider_.Remove("/marker");
    const auto b = provider_.CreateOrOpen("/marker").value();
    EXPECT_EQ(provider_.Flock(b, kExclusive).error(), kWouldBlock);
}

TEST_F(RegistryLockProviderTest, UnknownDescriptorIsRejected)
{
    const auto result = provider_.Flock(4711, kExclusive);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), score::os::Error::createFromErrnoFlockSpecific(EBADF));
}

TEST_F(RegistryLockProviderTest, BlockingLockWaitsForRelease)
{
    const auto a = provider_.CreateOrOpen("/marker").value();
    const auto b = provider_.CreateOrOpen("/marker").value();
    ASSERT_TRUE(provider_.Flock(a, kExclusive).has_value());

    std::atomic<bool> acquired{false};
    std::thread waiter{[&] {
        acquired = provider_.Flock(b, Operation::kLockExclusive).has_value();
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(acquired.load());
    provider_.Close(a);
    waiter.join();
    EXPECT_TRUE(acquired.load());
}

TEST(InProcessLockRegistryTest, TokenReleasesOnDestruction)
{
    InProcessLockRegistry registry;
    auto token = registry.TryAcquire("/x", LockKind::kExclusive);
    ASSERT_TRUE(token.has_value());
    ASSERT_NE(token.value(), nullptr);
    EXPECT_EQ(registry.TryAcquire("/x", LockKind::kShared).value(), nullptr);
    token.value().reset();
    EXPECT_NE(registry.TryAcquire("/x", LockKind::kShared).value(), nullptr);
}

TEST(InProcessLockRegistryTest, TokenMayOutliveRegistry)
{
    std::unique_ptr<ILockToken> token;
    {
        InProcessLockRegistry registry;
        token = std::move(registry.TryAcquire("/x", LockKind::kShared).value());
    }
    token.reset();
}

}  // namespace
}  // namespace score::memory::shared
