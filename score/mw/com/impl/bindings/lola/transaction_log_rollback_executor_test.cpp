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
#include "score/message_passing/identity_types.h"
#include "score/mw/com/impl/bindings/lola/transaction_log_rollback_executor.h"

#include "score/mw/com/impl/bindings/lola/application_id_pid_mapping.h"
#include "score/mw/com/impl/bindings/lola/messaging/message_passing_service_mock.h"
#include "score/mw/com/impl/bindings/lola/register_pid_fake.h"
#include "score/mw/com/impl/bindings/lola/rollback_synchronization.h"
#include "score/mw/com/impl/bindings/lola/runtime_mock.h"
#include "score/mw/com/impl/bindings/lola/service_data_control.h"
#include "score/mw/com/impl/bindings/lola/skeleton_event_properties.h"
#include "score/mw/com/impl/bindings/lola/skeleton_instance_identifier.h"
#include "score/mw/com/impl/bindings/lola/test/transaction_log_test_resources.h"
#include "score/mw/com/impl/runtime.h"
#include "score/mw/com/impl/runtime_mock.h"
#include "score/mw/com/impl/test/runtime_mock_guard.h"

#include "score/memory/shared/shared_memory_resource_heap_allocator_mock.h"

#include <gtest/gtest.h>
#include <score/jthread.hpp>
#include <sys/types.h>
#include <memory>
#include <optional>
#include <vector>

namespace score::mw::com::impl::lola
{
namespace
{

using ::testing::_;
using ::testing::Return;
using ::testing::ReturnRef;

constexpr std::uint64_t kMemoryResourceId{10U};
const TransactionLogId kDummyTransactionLogId{20U};
const pid_t kDummyProviderPid{33U};
const pid_t kDummyOldConsumerPid{17U};
const pid_t kDummyCurrentConsumerPid{155U};
const QualityType kDummyQualityType{QualityType::kASIL_QM};
const ElementFqId kDummyElementFqId{1U, 2U, 3U, ServiceElementType::EVENT};
const SkeletonInstanceIdentifier kSkeletonInstanceIdentifier{kDummyElementFqId.service_id_,
                                                             kDummyElementFqId.instance_id_};

constexpr std::size_t kNumberOfSlots{20U};
constexpr std::size_t kMaxSubscribers{20U};
// The test registers a single event (see WithTransactionLogRollbackExecutor), hence a capacity of one is sufficient
// for the fixed-capacity event_controls_ container within ServiceDataControl.
constexpr std::size_t kNumberOfServiceElements{1U};
const SkeletonEventProperties kDummySkeletonEventProperties{kNumberOfSlots, 0U, 0U, false, kMaxSubscribers, true};

class TransactionLogRollbackExecutorFixture : public ::testing::Test
{
  protected:
    TransactionLogRollbackExecutorFixture() noexcept
    {
        ON_CALL(runtime_mock_guard_.runtime_mock_, GetBindingRuntime(BindingType::kLoLa))
            .WillByDefault(Return(&lola_runtime_mock_));
        ON_CALL(lola_runtime_mock_, GetPid()).WillByDefault(Return(kDummyCurrentConsumerPid));
        ON_CALL(lola_runtime_mock_, GetLolaMessaging).WillByDefault(ReturnRef(message_passing_service_mock_));
        ON_CALL(lola_runtime_mock_, GetRollbackSynchronization()).WillByDefault(ReturnRef(rollback_synchronization_));
    }

    TransactionLogRollbackExecutorFixture& WithTransactionLogRollbackExecutor()
    {
        service_data_control_ = std::make_unique<ServiceDataControl>(kNumberOfServiceElements, memory_resource_mock_);
        AddEvent(kDummyElementFqId, kDummySkeletonEventProperties);

        unit_ = std::make_unique<TransactionLogRollbackExecutor>(*service_data_control_,
                                                                 kSkeletonInstanceIdentifier,
                                                                 kDummyQualityType,
                                                                 kDummyProviderPid,
                                                                 kDummyTransactionLogId);

        return *this;
    }

