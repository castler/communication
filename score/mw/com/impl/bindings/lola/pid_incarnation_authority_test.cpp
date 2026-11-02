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
#include "score/mw/com/impl/bindings/lola/pid_incarnation_authority.h"

#include <gtest/gtest.h>

namespace score::mw::com::impl::lola
{
namespace
{

TEST(PidIncarnationAuthorityTest, SelfIsThePid)
{
    const PidIncarnationAuthority unit{4711};
    EXPECT_EQ(unit.Self(), IncarnationId::FromPid(4711));
    EXPECT_EQ(unit.Self().ToPid(), 4711);
}

TEST(PidIncarnationAuthorityTest, SelfMapsToNodeIdOfPid)
{
    const PidIncarnationAuthority unit{4711};
    EXPECT_EQ(unit.Self().ToNodeId(), message_passing::NodeId::FromPid(4711));
}

TEST(PidIncarnationAuthorityTest, LivenessIsUnknown)
{
    const PidIncarnationAuthority unit{1};
    EXPECT_EQ(unit.Query(IncarnationId::FromPid(1)), Liveness::kUnknown);
    EXPECT_EQ(unit.Query(IncarnationId::FromPid(2)), Liveness::kUnknown);
}

TEST(PidIncarnationAuthorityTest, AlwaysTakesOver)
{
    EXPECT_EQ(PidIncarnationAuthority{1}.GetTakeoverPolicy(), TakeoverPolicy::kAlwaysTakeover);
}

TEST(PidIncarnationAuthorityTest, HasNoProviderSideRollback)
{
    EXPECT_FALSE(PidIncarnationAuthority{1}.SupportsProviderSideRollback());
}

}  // namespace
}  // namespace score::mw::com::impl::lola
