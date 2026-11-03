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
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_service_discovery_client.h"

#include "score/mw/com/impl/bindings/lola/fuchsia/fake_discovery_transport.h"
#include "score/mw/com/impl/com_error.h"
#include "score/mw/com/impl/configuration/service_identifier_type.h"
#include "score/mw/com/impl/configuration/test/configuration_store.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{
namespace
{

using test::FakeDiscoveryState;
using test::FakeDiscoveryTransport;
using Selector = IServiceDiscovery::QualityTypeSelector;

constexpr LolaServiceId kServiceId{0x1234U};
const auto kSpecifierQm = InstanceSpecifier::Create(std::string{"/lola/fuchsia/qm"}).value();
const auto kSpecifierAsilB = InstanceSpecifier::Create(std::string{"/lola/fuchsia/asilb"}).value();
const auto kSpecifierAny = InstanceSpecifier::Create(std::string{"/lola/fuchsia/any"}).value();

ConfigurationStore kQm{kSpecifierQm, make_ServiceIdentifierType("foo"), QualityType::kASIL_QM, kServiceId,
                       LolaServiceInstanceId{1U}};
ConfigurationStore kAsilB{kSpecifierAsilB, make_ServiceIdentifierType("foo"), QualityType::kASIL_B, kServiceId,
                          LolaServiceInstanceId{2U}};
ConfigurationStore kAnyQm{kSpecifierAny, make_ServiceIdentifierType("foo"), QualityType::kASIL_QM, kServiceId,
                          std::nullopt};

class FuchsiaServiceDiscoveryClientTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        client_ = std::make_unique<FuchsiaServiceDiscoveryClient>(std::make_unique<FakeDiscoveryTransport>(state_));
    }

    FindServiceHandler<HandleType> Recorder()
    {
        return [this](ServiceHandleContainer<HandleType> handles, FindServiceHandle) {
            calls_.push_back(std::vector<HandleType>{handles.begin(), handles.end()});
        };
    }

    FakeDiscoveryState state_;
    std::unique_ptr<FuchsiaServiceDiscoveryClient> client_;
    std::vector<std::vector<HandleType>> calls_;
};

TEST_F(FuchsiaServiceDiscoveryClientTest, OfferQmOffersQmOnlyWithServiceAndInstanceIdOnTheWire)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());

    ASSERT_EQ(state_.offers.size(), 1U);
    const auto& [key, epoch] = *state_.offers.begin();
    EXPECT_EQ(std::get<0>(key), 0x1234U);
    EXPECT_EQ(std::get<1>(key), 1U);
    EXPECT_EQ(std::get<2>(key), WireQuality::kQm);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, OfferAsilBOffersBothQualitiesWithTheSameEpoch)
{
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());

    ASSERT_EQ(state_.offers.size(), 2U);
    EXPECT_EQ(state_.offers.at({0x1234U, 2U, WireQuality::kQm}), state_.offers.at({0x1234U, 2U, WireQuality::kAsilB}));
}

TEST_F(FuchsiaServiceDiscoveryClientTest, OfferingTwiceFails)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    const auto result = client_->OfferService(kQm.GetInstanceIdentifier());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kBindingFailure);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, OfferRefusedByDiscoveryFailsAndCanBeRetried)
{
    state_.rejected_services.insert(0x1234U);
    const auto result = client_->OfferService(kQm.GetInstanceIdentifier());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kServiceNotOffered);
    EXPECT_TRUE(state_.offers.empty());

    state_.rejected_services.clear();
    EXPECT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
}