    void AddEvent(const ElementFqId element_fq_id, const SkeletonEventProperties skeleton_event_properties) noexcept
    {
        const auto emplace_result = service_data_control_->event_controls_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(element_fq_id),
            std::forward_as_tuple(skeleton_event_properties.GetTotalNumberOfSlots(),
                                  skeleton_event_properties.max_subscribers,
                                  skeleton_event_properties.enforce_max_samples,
                                  memory_resource_mock_));
        ASSERT_TRUE(emplace_result.second);
    }

    void AddApplicationIdPidMapping(std::uint32_t application_id, pid_t pid) noexcept
    {
        const auto result_pid = service_data_control_->application_id_pid_mapping_.RegisterPid(application_id, pid);
        ASSERT_TRUE(result_pid.has_value());
        ASSERT_EQ(result_pid.value(), pid);
    }

    ConsumerEventDataControlLocalView<> GetConsumerEventDataControlLocalView(const ElementFqId element_fq_id) noexcept
    {
        auto find_result = service_data_control_->event_controls_.find(element_fq_id);
        EXPECT_NE(find_result, service_data_control_->event_controls_.cend());
        return {find_result->second.data_control};
    }

    TransactionLogSet& GetTransactionLogSet(const ElementFqId element_fq_id) noexcept
    {
        auto find_result = service_data_control_->event_controls_.find(element_fq_id);
        EXPECT_NE(find_result, service_data_control_->event_controls_.cend());
        return find_result->second.transaction_log_set_;
    }

    void InsertServiceDataControl() noexcept
    {
        auto rollback_mutex = rollback_synchronization_.GetMutex(kSkeletonInstanceIdentifier);
        // we expect that the mutex did not yet exist, but has been created by our call.
        ASSERT_FALSE(rollback_mutex.second);
    }

    TransactionLogSet::TransactionLogNode& RegisterProxyElementWithTransactionLogSet(
        const ElementFqId& element_fq_id,
        const TransactionLogId& transaction_log_id) noexcept
    {
        auto consumer_event_data_control_local = GetConsumerEventDataControlLocalView(element_fq_id);
        auto& transaction_log_set = GetTransactionLogSet(element_fq_id);
        transaction_log_registration_guards_.push_back(
            transaction_log_set.RegisterProxyElement(transaction_log_id, consumer_event_data_control_local).value());
        const auto transaction_log_index = transaction_log_registration_guards_.back().GetTransactionLogIndex();

        auto& transaction_logs = TransactionLogSetAttorney{transaction_log_set}.GetProxyTransactionLogs();
        auto& transaction_log_node = transaction_logs.at(transaction_log_index);

        EXPECT_TRUE(transaction_log_node.IsActive());
        EXPECT_FALSE(transaction_log_node.NeedsRollback());

        return transaction_log_node;
    }

    RuntimeMockGuard runtime_mock_guard_{};
    lola::RuntimeMock lola_runtime_mock_{};
    memory::shared::SharedMemoryResourceHeapAllocatorMock memory_resource_mock_{kMemoryResourceId};
    MessagePassingServiceMock message_passing_service_mock_{};

    // In this test fixture, we want to simulate that we are opening the shared memory region of a crashed process
    // (including the TransactionLogSet). We can then test the rollback funtionality of the
    // TransactionLogRollbackExecutor. However, we therefore have to create TransactionLogRegistrationGuards within this
    // fixture to simulate the logs in the crashed process. Therefore, we disable the destruction operation of these
    // TransactionLogRegistrationGuards to prevent an additional rollback being done on the destruction of this fixture.
    TransactionLogRegistrationGuardDeactiveDestructionOperationGuard guard{};

