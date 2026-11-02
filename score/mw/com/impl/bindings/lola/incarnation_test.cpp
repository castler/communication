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
#include "score/mw/com/impl/bindings/lola/incarnation.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace score::mw::com::impl::lola
{
namespace
{

TEST(IncarnationIdTest, DefaultAndNoneAreInvalid)
{
    EXPECT_FALSE(IncarnationId{}.IsValid());
    EXPECT_EQ(IncarnationId::None(), IncarnationId{});
    EXPECT_EQ(IncarnationId::None().Raw(), 0U);
}

TEST(IncarnationIdTest, FromPidRoundTrips)
{
    const auto incarnation = IncarnationId::FromPid(1234);
    EXPECT_TRUE(incarnation.IsValid());
    EXPECT_EQ(incarnation.ToPid(), 1234);
    EXPECT_EQ(incarnation.Raw(), 1234U);
}

TEST(IncarnationIdTest, FromLeaseKeepsRawValue)
{
    constexpr std::uint64_t kLease{0x0123456789abcdefU};
    EXPECT_EQ(IncarnationId::FromLease(kLease).Raw(), kLease);
}

TEST(IncarnationIdTest, EqualityDistinguishesValues)
{
    EXPECT_EQ(IncarnationId::FromPid(5), IncarnationId::FromPid(5));
    EXPECT_NE(IncarnationId::FromPid(5), IncarnationId::FromPid(6));
}

TEST(IncarnationIdTest, HashIsConsistentWithEquality)
{
    EXPECT_EQ(std::hash<IncarnationId>{}(IncarnationId::FromPid(5)), std::hash<IncarnationId>{}(IncarnationId::FromPid(5)));
    std::unordered_set<IncarnationId> set{IncarnationId::FromPid(5), IncarnationId::FromPid(5), IncarnationId::FromPid(6)};
    EXPECT_EQ(set.size(), 2U);
}

TEST(IncarnationIdTest, ToStringContainsValue)
{
    EXPECT_EQ(IncarnationId::FromPid(42).ToString(), "incarnation=42");
}

TEST(IncarnationIdTest, ConvertsToAndFromNodeIdWithoutChangingTheWireValue)
{
    const auto incarnation = IncarnationId::FromPid(77);
    EXPECT_EQ(incarnation.ToNodeId(), message_passing::NodeId::FromPid(77));
    EXPECT_EQ(IncarnationId::FromNodeId(message_passing::NodeId::FromPid(77)), incarnation);
}

TEST(IncarnationIdTest, HasSharedMemorySafeLayout)
{
    EXPECT_EQ(sizeof(IncarnationId), 8U);
    EXPECT_TRUE(std::is_trivially_copyable_v<IncarnationId>);
}

TEST(SkeletonEpochTest, DefaultIsUnpublished)
{
    EXPECT_FALSE(SkeletonEpoch{}.IsPublished());
    EXPECT_EQ(SkeletonEpoch::Unpublished(), SkeletonEpoch{});
    EXPECT_FALSE(SkeletonEpoch{}.IsFenced());
}

TEST(SkeletonEpochTest, FencedIsPublishedAndMaximal)
{
    EXPECT_TRUE(SkeletonEpoch::Fenced().IsFenced());
    EXPECT_TRUE(SkeletonEpoch::Fenced().IsPublished());
    EXPECT_EQ(SkeletonEpoch::Fenced().Raw(), std::numeric_limits<std::uint64_t>::max());
}

TEST(SkeletonEpochTest, FromIncarnationEqualsIncarnationRaw)
{
    EXPECT_EQ(SkeletonEpoch::FromIncarnation(IncarnationId::FromPid(9)), SkeletonEpoch::FromRaw(9U));
    EXPECT_NE(SkeletonEpoch::FromRaw(9U), SkeletonEpoch::FromRaw(10U));
}

TEST(SkeletonEpochTest, HashAndToString)
{
    EXPECT_EQ(std::hash<SkeletonEpoch>{}(SkeletonEpoch::FromRaw(3U)), std::hash<SkeletonEpoch>{}(SkeletonEpoch::FromRaw(3U)));
    EXPECT_EQ(SkeletonEpoch::FromRaw(3U).ToString(), "epoch=3");
}

TEST(SkeletonEpochTest, HasSharedMemorySafeLayout)
{
    EXPECT_EQ(sizeof(SkeletonEpoch), 8U);
    EXPECT_TRUE(std::is_trivially_copyable_v<SkeletonEpoch>);
}

}  // namespace
}  // namespace score::mw::com::impl::lola
