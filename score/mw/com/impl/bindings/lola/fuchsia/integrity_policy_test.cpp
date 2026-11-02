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
#include "score/mw/com/impl/bindings/lola/fuchsia/integrity_policy.h"

#include <gtest/gtest.h>

namespace score::mw::com::impl::lola::fuchsia
{
namespace
{

TEST(IntegrityPolicyTest, QmConsumerMayNotReceiveAsilBControl)
{
    EXPECT_FALSE(MayReceiveWritableControl(Integrity::kQm, Integrity::kAsilB));
}

TEST(IntegrityPolicyTest, AsilBConsumerMayReceiveAsilBControl)
{
    EXPECT_TRUE(MayReceiveWritableControl(Integrity::kAsilB, Integrity::kAsilB));
}

TEST(IntegrityPolicyTest, SameLevelQmIsAllowed)
{
    EXPECT_TRUE(MayReceiveWritableControl(Integrity::kQm, Integrity::kQm));
}

TEST(IntegrityPolicyTest, HigherIntegrityConsumerMayReceiveQmControl)
{
    EXPECT_TRUE(MayReceiveWritableControl(Integrity::kAsilB, Integrity::kQm));
}

TEST(IntegrityPolicyTest, ServiceIntegrityFollowsPublishedControlRegions)
{
    EXPECT_EQ(ServiceIntegrity(false), Integrity::kQm);
    EXPECT_EQ(ServiceIntegrity(true), Integrity::kAsilB);
}

}  // namespace
}  // namespace score::mw::com::impl::lola::fuchsia