    std::unique_ptr<ServiceDataControl> service_data_control_{nullptr};
    std::unique_ptr<TransactionLogRollbackExecutor> unit_{nullptr};
    RollbackSynchronization rollback_synchronization_{};
    std::vector<TransactionLogRegistrationGuard> transaction_log_registration_guards_{};
};

using TransactionLogRegisterProxyElementFixture = TransactionLogRollbackExecutorFixture;
TEST_F(TransactionLogRegisterProxyElementFixture, RegisteringProxyElementLeadsToActiveLogNode)
{
    WithTransactionLogRollbackExecutor();
    RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
}

using TransactionLogRollbackExecutorRollbackLogsFixture = TransactionLogRollbackExecutorFixture;
TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, WillDoNothingWhenServiceDataControlAlreadyInMap)
{
    // Given our current service instance (which the TransactionLogRollbackExecutor unit under test deals with) is
    // already registered within the global RollbackSynchronization
    WithTransactionLogRollbackExecutor();
    InsertServiceDataControl();

    // and given our unit based on a transaction log set with one active log node
    auto& transaction_log_node = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);

    // when calling RollbackTransactionLogs()
    auto result = unit_->RollbackTransactionLogs();
    // expect, that it succeeded
    EXPECT_TRUE(result.has_value());
    // and expect, that the log node is still there and active (nothing has changed)
    EXPECT_TRUE(transaction_log_node.IsActive());
    EXPECT_FALSE(transaction_log_node.NeedsRollback());
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture,
       WillCreateRollbackSynchronizationEntryWhenRollbackSynchronizationEntryDoesNotAlreadyExist)
{
    WithTransactionLogRollbackExecutor();

    // when RollbackTransactionLogs() has been called
    std::ignore = unit_->RollbackTransactionLogs();

    // expect, that the synchronization mutex for service_data_control_ exists afterward
    auto [mutex, mutex_existed] = rollback_synchronization_.GetMutex(kSkeletonInstanceIdentifier);
    EXPECT_EQ(mutex_existed, true);
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, WillNotifyProviderOutdatedPidWhenPidAlreadyRegistered)
{
    WithTransactionLogRollbackExecutor();

    // given, we already have an existing application_id/pid mapping registered for our application_id
    AddApplicationIdPidMapping(static_cast<std::uint32_t>(kDummyTransactionLogId), kDummyOldConsumerPid);

    // expect, that the provider will get notified about the old/previous pid being outdated
    EXPECT_CALL(message_passing_service_mock_,
                NotifyOutdatedNodeId(kDummyQualityType, message_passing::NodeIdFromPid(kDummyOldConsumerPid), message_passing::NodeIdFromPid(kDummyProviderPid)));

    // when RollbackTransactionLogs() is called.
    auto result = unit_->RollbackTransactionLogs();
    EXPECT_TRUE(result.has_value());
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, WillNotNotifyProviderOutdatedPidWhenPidNotAlreadyRegistered)
{
    WithTransactionLogRollbackExecutor();

    // given, we don't have yet an existing application_id/pid mapping registered for our application_id

    // expect, that NO provider notification happens about an old/previous pid being outdated
    EXPECT_CALL(message_passing_service_mock_, NotifyOutdatedNodeId(_, _, _)).Times(0);

    // when RollbackTransactionLogs() is called.
    auto result = unit_->RollbackTransactionLogs();
    EXPECT_TRUE(result.has_value());
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, WillNotNotifyProviderWhenSamePidIsAlreadyRegistered)
{
    WithTransactionLogRollbackExecutor();

    // given the mapping already names the current pid (e.g. a second proxy process-incarnation with a reused pid)
    AddApplicationIdPidMapping(static_cast<std::uint32_t>(kDummyTransactionLogId), kDummyCurrentConsumerPid);

    // expect that no outdated notification is sent
    EXPECT_CALL(message_passing_service_mock_, NotifyOutdatedNodeId(_, _, _)).Times(0);

    EXPECT_TRUE(unit_->RollbackTransactionLogs().has_value());
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, TakesOverMappingOfPreviousPidAndMarksLogsNeedingRollback)
{
    WithTransactionLogRollbackExecutor();
    AddApplicationIdPidMapping(static_cast<std::uint32_t>(kDummyTransactionLogId), kDummyOldConsumerPid);
    auto& transaction_log_node = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    TransactionLogLocalView{transaction_log_node.GetTransactionLog()}.SubscribeTransactionBegin(kNumberOfSlots);
    EXPECT_CALL(message_passing_service_mock_, NotifyOutdatedNodeId(_, _, _)).Times(1);

    // when the rollback is prepared (the log cannot be rolled back, as the subscribe was not finished)
    EXPECT_FALSE(unit_->RollbackTransactionLogs().has_value());

    // expect that the log was flagged for rollback and the mapping now names the current pid (last writer wins)
    EXPECT_TRUE(transaction_log_node.NeedsRollback());
    EXPECT_EQ(service_data_control_->application_id_pid_mapping_
                  .RegisterPid(static_cast<std::uint32_t>(kDummyTransactionLogId), 9999)
                  .value(),
              kDummyCurrentConsumerPid);
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, OnlyLogsOfOwnApplicationIdAreMarkedAndRolledBack)
{
    WithTransactionLogRollbackExecutor();
    const TransactionLogId other_transaction_log_id{21U};
    auto& own_node = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    auto& other_node = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, other_transaction_log_id);

    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());

    EXPECT_FALSE(own_node.IsActive());
    EXPECT_TRUE(other_node.IsActive());
    EXPECT_FALSE(other_node.NeedsRollback());
}

