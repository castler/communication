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
#include "score/mw/com/impl/bindings/lola/application_id_pid_mapping.h"

#include "score/mw/com/impl/bindings/lola/incarnation_authority_mock.h"
#include "score/mw/com/impl/bindings/lola/pid_incarnation_authority.h"

#include "score/concurrency/atomic_indirector.h"
#include "score/concurrency/atomic_mock.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace score::mw::com::impl::lola
{
namespace
{

constexpr std::uint16_t kMaxNumberOfMappings{10};
using Allocator = std::allocator<lola::ApplicationIdPidMappingEntry>;
using testing::_;
using testing::Return;

TEST(ApplicationIdPidMapping, RegisterUpToMaxNumberMappingsSucceeds)
{
    // Given a ApplicationIdPidMapping with a max number of supported mappings of kMaxNumberOfMappings
    ApplicationIdPidMapping<Allocator> unit{kMaxNumberOfMappings, Allocator()};
    // when inserting kMaxNumberOfMappings different PIDs
    for (std::uint16_t i = 0; i < kMaxNumberOfMappings; i++)
    {
        auto result = unit.RegisterPid(i, 100 + i);
        // expect, that registering was successful
        EXPECT_TRUE(result.has_value());
        // and that the registered PID is returned
        EXPECT_EQ(result.value(), 100 + i);
    }
    // and when registering another application_id beyond the capacity
    auto result = unit.RegisterPid(42, 142);
    // expect, that the result is empty.
    EXPECT_FALSE(result.has_value());
}

TEST(ApplicationIdPidMapping, RegisterMaxRetryFailure)
{
    // Given kMaxNumberOfMappings, which are all but one "kUsed" for application_id 0 .. kMaxNumberOfMappings - 1
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> mapping_entries(kMaxNumberOfMappings);

    const std::uint16_t unused_entry{5};

    for (std::uint16_t i = 0; i < kMaxNumberOfMappings; i++)
    {
        if (i != unused_entry)
        {
            mapping_entries.at(i).SetStatusAndApplicationIdAtomic(
                ApplicationIdPidMappingEntry::MappingEntryStatus::kUsed, i);
            mapping_entries.at(i).pid_ = i;
        }
        else
        {
            mapping_entries.at(i).SetStatusAndApplicationIdAtomic(
                ApplicationIdPidMappingEntry::MappingEntryStatus::kUnused, i);
            mapping_entries.at(i).pid_ = 0;
        }
    }

    concurrency::AtomicMock<ApplicationIdPidMappingEntry::key_type> atomic_mock{};
    concurrency::AtomicIndirectorMock<ApplicationIdPidMappingEntry::key_type>::SetMockObject(&atomic_mock);

    const std::size_t kMaxRetries{50};
    // Given the operation to update the mapping entry fails kMaxRetries times
    EXPECT_CALL(atomic_mock, compare_exchange_weak(_, _, _)).Times(kMaxRetries).WillRepeatedly(Return(false));

    // when trying to register a new application_id 42 (not among the registered application_ids)
    auto result = lola::detail::RegisterPid<concurrency::AtomicIndirectorMock>(
        mapping_entries.begin(), mapping_entries.end(), 42, 142);
    // expect, that an empty optional is returned.
    EXPECT_FALSE(result.has_value());
}

TEST(ApplicationIdPidMapping, ReregisterPIDSuccess)
{
    // Given a ApplicationIdPidMapping with a max number of supported mappings of kMaxNumberOfMappings
    ApplicationIdPidMapping<Allocator> unit{kMaxNumberOfMappings, Allocator()};
    // when registering a PID for a given application_id
    auto result = unit.RegisterPid(42, 142);
    // expect, that registering was successful
    EXPECT_TRUE(result.has_value());
    // and that the registered PID is returned
    EXPECT_EQ(result.value(), 142);
    // and when registering another PID for the same application_id
    result = unit.RegisterPid(42, 999);
    // expect, that the result is not empty.
    EXPECT_TRUE(result.has_value());
    // and that the previously registered PID is returned
    EXPECT_EQ(result.value(), 142);
}

TEST(ApplicationIdPidMapping, RegisterUpdatesPidWhenEntryIsInUpdatingStateForSameApplicationId)
{
    // Given a single entry array that can be controlled
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> mapping_entries(1);

    const std::uint32_t test_application_id = 42;
    const pid_t new_pid = 200;
    const pid_t old_pid = 100;
    // Set up the entry in kUpdating state for our test application ID
    mapping_entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUpdating,
                                                          test_application_id);
    mapping_entries.at(0).pid_ = old_pid;

    // When tried to register
    auto result =
        lola::detail::RegisterPid(mapping_entries.begin(), mapping_entries.end(), test_application_id, new_pid);

    // Then the operation should succeed and return the new PID
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), new_pid);
}

