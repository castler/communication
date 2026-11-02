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

#include <unordered_set>

namespace score::message_passing
{
namespace
{

TEST(ClientIdentityTest, CredentialsExposeNodeAndPrincipal)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_EQ(identity.GetNodeId(), 10);
    EXPECT_EQ(identity.GetPrincipalId(), 20U);
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
    EXPECT_NE(ClientIdentity::FromEndpoint(7), ClientIdentity::FromCredentials(7, 7, 0));
    EXPECT_EQ(ClientIdentity::FromEndpoint(7), ClientIdentity::FromEndpoint(7));
    EXPECT_NE(ClientIdentity::FromEndpoint(7), ClientIdentity::FromEndpoint(8));
}

TEST(ClientIdentityTest, HashIsConsistentWithEquality)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_EQ(std::hash<ClientIdentity>{}(identity), std::hash<ClientIdentity>{}(ClientIdentity::FromCredentials(10, 20, 30)));

    std::unordered_set<ClientIdentity> identities{identity, ClientIdentity::FromCredentials(10, 20, 30)};
    EXPECT_EQ(identities.size(), 1U);
    identities.insert(ClientIdentity::FromEndpoint(1));
    EXPECT_EQ(identities.size(), 2U);
}

TEST(ClientIdentityTest, ToStringDescribesIdentity)
{
    EXPECT_EQ(ClientIdentity::FromCredentials(10, 20, 30).ToString(), "pid=10, uid=20, gid=30");
    EXPECT_EQ(ClientIdentity::FromEndpoint(5).ToString(), "consumer=5");
}

TEST(ClientIdentityTest, IsAllowedWithoutListAllowsEverybody)
{
    EXPECT_TRUE(ClientIdentity::FromCredentials(10, 20, 30).IsAllowed(ClientIdentity::AllowedPrincipals{}));
}

TEST(ClientIdentityTest, IsAllowedChecksMembershipInList)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_TRUE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{1U, 20U}}));
    EXPECT_FALSE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{1U, 2U}}));
    EXPECT_FALSE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{std::set<uid_t>{}}));
}

TEST(ClientIdentityTest, IsAllowedSinglePrincipal)
{
    const auto identity = ClientIdentity::FromCredentials(10, 20, 30);
    EXPECT_TRUE(identity.IsAllowed(20U));
    EXPECT_FALSE(identity.IsAllowed(21U));
}

TEST(ClientIdentityTest, EndpointIdentityIsAllowedByConsumerId)
{
    const auto identity = ClientIdentity::FromEndpoint(5);
    EXPECT_TRUE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{5U}}));
    EXPECT_FALSE(identity.IsAllowed(ClientIdentity::AllowedPrincipals{{6U}}));
}

}  // namespace
}  // namespace score::message_passing