// Re-run behaviour of RollbackProxyTransactions after the executing process stopped part-way through a rollback.
// The tests simulate the stop by applying the first steps of a rollback to the log by hand (exactly the sequence
// TransactionLogLocalView::RollbackIncrementTransactions / RollbackSubscribeTransactions performs) and then run the
// real rollback on the remaining state.
class TransactionLogRollbackRerunFixture : public TransactionLogRollbackExecutorFixture
{
  protected:
    static constexpr std::size_t kSubscriptionMaxSampleCount{kNumberOfSlots};

    TransactionLogSet::TransactionLogNode& SetupLogWithReferencedSlotsAndSubscription(
        const std::vector<TransactionLog::SlotIndexType>& referenced_slots) noexcept
    {
        WithTransactionLogRollbackExecutor();
        auto& node = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
        TransactionLogLocalView view{node.GetTransactionLog()};
        view.SubscribeTransactionBegin(kSubscriptionMaxSampleCount);
        view.SubscribeTransactionCommit();
        for (const auto slot : referenced_slots)
        {
            view.ReferenceTransactionBegin(slot);
            view.ReferenceTransactionCommit(slot);
        }
        GetTransactionLogSet(kDummyElementFqId).MarkTransactionLogsNeedRollback(kDummyTransactionLogId);
        EXPECT_TRUE(node.NeedsRollback());
        return node;
    }

    Result<void> RunRollback() noexcept
    {
        return GetTransactionLogSet(kDummyElementFqId)
            .RollbackProxyTransactions(
                kDummyTransactionLogId,
                [this](const TransactionLog::SlotIndexType slot_index) noexcept {
                    dereferenced_slots_.push_back(slot_index);
                },
                [this](const TransactionLog::MaxSampleCountType max_sample_count) noexcept {
                    unsubscribed_with_.push_back(max_sample_count);
                });
    }

    std::vector<TransactionLog::SlotIndexType> dereferenced_slots_{};
    std::vector<TransactionLog::MaxSampleCountType> unsubscribed_with_{};
};

TEST_F(TransactionLogRollbackRerunFixture, UninterruptedRollbackDereferencesEverySlotOnceAndUnsubscribes)
{
    auto& node = SetupLogWithReferencedSlotsAndSubscription({0U, 3U, 4U});

    ASSERT_TRUE(RunRollback().has_value());

    EXPECT_EQ(dereferenced_slots_, (std::vector<TransactionLog::SlotIndexType>{0U, 3U, 4U}));
    EXPECT_EQ(unsubscribed_with_.size(), 1U);
    EXPECT_FALSE(node.IsActive());
}

