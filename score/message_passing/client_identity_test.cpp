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
#include "score/message_passing/server_types.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <unordered_set>

namespace score::message_passing
{
namespace
{

TEST(OpaqueIdTest, NodeAndPrincipalAreDistinctNonConvertibleTypes)
{
    static_assert(!std::is_convertible_v<NodeId, PrincipalId>);
    static_assert(!std::is_convertible_v<PrincipalId, NodeId>);
    static_assert(!std::is_convertible_v<pid_t, NodeId>);
    static_assert(!std::is_convertible_v<NodeId, pid_t>);
    static_assert(!std::is_convertible_v<uid_t, PrincipalId>);
    static_assert(!std::is_convertible_v<PrincipalId, uid_t>);
}

TEST(OpaqueIdTest, PidAndUidRoundTrip)
{
    EXPECT_EQ(NodeIdToPid(NodeIdFromPid(1234)), 1234);
    EXPECT_EQ(NodeIdToPid(NodeIdFromPid(-1)), -1);
    EXPECT_EQ(PrincipalIdToUid(PrincipalIdFromUid(4321U)), 4321U);
    EXPECT_EQ(NodeIdFromPid(1234).Raw(), 1234U);
}

TEST(OpaqueIdTest, HashAndOrderingFollowRawValue)
{
    EXPECT_EQ(std::hash<NodeId>{}(NodeId{7U}), std::hash<NodeId>{}(NodeId{7U}));
    EXPECT_LT(NodeId{1U}, NodeId{2U});
    EXPECT_NE(NodeId{1U}, NodeId{2U});
}

TEST(ClientIdentityTest, CredentialsExposeNodeAndPrincipal)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_EQ(identity.GetNodeId(), NodeIdFromPid(10));
    EXPECT_EQ(identity.GetPrincipalId(), PrincipalIdFromUid(20U));
}

TEST(ClientIdentityTest, EqualityComparesAllCredentials)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_EQ(identity, ClientIdentity::FromCredentials(10, 20, 30));
    EXPECT_NE(identity, ClientIdentity::FromCredentials(11, 20, 30));
    EXPECT_NE(identity, ClientIdentity::FromCredentials(10, 21, 30));
    EXPECT_NE(identity, ClientIdentity::FromCredentials(10, 20, 31));
}

TEST(ClientIdentityTest, EndpointIdentityDiffersFromCredentialIdentity)
{
    EXPECT_NE(ClientIdentity::FromEndpoint(7, 7), ClientIdentity::FromCredentials(7, 7, 0));
    EXPECT_EQ(ClientIdentity::FromEndpoint(7, 3), ClientIdentity::FromEndpoint(7, 3));
    EXPECT_NE(ClientIdentity::FromEndpoint(7, 3), ClientIdentity::FromEndpoint(8, 3));
    EXPECT_NE(ClientIdentity::FromEndpoint(7, 3), ClientIdentity::FromEndpoint(7, 4));
}

TEST(ClientIdentityTest, EndpointIdentitySeparatesNodeFromPrincipal)
{
    const auto identity = ClientIdentity::FromEndpoint(11, 5);
    EXPECT_EQ(identity.GetNodeId(), NodeId{11U});
    EXPECT_EQ(identity.GetPrincipalId(), PrincipalId{5U});
    EXPECT_NE(identity.GetNodeId().Raw(), identity.GetPrincipalId().Raw());
}

TEST(ClientIdentityTest, ReconnectOfSameConsumerKeepsPrincipalButChangesNode)
{
    const auto first = ClientIdentity::FromEndpoint(1, 5);
    const auto second = ClientIdentity::FromEndpoint(2, 5);
    EXPECT_EQ(first.GetPrincipalId(), second.GetPrincipalId());
    EXPECT_NE(first.GetNodeId(), second.GetNodeId());
}

TEST(ClientIdentityTest, HashIsConsistentWithEquality)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_EQ(std::hash<ClientIdentity>{}(identity), std::hash<ClientIdentity>{}(ClientIdentity::FromCredentials(10, 20, 30)));

    std::unordered_set<ClientIdentity> identities{identity, ClientIdentity::FromCredentials(10, 20, 30)};
    EXPECT_EQ(identities.size(), 1U);
    identities.insert(ClientIdentity::FromEndpoint(1, 2));
    EXPECT_EQ(identities.size(), 2U);
}

TEST(ClientIdentityTest, ToStringDescribesIdentity)
{
    EXPECT_EQ(ClientIdentity::FromCredentials(10, 20, 30).ToString(), "pid=10, uid=20, gid=30");
    EXPECT_EQ(ClientIdentity::FromEndpoint(4, 5).ToString(), "connection=node=4, consumer=5");
}

TEST(ClientIdentityTest, IsAllowedWithoutListAllowsEverybody)
{
    EXPECT_TRUE(ClientIdentity::FromCredentials(10, 20, 30).IsAllowed(ClientIdentity::AllowedPrincipals{}));
}

TEST(ClientIdentityTest, IsAllowedChecksMembershipInList)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_TRUE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{PrincipalId{1U}, PrincipalId{20U}}}));
    EXPECT_FALSE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{PrincipalId{1U}, PrincipalId{2U}}}));
    EXPECT_FALSE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{std::set<PrincipalId>{}}));
}

TEST(ClientIdentityTest, IsAllowedSinglePrincipal)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_TRUE(identity.IsAllowed(PrincipalId{20U}));
    EXPECT_FALSE(identity.IsAllowed(PrincipalId{21U}));
}

TEST(ClientIdentityTest, EndpointIdentityIsAllowedByConsumerId)
{
    const auto identity = ClientIdentity::FromEndpoint(9, 5);
    EXPECT_TRUE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{PrincipalId{5U}}}));
    EXPECT_FALSE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{PrincipalId{6U}}}));
}

}  // namespace
}  // namespace score::message_passing