// Characterization tests: pin the current takeover semantics (always take over, last writer wins, previous value is
// handed back to the taker).
TEST(ApplicationIdPidMapping, EachRegistrationReturnsThePidOfTheMostRecentPreviousRegistration)
{
    ApplicationIdPidMapping<Allocator> unit{kMaxNumberOfMappings, Allocator()};

    // A fresh registration returns the own pid (indistinguishable from "same pid registered again").
    EXPECT_EQ(unit.RegisterPid(42, 100).value(), 100);
    // Every following registration unconditionally takes over and returns the pid it replaced.
    EXPECT_EQ(unit.RegisterPid(42, 101).value(), 100);
    EXPECT_EQ(unit.RegisterPid(42, 102).value(), 101);
    EXPECT_EQ(unit.RegisterPid(42, 103).value(), 102);
    // Registering the already registered pid is a no-op that returns the same pid.
    EXPECT_EQ(unit.RegisterPid(42, 103).value(), 103);
}

TEST(ApplicationIdPidMapping, TakeoverOfOneApplicationIdDoesNotAffectOtherApplicationIds)
{
    ApplicationIdPidMapping<Allocator> unit{kMaxNumberOfMappings, Allocator()};
    ASSERT_EQ(unit.RegisterPid(1, 11).value(), 11);
    ASSERT_EQ(unit.RegisterPid(2, 22).value(), 22);

    EXPECT_EQ(unit.RegisterPid(1, 12).value(), 11);

    EXPECT_EQ(unit.RegisterPid(2, 23).value(), 22);
    EXPECT_EQ(unit.RegisterPid(1, 13).value(), 12);
}

TEST(ApplicationIdPidMapping, TakeoverDoesNotConsumeAdditionalEntries)
{
    ApplicationIdPidMapping<Allocator> unit{1, Allocator()};
    ASSERT_TRUE(unit.RegisterPid(7, 70).has_value());

    // A single entry is sufficient for any number of takeovers of the same application ID ...
    EXPECT_EQ(unit.RegisterPid(7, 71).value(), 70);
    EXPECT_EQ(unit.RegisterPid(7, 72).value(), 71);
    // ... but another application ID finds no free entry.
    EXPECT_FALSE(unit.RegisterPid(8, 80).has_value());
}

TEST(ApplicationIdPidMapping, EntryLeftInUpdatingStateIsFinalizedByTakeoverAndBehavesAsUsedAfterwards)
{
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> mapping_entries(1);
    const std::uint32_t application_id{42U};
    mapping_entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUpdating,
                                                          application_id);
    mapping_entries.at(0).pid_ = 100;

    // Takeover of a half-written entry returns the new pid (the stale pid is NOT reported) ...
    EXPECT_EQ(lola::detail::RegisterPid(mapping_entries.begin(), mapping_entries.end(), application_id, 200).value(),
              200);
    // ... and the entry is kUsed afterwards, i.e. a further takeover reports the pid registered by the first one.
    const auto status_and_id = mapping_entries.at(0).GetStatusAndApplicationIdAtomic();
    EXPECT_EQ(status_and_id.first, ApplicationIdPidMappingEntry::MappingEntryStatus::kUsed);
    EXPECT_EQ(status_and_id.second, application_id);
    EXPECT_EQ(lola::detail::RegisterPid(mapping_entries.begin(), mapping_entries.end(), application_id, 300).value(),
              200);
}

TEST(ApplicationIdPidMapping, EntryInUpdatingStateOfOtherApplicationIdIsNotTakenOver)
{
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> mapping_entries(1);
    mapping_entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUpdating,
                                                          1U);
    mapping_entries.at(0).pid_ = 100;

    EXPECT_FALSE(lola::detail::RegisterPid(mapping_entries.begin(), mapping_entries.end(), 2U, 200).has_value());
    EXPECT_EQ(mapping_entries.at(0).pid_.load(), 100);
}