TEST_F(TransactionLogRollbackRerunFixture, RerunAfterStopBetweenSlotsFinishesRemainingSlotsExactlyOnce)
{
    auto& node = SetupLogWithReferencedSlotsAndSubscription({0U, 1U, 2U, 3U});
    TransactionLogLocalView view{node.GetTransactionLog()};

    // Given a first rollback which stopped after it completely dereferenced slots 0 and 1 (callback already invoked
    // for them in the stopped run, so they are not counted here)
    for (const TransactionLog::SlotIndexType slot : std::vector<TransactionLog::SlotIndexType>{0U, 1U})
    {
        view.DereferenceTransactionBegin(slot);
        view.DereferenceTransactionCommit(slot);
    }

    // when the rollback is re-run
    ASSERT_TRUE(RunRollback().has_value());

    // then only the remaining slots are dereferenced (exactly once), the subscription is undone and the log released
    EXPECT_EQ(dereferenced_slots_, (std::vector<TransactionLog::SlotIndexType>{2U, 3U}));
    EXPECT_EQ(unsubscribed_with_.size(), 1U);
    EXPECT_FALSE(node.IsActive());
    EXPECT_FALSE(node.NeedsRollback());
}

TEST_F(TransactionLogRollbackRerunFixture, RerunAfterStopBeforeUnsubscribeOnlyUnsubscribes)
{
    auto& node = SetupLogWithReferencedSlotsAndSubscription({1U});
    TransactionLogLocalView view{node.GetTransactionLog()};
    view.DereferenceTransactionBegin(1U);
    view.DereferenceTransactionCommit(1U);

    ASSERT_TRUE(RunRollback().has_value());

    EXPECT_TRUE(dereferenced_slots_.empty());
    EXPECT_EQ(unsubscribed_with_, (std::vector<TransactionLog::MaxSampleCountType>{kSubscriptionMaxSampleCount}));
    EXPECT_FALSE(node.IsActive());
}

TEST_F(TransactionLogRollbackRerunFixture, RerunAfterStopInsideSlotDereferenceFailsWithoutDereferencingAgain)
{
    auto& node = SetupLogWithReferencedSlotsAndSubscription({0U, 1U, 2U});
    TransactionLogLocalView view{node.GetTransactionLog()};

    // Given a first rollback which completed slot 0 and stopped inside the dereference of slot 1, i.e. after
    // DereferenceTransactionBegin but before DereferenceTransactionCommit. Whether the refcount decrement of slot 1
    // already happened is not recorded.
    view.DereferenceTransactionBegin(0U);
    view.DereferenceTransactionCommit(0U);
    view.DereferenceTransactionBegin(1U);

    // when the rollback is re-run
    const auto result = RunRollback();

    // then it fails (fail-safe: no blind second decrement of slot 1) ...
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kCouldNotRestartProxy);
    EXPECT_TRUE(dereferenced_slots_.empty());
    EXPECT_TRUE(unsubscribed_with_.empty());
    // ... and the log stays active and flagged, i.e. it is permanently stuck.
    EXPECT_TRUE(node.IsActive());
    EXPECT_TRUE(node.NeedsRollback());

    // and a further re-run yields the same result (idempotent failure, no side effects)
    EXPECT_FALSE(RunRollback().has_value());
    EXPECT_TRUE(dereferenced_slots_.empty());
}

TEST_F(TransactionLogRollbackRerunFixture, RerunAfterStopInsideUnsubscribeFailsWithoutUnsubscribingAgain)
{
    auto& node = SetupLogWithReferencedSlotsAndSubscription({});
    TransactionLogLocalView view{node.GetTransactionLog()};
    view.UnsubscribeTransactionBegin();

    const auto result = RunRollback();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ComErrc::kCouldNotRestartProxy);
    EXPECT_TRUE(unsubscribed_with_.empty());
    EXPECT_TRUE(node.IsActive());
    EXPECT_TRUE(node.NeedsRollback());
}

TEST_F(TransactionLogRollbackRerunFixture, RerunAfterCompletedRollbackIsNoOp)
{
    SetupLogWithReferencedSlotsAndSubscription({2U});
    ASSERT_TRUE(RunRollback().has_value());
    dereferenced_slots_.clear();
    unsubscribed_with_.clear();

    EXPECT_TRUE(RunRollback().has_value());

    EXPECT_TRUE(dereferenced_slots_.empty());
    EXPECT_TRUE(unsubscribed_with_.empty());
}

