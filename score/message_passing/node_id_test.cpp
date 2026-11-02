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
#include "score/message_passing/node_id.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace score::message_passing
{
namespace
{

TEST(NodeIdTest, DefaultConstructedIsInvalid)
{
    EXPECT_FALSE(NodeId{}.IsValid());
    EXPECT_EQ(NodeId{}.ToWire(), 0U);
}

TEST(NodeIdTest, FromPidRoundTrips)
{
    const auto node = NodeId::FromPid(4711);
    EXPECT_TRUE(node.IsValid());
    EXPECT_EQ(node.ToPid(), 4711);
    EXPECT_EQ(node.ToWire(), 4711U);
}

TEST(NodeIdTest, WireRoundTrips)
{
    constexpr std::uint64_t kWire{0x1122334455667788U};
    EXPECT_EQ(NodeId::FromWire(kWire).ToWire(), kWire);
    EXPECT_EQ(NodeId::FromWire(kWire), NodeId::FromWire(kWire));
}

TEST(NodeIdTest, EqualityDistinguishesValues)
{
    EXPECT_EQ(NodeId::FromPid(10), NodeId::FromPid(10));
    EXPECT_NE(NodeId::FromPid(10), NodeId::FromPid(11));
}

TEST(NodeIdTest, OrderingIsStrictWeak)
{
    EXPECT_LT(NodeId::FromPid(1), NodeId::FromPid(2));
    EXPECT_FALSE(NodeId::FromPid(2) < NodeId::FromPid(2));
}

TEST(NodeIdTest, HashIsConsistentWithEquality)
{
    EXPECT_EQ(std::hash<NodeId>{}(NodeId::FromPid(10)), std::hash<NodeId>{}(NodeId::FromPid(10)));

    std::unordered_set<NodeId> nodes{NodeId::FromPid(10), NodeId::FromPid(10), NodeId::FromPid(11)};
    EXPECT_EQ(nodes.size(), 2U);
}

TEST(NodeIdTest, ToStringContainsValue)
{
    EXPECT_EQ(NodeId::FromPid(10).ToString(), "node=10");
}

TEST(NodeIdTest, HasSharedMemorySafeLayout)
{
    static_assert(sizeof(NodeId) == 8U);
    static_assert(std::is_trivially_copyable_v<NodeId>);
    SUCCEED();
}

}  // namespace
}  // namespace score::message_passing