TEST(ApplicationIdPidMapping, ConcurrentAccess)
{
    // Given a ApplicationIdPidMapping with a max number of supported mappings of 100
    ApplicationIdPidMapping<Allocator> unit{100, Allocator()};
    using namespace std::chrono_literals;

    // given a thread, which tries to Register 30 different application_id/pid pairs, by ...
    auto thread_action = [&unit](std::uint32_t start_application_id) {
        for (int i = 0; i < 30; i++)
        {
            std::uint32_t application_id = start_application_id;
            auto pid = static_cast<pid_t>(application_id + 100);
            for (int j = 0; j < 3; j++)
            {
                // ... calling Register for a specific/unique application_id a pid
                auto result = unit.RegisterPid(application_id, pid);
                // and expects, that register was successful
                EXPECT_TRUE(result.has_value());
                // and returns the newly registered pid
                EXPECT_EQ(result.value(), pid);
                application_id++;
                pid++;
            }
            // and then sleeps
            std::this_thread::sleep_for(50ms);
        }
    };

    // we start 3 of those threads, which will in sum register 90 application_id/pid pairs, which needs to fit in the
    // 100 capacity.
    std::thread t1(thread_action, 100);
    std::thread t2(thread_action, 200);
    std::thread t3(thread_action, 300);
    t1.join();
    t2.join();
    t3.join();
}

TEST(ApplicationIdPidMapping, ConcurrentTakeoversOfSameApplicationIdHandBackEveryPreviousPidExactlyOnce)
{
    constexpr int kThreads{8};
    constexpr int kTakeoversPerThread{2000};
    constexpr std::uint32_t kApplicationId{42U};
    ApplicationIdPidMapping<Allocator> unit{kMaxNumberOfMappings, Allocator()};
    constexpr pid_t kInitialPid{1};
    ASSERT_EQ(unit.RegisterPid(kApplicationId, kInitialPid).value(), kInitialPid);

    // Given several takers racing for the same application id, each registering only unique pids ...
    std::vector<std::vector<pid_t>> previous_pids(kThreads);
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index)
    {
        threads.emplace_back([&unit, &previous_pids, thread_index]() {
            for (int i = 0; i < kTakeoversPerThread; ++i)
            {
                const pid_t pid = 100 + thread_index * kTakeoversPerThread + i;
                previous_pids[static_cast<std::size_t>(thread_index)].push_back(
                    unit.RegisterPid(kApplicationId, pid).value());
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    // expect that every registered pid (including the initial one) was handed back to exactly one taker, except the
    // final owner, i.e. no taker lost or duplicated a previous value.
    std::set<pid_t> returned{};
    std::size_t number_of_returned{0U};
    for (const auto& per_thread : previous_pids)
    {
        returned.insert(per_thread.begin(), per_thread.end());
        number_of_returned += per_thread.size();
    }
    EXPECT_EQ(returned.size(), number_of_returned);
    EXPECT_EQ(number_of_returned, static_cast<std::size_t>(kThreads * kTakeoversPerThread));
    const pid_t final_owner = unit.RegisterPid(kApplicationId, 7).value();
    EXPECT_EQ(returned.count(final_owner), 0U);
    EXPECT_EQ(returned.count(kInitialPid), 1U);
}

class TakeoverFixture : public ::testing::Test
{
  protected:
    static constexpr std::uint32_t kAppId{42U};
    static IncarnationId Pid(const pid_t pid)
    {
        return IncarnationId::FromPid(pid);
    }

    ApplicationIdPidMapping<Allocator> unit_{kMaxNumberOfMappings, Allocator()};
    PidIncarnationAuthority linux_authority_{1};
    testing::NiceMock<IncarnationAuthorityMock> require_dead_authority_{};
};

TEST_F(TakeoverFixture, FirstTakeoverIsFreshAndRepeatedTakeoverBySameIncarnationIsSame)
{
    const auto first = unit_.Takeover(kAppId, Pid(100), linux_authority_);
    EXPECT_EQ(first.result, TakeoverResult::kFresh);
    EXPECT_FALSE(first.previous.IsValid());

    const auto second = unit_.Takeover(kAppId, Pid(100), linux_authority_);
    EXPECT_EQ(second.result, TakeoverResult::kSame);
    EXPECT_EQ(second.previous, Pid(100));
}

TEST_F(TakeoverFixture, AlwaysTakeoverPolicyTakesOverFromAnyPreviousIncarnationAndReportsIt)
{
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(100), linux_authority_).result, TakeoverResult::kFresh);

    const auto outcome = unit_.Takeover(kAppId, Pid(101), linux_authority_);
    EXPECT_EQ(outcome.result, TakeoverResult::kTookOver);
    EXPECT_EQ(outcome.previous, Pid(100));
    EXPECT_EQ(unit_.Takeover(kAppId, Pid(102), linux_authority_).previous, Pid(101));
}

TEST_F(TakeoverFixture, TakeoverIsCompatibleWithRegisterPid)
{
    ASSERT_EQ(unit_.RegisterPid(kAppId, 100).value(), 100);
    EXPECT_EQ(unit_.Takeover(kAppId, Pid(101), linux_authority_).previous, Pid(100));
    EXPECT_EQ(unit_.RegisterPid(kAppId, 102).value(), 101);
}

TEST_F(TakeoverFixture, RequireDeadPolicyRefusesLivePreviousIncarnationAndLeavesOwnerUntouched)
{
    ON_CALL(require_dead_authority_, GetTakeoverPolicy()).WillByDefault(Return(TakeoverPolicy::kRequireDead));
    ON_CALL(require_dead_authority_, Query(Pid(100))).WillByDefault(Return(Liveness::kAlive));
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(100), require_dead_authority_).result, TakeoverResult::kFresh);

    const auto outcome = unit_.Takeover(kAppId, Pid(101), require_dead_authority_);

    EXPECT_EQ(outcome.result, TakeoverResult::kRefusedLive);
    EXPECT_EQ(outcome.previous, Pid(100));
    EXPECT_EQ(unit_.RegisterPid(kAppId, 100).value(), 100);
}