TEST_F(TransactionLogRollbackRerunFixture, RestartedProxyTakesOverFromCrashedRollbackAndRollsBackRemainingLog)
{
    // Given two logs of the same application id (two proxy elements) and a first proxy incarnation, which prepared the
    // rollback (took over the mapping, flagged the logs) and rolled back one log before stopping
    WithTransactionLogRollbackExecutor();
    AddApplicationIdPidMapping(static_cast<std::uint32_t>(kDummyTransactionLogId), kDummyOldConsumerPid);
    auto& node_0 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    auto& node_1 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    EXPECT_CALL(message_passing_service_mock_,
                NotifyOutdatedNodeId(kDummyQualityType, kDummyOldConsumerPid, kDummyProviderPid))
        .Times(1);
    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());
    ASSERT_FALSE(node_0.IsActive());
    ASSERT_TRUE(node_1.IsActive());
    ASSERT_TRUE(node_1.NeedsRollback());

    // when a second incarnation (new pid, new process-local RollbackSynchronization) runs the rollback
    constexpr pid_t kNextConsumerPid{156};
    RollbackSynchronization restarted_rollback_synchronization{};
    ON_CALL(lola_runtime_mock_, GetPid()).WillByDefault(Return(kNextConsumerPid));
    ON_CALL(lola_runtime_mock_, GetRollbackSynchronization())
        .WillByDefault(ReturnRef(restarted_rollback_synchronization));
    EXPECT_CALL(message_passing_service_mock_,
                NotifyOutdatedNodeId(kDummyQualityType, kDummyCurrentConsumerPid, kDummyProviderPid))
        .Times(1);
    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());

    // then it reported the crashed incarnation as outdated and completed the remaining rollback
    EXPECT_FALSE(node_1.IsActive());
    EXPECT_FALSE(node_1.NeedsRollback());
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, RollbackTransactionLogsCanBeCalledMultiThreaded)
{
    WithTransactionLogRollbackExecutor();

    // We do the registration in a pre-step so that the threads are created as close as possible to each other to
    // increase the chance of thread contention.
    std::vector<std::reference_wrapper<const TransactionLogSet::TransactionLogNode>> transaction_log_nodes{};
    for (std::size_t i = 0; i < kMaxSubscribers; ++i)
    {
        const auto& transaction_log_node =
            RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
        transaction_log_nodes.emplace_back(transaction_log_node);
    }

    std::vector<score::cpp::jthread> threads{};
    for (std::size_t i = 0; i < kMaxSubscribers; ++i)
    {
        threads.emplace_back([this]() noexcept {
            std::ignore = unit_->RollbackTransactionLogs();
        });
    }

    for (std::size_t i = 0; i < kMaxSubscribers; ++i)
    {
        auto& thread = threads.at(i);
        if (thread.joinable())
        {
            thread.join();
        }

        auto transaction_log_node = transaction_log_nodes.at(i);
        EXPECT_FALSE(transaction_log_node.get().IsActive());
        EXPECT_FALSE(transaction_log_node.get().NeedsRollback());
    }
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, CallingRollbackTransactionLogsWillRollBackOneLogPerCall)
{
    WithTransactionLogRollbackExecutor();

    auto& transaction_log_node_0 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    auto& transaction_log_node_1 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);

    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());

    EXPECT_FALSE(transaction_log_node_0.IsActive());
    EXPECT_FALSE(transaction_log_node_0.NeedsRollback());
    EXPECT_TRUE(transaction_log_node_1.IsActive());
    EXPECT_TRUE(transaction_log_node_1.NeedsRollback());

    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());

    EXPECT_FALSE(transaction_log_node_1.IsActive());
    EXPECT_FALSE(transaction_log_node_1.NeedsRollback());

    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());
}

