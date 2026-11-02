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
#include "score/mw/com/impl/bindings/lola/fuchsia/broker_core.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/incarnation_header.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{
namespace
{

const InstanceKey kInstance{0x1234U, 1U, 1U, 0U};
const InstanceKey kOther{0x1234U, 2U, 1U, 0U};

class BrokerCoreTest : public ::testing::Test
{
  protected:
    BrokerCoreTest()
        : core_{[this] { return now_; },
                BrokerCoreHooks{[this](const InstanceKey&, Epoch epoch, BrokerFenceReason reason) {
                                    events_.push_back("fence:" + std::to_string(epoch));
                                    last_reason_ = reason;
                                },
                                [this](const InstanceKey&, Epoch epoch) { events_.push_back("release:" + std::to_string(epoch)); },
                                [this](LeaseId lease, LeaseKind kind) {
                                    events_.push_back(std::string{kind == LeaseKind::kSkeleton ? "skel_end:" : "proxy_end:"} +
                                                      std::to_string(lease));
                                }},
                BrokerLimits{2U}}
    {
    }

    Epoch Provision(const InstanceKey& key, LeaseId* lease = nullptr)
    {
        Epoch epoch{};
        LeaseId skeleton{};
        EXPECT_EQ(core_.BeginIncarnation(key, epoch, skeleton), BrokerStatus::kOk);
        EXPECT_EQ(core_.Publish(key, epoch), BrokerStatus::kOk);
        if (lease != nullptr)
        {
            *lease = skeleton;
        }
        return epoch;
    }