TEST_F(TakeoverFixture, RequireDeadPolicyRefusesPreviousIncarnationWithUnknownLiveness)
{
    ON_CALL(require_dead_authority_, GetTakeoverPolicy()).WillByDefault(Return(TakeoverPolicy::kRequireDead));
    ON_CALL(require_dead_authority_, Query(_)).WillByDefault(Return(Liveness::kUnknown));
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(100), require_dead_authority_).result, TakeoverResult::kFresh);

    EXPECT_EQ(unit_.Takeover(kAppId, Pid(101), require_dead_authority_).result, TakeoverResult::kRefusedLive);
}

TEST_F(TakeoverFixture, RequireDeadPolicyTakesOverFromDeadPreviousIncarnation)
{
    ON_CALL(require_dead_authority_, GetTakeoverPolicy()).WillByDefault(Return(TakeoverPolicy::kRequireDead));
    ON_CALL(require_dead_authority_, Query(Pid(100))).WillByDefault(Return(Liveness::kDead));
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(100), require_dead_authority_).result, TakeoverResult::kFresh);

    const auto outcome = unit_.Takeover(kAppId, Pid(101), require_dead_authority_);

    EXPECT_EQ(outcome.result, TakeoverResult::kTookOver);
    EXPECT_EQ(outcome.previous, Pid(100));
}

TEST_F(TakeoverFixture, ReleaseIfOwnerReleasesOnlyForExpectedOwnerAndNextTakeoverIsFresh)
{
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(100), linux_authority_).result, TakeoverResult::kFresh);

    // a restarted proxy took over in the meantime: the stale owner cannot release
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(101), linux_authority_).result, TakeoverResult::kTookOver);
    EXPECT_FALSE(unit_.ReleaseIfOwner(kAppId, Pid(100)));
    EXPECT_FALSE(unit_.ReleaseIfOwner(kAppId + 1U, Pid(101)));
    EXPECT_FALSE(unit_.ReleaseIfOwner(kAppId, IncarnationId::None()));

    EXPECT_TRUE(unit_.ReleaseIfOwner(kAppId, Pid(101)));
    EXPECT_FALSE(unit_.ReleaseIfOwner(kAppId, Pid(101)));

    const auto outcome = unit_.Takeover(kAppId, Pid(102), linux_authority_);
    EXPECT_EQ(outcome.result, TakeoverResult::kFresh);
    EXPECT_FALSE(outcome.previous.IsValid());
}

