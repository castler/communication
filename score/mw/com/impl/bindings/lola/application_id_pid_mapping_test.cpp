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

#include "score/concurrency/atomic_indirector.h"
#include "score/concurrency/atomic_mock.h"

#include <gtest/gtest.h>

#include <thread>

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
    EXPECT_EQ(mapping_entries.at(0).pid_, 100);
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