    std::uint64_t now_{1000U};
    std::vector<std::string> events_;
    BrokerFenceReason last_reason_{BrokerFenceReason::kNone};
    BrokerCore core_;
};

TEST_F(BrokerCoreTest, EpochIsStrictlyIncreasingEvenIfTheClockStandsStillOrGoesBack)
{
    LeaseId lease{};
    const Epoch first = Provision(kInstance, &lease);
    EXPECT_EQ(first, 1000U);
    core_.SkeletonLost(lease, BrokerFenceReason::kLivenessClosed);
    now_ = 10U;
    EXPECT_EQ(Provision(kInstance), 1001U);
}

TEST_F(BrokerCoreTest, SecondSkeletonIsRefusedWhileTheFirstIncarnationIsLive)
{
    Provision(kInstance);
    Epoch epoch{};
    LeaseId lease{};
    EXPECT_EQ(core_.BeginIncarnation(kInstance, epoch, lease), BrokerStatus::kAlreadyProvisioned);
    EXPECT_EQ(core_.BeginIncarnation(kOther, epoch, lease), BrokerStatus::kOk);
}

TEST_F(BrokerCoreTest, SkeletonDeathWithoutProxiesFencesAndReleasesAtOnce)
{
    LeaseId lease{};
    const Epoch epoch = Provision(kInstance, &lease);
    core_.SkeletonLost(lease, BrokerFenceReason::kProcessTerminated);

    EXPECT_EQ(events_, (std::vector<std::string>{"fence:1000", "skel_end:1", "release:1000"}));
    EXPECT_EQ(last_reason_, BrokerFenceReason::kProcessTerminated);
    const auto stats = core_.Stats(kInstance);
    EXPECT_EQ(stats.active_epoch, 0U);
    EXPECT_EQ(stats.retained_incarnations, 0U);
    EXPECT_EQ(stats.fenced_by_process, 1U);
    EXPECT_EQ(epoch, 1000U);
}

TEST_F(BrokerCoreTest, FencedIncarnationStaysPinnedUntilTheLastProxyLeaseEnds)
{
    LeaseId skeleton{};
    const Epoch epoch = Provision(kInstance, &skeleton);
    LeaseId p1{};
    LeaseId p2{};
    ASSERT_EQ(core_.RegisterProxy(kInstance, epoch, p1), BrokerStatus::kOk);
    ASSERT_EQ(core_.RegisterProxy(kInstance, epoch, p2), BrokerStatus::kOk);

    core_.SkeletonLost(skeleton, BrokerFenceReason::kLivenessClosed);
    auto stats = core_.Stats(kInstance);
    EXPECT_EQ(stats.draining_incarnations, 1U);
    EXPECT_EQ(stats.retained_incarnations, 1U);
    EXPECT_EQ(stats.released_total, 0U);

    EXPECT_EQ(core_.ProxyEnded(p1), BrokerStatus::kOk);
    EXPECT_EQ(core_.Stats(kInstance).released_total, 0U);
    EXPECT_EQ(core_.ProxyEnded(p2), BrokerStatus::kOk);
    stats = core_.Stats(kInstance);
    EXPECT_EQ(stats.released_total, 1U);
    EXPECT_EQ(stats.retained_incarnations, 0U);
    EXPECT_EQ(events_.back(), "release:1000");
}

TEST_F(BrokerCoreTest, DeathNoticesAreIdempotent)
{
    LeaseId skeleton{};
    const Epoch epoch = Provision(kInstance, &skeleton);
    LeaseId proxy{};
    ASSERT_EQ(core_.RegisterProxy(kInstance, epoch, proxy), BrokerStatus::kOk);
    core_.SkeletonLost(skeleton, BrokerFenceReason::kProcessTerminated);
    core_.SkeletonLost(skeleton, BrokerFenceReason::kLivenessClosed);
    core_.Resign(kInstance, epoch);
    EXPECT_EQ(core_.Stats(kInstance).fenced_total, 1U);
    EXPECT_EQ(core_.Stats(kInstance).fenced_by_liveness, 0U);
    EXPECT_EQ(core_.ProxyEnded(proxy), BrokerStatus::kOk);
    EXPECT_EQ(core_.ProxyEnded(proxy), BrokerStatus::kUnknownLease);
    EXPECT_EQ(core_.Stats(kInstance).released_total, 1U);
}

TEST_F(BrokerCoreTest, RestartedSkeletonGetsANewEpochWhileTheOldOneDrains)
{
    LeaseId skeleton{};
    const Epoch e1 = Provision(kInstance, &skeleton);
    LeaseId proxy{};
    ASSERT_EQ(core_.RegisterProxy(kInstance, e1, proxy), BrokerStatus::kOk);
    core_.SkeletonLost(skeleton, BrokerFenceReason::kProcessTerminated);

    now_ = 5000U;
    const Epoch e2 = Provision(kInstance);
    EXPECT_GT(e2, e1);
    auto stats = core_.Stats(kInstance);
    EXPECT_EQ(stats.active_epoch, e2);
    EXPECT_EQ(stats.retained_incarnations, 2U);

    LeaseId reattached{};
    EXPECT_EQ(core_.RegisterProxy(kInstance, e1, reattached), BrokerStatus::kStaleEpoch);
    ASSERT_EQ(core_.RegisterProxy(kInstance, e2, reattached), BrokerStatus::kOk);
    EXPECT_EQ(core_.ProxyEnded(proxy), BrokerStatus::kOk);
    stats = core_.Stats(kInstance);
    EXPECT_EQ(stats.retained_incarnations, 1U);
    EXPECT_EQ(stats.draining_incarnations, 0U);
    EXPECT_EQ(stats.live_proxy_leases, 1U);
}

TEST_F(BrokerCoreTest, QuarantineBudgetStopsNewIncarnations)
{
    std::vector<LeaseId> proxies;
    for (int i = 0; i < 2; ++i)
    {
        LeaseId skeleton{};
        const Epoch epoch = Provision(kInstance, &skeleton);
        LeaseId proxy{};
        ASSERT_EQ(core_.RegisterProxy(kInstance, epoch, proxy), BrokerStatus::kOk);
        proxies.push_back(proxy);
        core_.SkeletonLost(skeleton, BrokerFenceReason::kLivenessClosed);
    }
    Epoch epoch{};
    LeaseId lease{};
    EXPECT_EQ(core_.BeginIncarnation(kInstance, epoch, lease), BrokerStatus::kResourceExhausted);
    EXPECT_EQ(core_.ProxyEnded(proxies[0]), BrokerStatus::kOk);
    EXPECT_EQ(core_.BeginIncarnation(kInstance, epoch, lease), BrokerStatus::kOk);
}

TEST_F(BrokerCoreTest, SkeletonDyingWhileProvisioningLeavesNothingBehind)
{
    Epoch epoch{};
    LeaseId lease{};
    ASSERT_EQ(core_.BeginIncarnation(kInstance, epoch, lease), BrokerStatus::kOk);
    core_.SkeletonLost(lease, BrokerFenceReason::kLivenessClosed);
    EXPECT_EQ(core_.Stats(kInstance).retained_incarnations, 0U);
    EXPECT_EQ(core_.Publish(kInstance, epoch), BrokerStatus::kStaleEpoch);
    EXPECT_EQ(core_.BeginIncarnation(kInstance, epoch, lease), BrokerStatus::kOk);
}

TEST_F(BrokerCoreTest, ProxyCannotRegisterOnFencedOrUnknownIncarnation)
{
    LeaseId proxy{};
    EXPECT_EQ(core_.RegisterProxy(kInstance, 1U, proxy), BrokerStatus::kNotActive);
    LeaseId skeleton{};
    const Epoch epoch = Provision(kInstance, &skeleton);
    core_.Resign(kInstance, epoch);
    EXPECT_EQ(core_.RegisterProxy(kInstance, epoch, proxy), BrokerStatus::kNotActive);
    EXPECT_EQ(last_reason_, BrokerFenceReason::kResigned);
}

TEST_F(BrokerCoreTest, RestartedBrokerAdoptsALiveIncarnationAndKeepsEpochsMonotonic)
{
    LeaseId skeleton{};
    ASSERT_EQ(core_.AdoptIncarnation(kInstance, 5000U, skeleton), BrokerStatus::kOk);
    CoreStats stats = core_.Stats(kInstance);
    EXPECT_EQ(stats.active_epoch, 5000U);
    EXPECT_EQ(stats.last_epoch, 5000U);

    LeaseId proxy{};
    EXPECT_EQ(core_.RegisterProxy(kInstance, 5000U, proxy), BrokerStatus::kOk);
    core_.SkeletonLost(skeleton, BrokerFenceReason::kProcessTerminated);
    core_.ProxyEnded(proxy);

    now_ = 100U;  // behind the adopted epoch
    EXPECT_EQ(Provision(kInstance), 5001U);
}

TEST_F(BrokerCoreTest, AdoptionIsRefusedWhileTheInstanceHasALiveIncarnation)
{
    LeaseId lease{};
    const Epoch live = Provision(kInstance);
    EXPECT_EQ(core_.AdoptIncarnation(kInstance, live + 10U, lease), BrokerStatus::kAlreadyProvisioned);
    EXPECT_EQ(core_.AdoptIncarnation(kOther, 0U, lease), BrokerStatus::kStaleEpoch);
    EXPECT_EQ(core_.AdoptIncarnation(kOther, UINT64_MAX, lease), BrokerStatus::kStaleEpoch);
    EXPECT_EQ(core_.AdoptIncarnation(kOther, 77U, lease), BrokerStatus::kOk);
    EXPECT_EQ(core_.AdoptIncarnation(kOther, 78U, lease), BrokerStatus::kAlreadyProvisioned);
}

TEST(IncarnationHeaderTest, FenceIsVisibleToTheReaderOfThatEpochOnly)
{
    IncarnationHeader header{};
    EXPECT_FALSE(IsIncarnationLive(header, 7U));
    PublishIncarnation(header, 7U);
    EXPECT_TRUE(IsIncarnationLive(header, 7U));
    EXPECT_FALSE(IsIncarnationLive(header, 8U));
    FenceIncarnation(header);
    EXPECT_FALSE(IsIncarnationLive(header, 7U));
    EXPECT_TRUE(IsIncarnationFenced(header));
}

}  // namespace
}  // namespace score::mw::com::impl::lola::fuchsia