TEST_F(TakeoverFixture, ReleasedEntryIsReusedByTakeoverRequiringDeadWithoutQueryingLiveness)
{
    ON_CALL(require_dead_authority_, GetTakeoverPolicy()).WillByDefault(Return(TakeoverPolicy::kRequireDead));
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(100), require_dead_authority_).result, TakeoverResult::kFresh);
    ASSERT_TRUE(unit_.ReleaseIfOwner(kAppId, Pid(100)));

    EXPECT_CALL(require_dead_authority_, Query(_)).Times(0);
    EXPECT_EQ(unit_.Takeover(kAppId, Pid(101), require_dead_authority_).result, TakeoverResult::kFresh);
}

TEST_F(TakeoverFixture, ReportsContendedWhenMappingIsFull)
{
    ApplicationIdPidMapping<Allocator> full{1, Allocator()};
    ASSERT_EQ(full.Takeover(1U, Pid(10), linux_authority_).result, TakeoverResult::kFresh);

    EXPECT_EQ(full.Takeover(2U, Pid(11), linux_authority_).result, TakeoverResult::kContended);
}

TEST_F(TakeoverFixture, EntryLeftInUpdatingStateIsTakenOverAsFresh)
{
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> entries(1);
    entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUpdating, kAppId);
    entries.at(0).pid_ = 100;

    const auto outcome = detail::Takeover(entries.begin(), entries.end(), kAppId, Pid(200), linux_authority_);

    EXPECT_EQ(outcome.result, TakeoverResult::kFresh);
    EXPECT_EQ(entries.at(0).GetStatusAndApplicationIdAtomic().first, ApplicationIdPidMappingEntry::MappingEntryStatus::kUsed);
    EXPECT_EQ(entries.at(0).pid_.load(), 200);
}

TEST_F(TakeoverFixture, LosingTheOwnerCasRevaluatesAgainstTheWinnerUnderRequireDead)
{
    ON_CALL(require_dead_authority_, GetTakeoverPolicy()).WillByDefault(Return(TakeoverPolicy::kRequireDead));
    ON_CALL(require_dead_authority_, Query(Pid(100))).WillByDefault(Return(Liveness::kDead));
    ON_CALL(require_dead_authority_, Query(Pid(300))).WillByDefault(Return(Liveness::kAlive));
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> entries(1);
    entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUsed, kAppId);
    entries.at(0).pid_ = 100;

    // Given a competing taker (pid 300) wins the CAS between our load of the dead owner 100 and our CAS
    concurrency::AtomicMock<pid_t> atomic_mock{};
    concurrency::AtomicIndirectorMock<pid_t>::SetMockObject(&atomic_mock);
    EXPECT_CALL(atomic_mock, compare_exchange_strong(_, _, _))
        .WillOnce(testing::DoAll(testing::SetArgReferee<0>(300), Return(false)));

    const auto outcome =
        detail::Takeover<concurrency::AtomicIndirectorMock>(entries.begin(), entries.end(), kAppId, Pid(200), require_dead_authority_);

    // expect that we do not take over from the live winner
    EXPECT_EQ(outcome.result, TakeoverResult::kRefusedLive);
    EXPECT_EQ(outcome.previous, Pid(300));
    concurrency::AtomicIndirectorMock<pid_t>::SetMockObject(nullptr);
}

TEST_F(TakeoverFixture, LosingTheOwnerCasRetriesAgainstTheWinnerUnderAlwaysTakeover)
{
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> entries(1);
    entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUsed, kAppId);
    entries.at(0).pid_ = 100;
    concurrency::AtomicMock<pid_t> atomic_mock{};
    concurrency::AtomicIndirectorMock<pid_t>::SetMockObject(&atomic_mock);
    EXPECT_CALL(atomic_mock, compare_exchange_strong(_, _, _))
        .WillOnce(testing::DoAll(testing::SetArgReferee<0>(300), Return(false)))
        .WillOnce(Return(true));

    const auto outcome =
        detail::Takeover<concurrency::AtomicIndirectorMock>(entries.begin(), entries.end(), kAppId, Pid(200), linux_authority_);

    EXPECT_EQ(outcome.result, TakeoverResult::kTookOver);
    EXPECT_EQ(outcome.previous, Pid(300));
    concurrency::AtomicIndirectorMock<pid_t>::SetMockObject(nullptr);
}