/// \brief test case checks, that rollback of transaction logs for a given service instance is only done
///        once per lifecycle.
/// \details The 1st call to RollbackTransactionLogs() of an service instance specific TransactionLogRollbackExecutor
///          will rollback any existing transaction-logs for the given service instance and note down in the runtime,
///          that rollback for this service instance has ben done. Any further call to RollbackTransactionLogs() for the
///          same TransactionLogRollbackExecutor instance (service instance) has no effect, even if AFTER the 1st call
///          to RollbackTransactionLogs() new transaction-logs are created/added related to the same service instance,
///          they get ignored by a later call to RollbackTransactionLogs().
///
TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, WillOnlyRollBackLogsInFirstCall)
{
    // Given a unit under test for a given service instance and given proxy application (identified by
    // kDummyTransactionLogId)
    WithTransactionLogRollbackExecutor();

    // when we register a proxy-element
    auto& transaction_log_node_0 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);

    // expect, that a rollback of transaction logs is successful
    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());
    // and that the transaction log for the previously registered proxy element is NOT active/does NOT need a rollback
    EXPECT_FALSE(transaction_log_node_0.IsActive());
    EXPECT_FALSE(transaction_log_node_0.NeedsRollback());

    // when we register the same proxy element a 2nd time (simulating a 2nd local proxy for the same service instance)
    auto& transaction_log_node_1 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    // expect, that call to RollbackTransactionLogs() succeeds
    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());
    // and expect, that it did nothing -> i.e. the transaction log of the 2nd registered proxy element is still active
    EXPECT_TRUE(transaction_log_node_1.IsActive());
    // and it does NOT need rollback.
    EXPECT_FALSE(transaction_log_node_1.NeedsRollback());
}

TEST_F(TransactionLogRollbackExecutorRollbackLogsFixture, WillReturnErrorIfAnyLogsFailToRollBack)
{
    WithTransactionLogRollbackExecutor();

    auto& transaction_log_node_0 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);
    auto& transaction_log_node_1 = RegisterProxyElementWithTransactionLogSet(kDummyElementFqId, kDummyTransactionLogId);

    TransactionLogLocalView transaction_log_local_view{transaction_log_node_1.GetTransactionLog()};
    transaction_log_local_view.SubscribeTransactionBegin(0U);

    ASSERT_TRUE(unit_->RollbackTransactionLogs().has_value());

    EXPECT_FALSE(transaction_log_node_0.IsActive());
    EXPECT_FALSE(transaction_log_node_0.NeedsRollback());
    EXPECT_TRUE(transaction_log_node_1.IsActive());
    EXPECT_TRUE(transaction_log_node_1.NeedsRollback());

    ASSERT_FALSE(unit_->RollbackTransactionLogs().has_value());

    EXPECT_TRUE(transaction_log_node_1.IsActive());
    EXPECT_TRUE(transaction_log_node_1.NeedsRollback());
}

using TransactionLogRollbackExecutorMarkNeedRollbackDeathTest = TransactionLogRollbackExecutorFixture;
TEST_F(TransactionLogRollbackExecutorMarkNeedRollbackDeathTest, FailingToGetLolaRuntimeTerminates)
{
    WithTransactionLogRollbackExecutor();

    ON_CALL(runtime_mock_guard_.runtime_mock_, GetBindingRuntime(BindingType::kLoLa)).WillByDefault(Return(nullptr));
    EXPECT_DEATH(std::ignore = unit_->RollbackTransactionLogs(), ".*");
}

TEST_F(TransactionLogRollbackExecutorMarkNeedRollbackDeathTest, FailingToRegisterPidTerminates)
{
    WithTransactionLogRollbackExecutor();

    // Given that RegisterPid will return an empty result when called
    RegisterPidFake register_pid_fake{};
    const std::optional<pid_t> empty_register_pid_result{};
    register_pid_fake.InjectRegisterPidResult(empty_register_pid_result);
    ApplicationIdPidMapping<score::memory::shared::PolymorphicOffsetPtrAllocator<ApplicationIdPidMappingEntry>>::
        InjectRegisterPidFake(register_pid_fake);

    // When calling RollbackTransactionLogs
    // Then the program terminates
    EXPECT_DEATH(std::ignore = unit_->RollbackTransactionLogs(), ".*");

    ApplicationIdPidMapping<
        score::memory::shared::PolymorphicOffsetPtrAllocator<ApplicationIdPidMappingEntry>>::ClearRegisterPidFake();
}

}  // namespace
}  // namespace score::mw::com::impl::lola