TEST_F(FuchsiaServiceDiscoveryClientTest, OfferWithDiscoveryUnreachableFails)
{
    state_.reachable = false;
    const auto result = client_->OfferService(kQm.GetInstanceIdentifier());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kServiceNotOffered);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, FailedSecondQualityRollsBackTheFirst)
{
    // ASIL-B is offered first; make the registry drop out after one successful offer.
    struct Flaky final : IDiscoveryTransport
    {
        explicit Flaky(FakeDiscoveryState& s) : inner{s} {}
        OfferStatus Offer(const WireInstance& i) override
        {
            return (calls++ == 0) ? inner.Offer(i) : OfferStatus::kUnavailable;
        }
        void StopOffer(const WireInstance& i) override { inner.StopOffer(i); }
        std::optional<std::vector<WireInstance>> Find(const WireFilter& f) override { return inner.Find(f); }
        std::unique_ptr<ISearch> StartSearch(const WireFilter& f, SnapshotHandler a, ClosedHandler b) override
        {
            return inner.StartSearch(f, a, b);
        }
        void SetRegistryLostHandler(std::function<void()> h) override { inner.SetRegistryLostHandler(h); }
        void PostDelayed(std::function<void()> t, std::chrono::milliseconds d) override { inner.PostDelayed(t, d); }
        FakeDiscoveryTransport inner;
        int calls{0};
    };
    FuchsiaServiceDiscoveryClient client{std::make_unique<Flaky>(state_)};

    EXPECT_FALSE(client.OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    EXPECT_TRUE(state_.offers.empty());
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopOfferBothWithdrawsEverythingAndAllowsOfferingAgain)
{
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_->StopOfferService(kAsilB.GetInstanceIdentifier(), Selector::kBoth).has_value());
    EXPECT_TRUE(state_.offers.empty());
    EXPECT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    EXPECT_EQ(state_.offers.size(), 2U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopOfferQmKeepsAsilB)
{
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_->StopOfferService(kAsilB.GetInstanceIdentifier(), Selector::kAsilQm).has_value());
    ASSERT_EQ(state_.offers.size(), 1U);
    EXPECT_EQ(std::get<2>(state_.offers.begin()->first), WireQuality::kAsilB);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopOfferOfNeverOfferedServiceFails)
{
    const auto result = client_->StopOfferService(kQm.GetInstanceIdentifier(), Selector::kBoth);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kBindingFailure);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, FindServiceOnlyReturnsOffersOfTheRequestedQuality)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());

    const auto qm = client_->FindService(EnrichedInstanceIdentifier{kAnyQm.GetInstanceIdentifier()});
    ASSERT_TRUE(qm.has_value());
    EXPECT_EQ(qm.value().size(), 2U);  // find-any, QM: instance 1 (QM) and instance 2 (ASIL-B provides QM too)

    const auto specific = client_->FindService(EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()});
    ASSERT_TRUE(specific.has_value());
    ASSERT_EQ(specific.value().size(), 1U);
    EXPECT_EQ(specific.value().front(), kQm.GetHandle());
}

TEST_F(FuchsiaServiceDiscoveryClientTest, FindServiceReturnsNothingForNothingOffered)
{
    const auto result = client_->FindService(EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().empty());
}

TEST_F(FuchsiaServiceDiscoveryClientTest, FindServiceFailsWhenDiscoveryIsUnreachable)
{
    state_.reachable = false;
    const auto result = client_->FindService(EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kBindingFailure);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StartFindAfterOfferCallsHandlerBeforeReturning)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());

    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());

    ASSERT_EQ(calls_.size(), 1U);
    EXPECT_EQ(calls_[0], std::vector<HandleType>{kQm.GetHandle()});

    // The first snapshot of the search itself carries nothing new.
    state_.Pump();
    EXPECT_EQ(calls_.size(), 1U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StartFindBeforeOfferCallsHandlerOnOffer)
{
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());
    state_.Pump();
    EXPECT_TRUE(calls_.empty());

    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Pump();

    ASSERT_EQ(calls_.size(), 1U);
    EXPECT_EQ(calls_[0], std::vector<HandleType>{kQm.GetHandle()});
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopOfferCallsHandlerWithEmptySetAndOfferAgainWithTheHandle)
{
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Pump();
    ASSERT_TRUE(client_->StopOfferService(kQm.GetInstanceIdentifier(), Selector::kBoth).has_value());
    state_.Pump();
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Pump();

    ASSERT_EQ(calls_.size(), 3U);
    EXPECT_EQ(calls_[0].size(), 1U);
    EXPECT_TRUE(calls_[1].empty());
    EXPECT_EQ(calls_[2], std::vector<HandleType>{kQm.GetHandle()});
}