TEST_F(TakeoverFixture, ReportsContendedWhenOwnerCasKeepsFailing)
{
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> entries(1);
    entries.at(0).SetStatusAndApplicationIdAtomic(ApplicationIdPidMappingEntry::MappingEntryStatus::kUsed, kAppId);
    entries.at(0).pid_ = 100;
    concurrency::AtomicMock<pid_t> atomic_mock{};
    concurrency::AtomicIndirectorMock<pid_t>::SetMockObject(&atomic_mock);
    EXPECT_CALL(atomic_mock, compare_exchange_strong(_, _, _)).WillRepeatedly(Return(false));

    EXPECT_EQ(detail::Takeover<concurrency::AtomicIndirectorMock>(
                  entries.begin(), entries.end(), kAppId, Pid(200), linux_authority_)
                  .result,
              TakeoverResult::kContended);
    concurrency::AtomicIndirectorMock<pid_t>::SetMockObject(nullptr);
}

TEST_F(TakeoverFixture, ConcurrentTakeoversHandBackEveryPreviousIncarnationExactlyOnce)
{
    constexpr int kThreads{8};
    constexpr int kTakeoversPerThread{2000};
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(1), linux_authority_).result, TakeoverResult::kFresh);

    std::vector<std::vector<IncarnationId>> previous(kThreads);
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index)
    {
        threads.emplace_back([this, &previous, thread_index]() {
            for (int i = 0; i < kTakeoversPerThread; ++i)
            {
                const auto outcome =
                    unit_.Takeover(kAppId, Pid(100 + thread_index * kTakeoversPerThread + i), linux_authority_);
                EXPECT_EQ(outcome.result, TakeoverResult::kTookOver);
                previous[static_cast<std::size_t>(thread_index)].push_back(outcome.previous);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    std::set<std::uint64_t> returned{};
    for (const auto& per_thread : previous)
    {
        for (const auto& id : per_thread)
        {
            EXPECT_TRUE(returned.insert(id.Raw()).second) << "previous incarnation reported twice";
        }
    }
    EXPECT_EQ(returned.size(), static_cast<std::size_t>(kThreads * kTakeoversPerThread));
    EXPECT_EQ(returned.count(1U), 1U);
}

TEST_F(TakeoverFixture, ConcurrentTakeoverAndReleaseNeverReleaseANewerOwner)
{
    constexpr int kRounds{20000};
    ASSERT_EQ(unit_.Takeover(kAppId, Pid(1), linux_authority_).result, TakeoverResult::kFresh);
    std::atomic<int> successful_releases_of_stale_owner{0};

    // A "provider side" releaser keeps releasing the owner it observed (pid 1) while a restarted proxy takes over.
    std::thread releaser([&]() {
        for (int i = 0; i < kRounds; ++i)
        {
            if (unit_.ReleaseIfOwner(kAppId, Pid(1)) && (unit_.Takeover(kAppId, Pid(2), linux_authority_).result ==
                                                         TakeoverResult::kSame))
            {
                ++successful_releases_of_stale_owner;
            }
        }
    });
    std::thread taker([&]() {
        for (int i = 0; i < kRounds; ++i)
        {
            std::ignore = unit_.Takeover(kAppId, Pid(2), linux_authority_);
        }
    });
    releaser.join();
    taker.join();

    // Once pid 2 owns the entry, a release for pid 1 must fail, and the final owner must be pid 2 (or released/None).
    EXPECT_FALSE(unit_.ReleaseIfOwner(kAppId, Pid(1)));
    const auto last = unit_.Takeover(kAppId, Pid(3), linux_authority_);
    EXPECT_TRUE(last.previous == Pid(2) || last.result == TakeoverResult::kFresh);
}

TEST(DetailRegisterPidEmpty, NoNullPointerDereferenceInCaseOfEmptyMappingEntriesArray)
{
    constexpr auto number_of_elements{0U};
    score::containers::DynamicArray<ApplicationIdPidMappingEntry> empty{number_of_elements};
    constexpr std::uint16_t application_id{0U};
    constexpr std::uint16_t pid{1U};

    EXPECT_EQ(empty.size(), 0);
    auto result = detail::RegisterPid(empty.begin(), empty.end(), application_id, pid);

    EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace score::mw::com::impl::lola