TEST_F(FuchsiaServiceDiscoveryClientTest, FindAnyReportsTheFullSetOnEveryChange)
{
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kAnyQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Pump();
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    state_.Pump();

    // The ASIL-B offer arrives as QM and as ASIL-B; the QM search sees one change, not two.
    ASSERT_EQ(calls_.size(), 2U);
    EXPECT_EQ(calls_[0].size(), 1U);
    EXPECT_EQ(calls_[1].size(), 2U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, ChangeOfOnlyTheEpochIsNotReported)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_EQ(calls_.size(), 1U);

    // The skeleton re-offers with a new epoch without a gap the search could see.
    for (auto& [key, epoch] : state_.offers)
    {
        ++epoch;
    }
    state_.Notify();
    state_.Pump();

    EXPECT_EQ(calls_.size(), 1U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, NoHandlerCallAfterStopFind)
{
    const auto handle = make_FindServiceHandle(1U);
    ASSERT_TRUE(
        client_->StartFindService(handle, Recorder(), EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
            .has_value());
    ASSERT_TRUE(client_->StopFindService(handle).has_value());

    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Pump();

    EXPECT_TRUE(calls_.empty());
    EXPECT_EQ(state_.LiveSearches(), 0U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopFindOfUnknownHandleIsOk)
{
    EXPECT_TRUE(client_->StopFindService(make_FindServiceHandle(42U)).has_value());
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopFindFromWithinTheHandlerIsAllowed)
{
    const auto handle = make_FindServiceHandle(1U);
    int calls = 0;
    ASSERT_TRUE(client_
                    ->StartFindService(
                        handle,
                        [this, &calls](ServiceHandleContainer<HandleType>, FindServiceHandle h) {
                            ++calls;
                            EXPECT_TRUE(client_->StopFindService(h).has_value());
                        },
                        EnrichedInstanceIdentifier{kAnyQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Pump();
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    state_.Pump();

    EXPECT_EQ(calls, 1);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, SearchesAreIndependent)
{
    std::vector<std::vector<HandleType>> other;
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_TRUE(client_
                    ->StartFindService(
                        make_FindServiceHandle(2U),
                        [&other](ServiceHandleContainer<HandleType> h, FindServiceHandle) {
                            other.emplace_back(h.begin(), h.end());
                        },
                        EnrichedInstanceIdentifier{kAnyQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    state_.Pump();

    EXPECT_TRUE(calls_.empty());  // instance 1 was not offered
    EXPECT_EQ(other.size(), 1U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, DiscoveryRestartReestablishesTheSearchAndReportsLostOffers)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kAnyQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_EQ(calls_.size(), 1U);

    // A second provider whose offer is lost with the restart (this client's own is offered again).
    state_.offers[{0x1234U, 7U, WireQuality::kQm}] = 1U;
    state_.Notify();
    state_.Pump();
    ASSERT_EQ(calls_.size(), 2U);

    state_.Restart();
    state_.Pump();  // search closed, registry lost: own offers are offered again, restart is scheduled
    EXPECT_EQ(state_.offers.size(), 1U);
    ASSERT_EQ(state_.delayed.size(), 1U);
    EXPECT_EQ(state_.delayed.front().first, std::chrono::milliseconds{10});

    ASSERT_TRUE(state_.RunDelayed());

    EXPECT_EQ(state_.LiveSearches(), 1U);
    ASSERT_EQ(calls_.size(), 3U);  // instance 7 is gone
    EXPECT_EQ(calls_[2], std::vector<HandleType>{kAnyQm.GetHandle(ServiceInstanceId{LolaServiceInstanceId{1U}})});
}

TEST_F(FuchsiaServiceDiscoveryClientTest, RestartThatChangesNothingIsInvisible)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());
    ASSERT_EQ(calls_.size(), 1U);

    state_.Restart();
    state_.Pump();
    ASSERT_TRUE(state_.RunDelayed());

    EXPECT_EQ(calls_.size(), 1U);
    EXPECT_EQ(state_.LiveSearches(), 1U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, RestartRetriesWithBackoffWhileDiscoveryIsAway)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());

    state_.Restart(true);
    state_.Pump();
    for (int i = 0; i < 14; ++i)
    {
        ASSERT_TRUE(state_.RunDelayed());
    }

    // Own offers are retried in between (100 ms); the search backs off 10, 20, ... up to 500 ms.
    std::vector<std::int64_t> search_delays;
    for (const auto delay : state_.delays_requested)
    {
        if (delay != std::chrono::milliseconds{100})
        {
            search_delays.push_back(delay.count());
        }
    }
    ASSERT_GE(search_delays.size(), 7U);
    EXPECT_EQ(search_delays[0], 10);
    EXPECT_EQ(search_delays[1], 20);
    EXPECT_EQ(search_delays[2], 40);
    EXPECT_EQ(search_delays[6], 500);

    state_.reachable = true;
    while (state_.LiveSearches() == 0U || state_.offers.empty())
    {
        ASSERT_TRUE(state_.RunDelayed());
    }
    state_.Pump();
    EXPECT_EQ(state_.offers.size(), 1U);  // own offer is back
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StopFindWhileRestartIsPendingCancelsIt)
{
    const auto handle = make_FindServiceHandle(1U);
    ASSERT_TRUE(
        client_->StartFindService(handle, Recorder(), EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
            .has_value());
    state_.Restart();
    state_.Pump();
    ASSERT_TRUE(client_->StopFindService(handle).has_value());

    while (state_.RunDelayed())
    {
    }

    EXPECT_EQ(state_.LiveSearches(), 0U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StartFindWithDiscoveryAbsentDeliversOnceItIsThere)
{
    state_.reachable = false;
    ASSERT_TRUE(client_
                    ->StartFindService(make_FindServiceHandle(1U),
                                       Recorder(),
                                       EnrichedInstanceIdentifier{kQm.GetInstanceIdentifier()})
                    .has_value());
    state_.Pump();
    EXPECT_TRUE(calls_.empty());

    state_.reachable = true;
    state_.offers[{0x1234U, 1U, WireQuality::kQm}] = 1U;
    ASSERT_TRUE(state_.RunDelayed());

    ASSERT_EQ(calls_.size(), 1U);
    EXPECT_EQ(calls_[0], std::vector<HandleType>{kQm.GetHandle()});
}

TEST_F(FuchsiaServiceDiscoveryClientTest, RegistryLossOffersEverythingAgain)
{
    ASSERT_TRUE(client_->OfferService(kAsilB.GetInstanceIdentifier()).has_value());
    const auto before = state_.offers;

    state_.offers.clear();
    ASSERT_TRUE(static_cast<bool>(state_.registry_lost));
    state_.registry_lost();

    EXPECT_EQ(state_.offers.size(), before.size());
    for (const auto& [key, epoch] : before)
    {
        ASSERT_EQ(state_.offers.count(key), 1U);
        EXPECT_EQ(state_.offers.at(key), epoch);  // same incarnation of the offer
    }
}

TEST_F(FuchsiaServiceDiscoveryClientTest, RegistryLossWithDiscoveryAwayRetriesTheOffers)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    state_.Restart(true);
    state_.Pump();
    EXPECT_TRUE(state_.offers.empty());

    state_.reachable = true;
    ASSERT_TRUE(state_.RunDelayed());

    EXPECT_EQ(state_.offers.size(), 1U);
}

TEST_F(FuchsiaServiceDiscoveryClientTest, StoppedOfferIsNotOfferedAgainAfterRegistryLoss)
{
    ASSERT_TRUE(client_->OfferService(kQm.GetInstanceIdentifier()).has_value());
    ASSERT_TRUE(client_->StopOfferService(kQm.GetInstanceIdentifier(), Selector::kBoth).has_value());

    state_.registry_lost();

    EXPECT_TRUE(state_.offers.empty());
}

}  // namespace
}  // namespace score::mw::com::impl::lola::fuchsia
