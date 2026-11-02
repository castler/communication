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

#include "score/mw/com/impl/binding_type.h"
#include "score/mw/com/impl/bindings/lola/consumer_event_data_control_local_view.h"
#include "score/mw/com/impl/bindings/lola/i_runtime.h"
#include "score/mw/com/impl/bindings/lola/transaction_log_set.h"
#include "score/mw/com/impl/runtime.h"

#include "score/mw/log/logging.h"

#include <score/assert.hpp>
#include <exception>

namespace score::mw::com::impl::lola
{

namespace
{

// Suppress "AUTOSAR C++14 A15-5-3" rule findings.
// This rule states: "The std::terminate() function shall not be called implicitly".
// The coverity tool reports: "fun_call_w_exception: Called function throws an exception of type
// std::bad_optional_access." and points on statement `for (auto& element : service_data_control.event_controls_)`.
// This is a false positive; no optional is involved in this statement.
// coverity[autosar_cpp14_a15_5_3_violation : FALSE]
void MarkTransactionLogsNeedRollback(ServiceDataControl& service_data_control,
                                     const TransactionLogId transaction_log_id) noexcept
{
    for (auto& element : service_data_control.event_controls_)
    {
        auto& transaction_log_set = element.second.transaction_log_set_;
        transaction_log_set.MarkTransactionLogsNeedRollback(transaction_log_id);
    }
}

}  // namespace

TransactionLogRollbackExecutor::TransactionLogRollbackExecutor(
    ServiceDataControl& service_data_control,
    const SkeletonInstanceIdentifier skeleton_instance_identifier,
    const QualityType asil_level,
    const message_passing::NodeId provider_node,
    const TransactionLogId transaction_log_id,
    const std::optional<RollbackAmbiguityPolicy> ambiguity_policy) noexcept
    : service_data_control_{service_data_control},
      asil_level_{asil_level},
      provider_node_{provider_node},
      transaction_log_id_{transaction_log_id},
      skeleton_instance_identifier_{skeleton_instance_identifier},
      ambiguity_policy_{ambiguity_policy}
{
}

// Suppress "AUTOSAR C++14 A15-5-3" rule findings. This rule states: "The std::terminate() function shall not be called
// implicitly". This is a false positive, all results which are accessed with '.value()' that could implicitly call
// 'std::terminate()' (in case it doesn't have value) has a check in advance using '.has_value()', so no way for
// throwing std::bad_optional_access which leds to std::terminate(). This suppression should be removed after fixing
// [Ticket-173043](broken_link_j/Ticket-173043)
// coverity[autosar_cpp14_a15_5_3_violation : FALSE]
Result<void> TransactionLogRollbackExecutor::PrepareRollback(lola::IRuntime& lola_runtime) noexcept
{
    // Make the current incarnation the owner of the application's unique identifier (which is the transaction_log_id_
    // for this context) in the shared mapping.
    const auto current_incarnation = lola_runtime.GetIncarnation();
    const auto outcome = service_data_control_.application_id_pid_mapping_.Takeover(
        transaction_log_id_, current_incarnation, lola_runtime.GetIncarnationAuthority());

    switch (outcome.result)
    {
        case TakeoverResult::kContended:
            score::mw::log::LogFatal("lola")
                << "Couldn't register current incarnation for application id within shared memory. This can occurr "
                   "if there is too high contention accessing the registry. Terminating.";
            std::terminate();
        case TakeoverResult::kRefusedLive:
            score::mw::log::LogError("lola") << "Previous incarnation" << outcome.previous.Raw()
                                             << "of this application id is not known to be dead. Refusing takeover.";
            return MakeUnexpected(ComErrc::kBindingFailure,
                                  "Rolling back transaction log failed: previous incarnation is still alive.");
        case TakeoverResult::kTookOver:
            if (outcome.previous.IsValid() && (outcome.previous != current_incarnation))
            {
                // we found an old/outdated incarnation for our application id in the shared-memory of the
                // service-instance. Notify provider, that this incarnation is outdated.
                lola_runtime.GetLolaMessaging().NotifyOutdatedNodeId(
                    asil_level_, outcome.previous.ToNodeId(), provider_node_);
            }
            break;
        case TakeoverResult::kFresh:
        case TakeoverResult::kSame:
        default:
            break;
    }

    // Mark all TransactionLogs for each event that correspond to transaction_log_id as needing to be rolled back.
    MarkTransactionLogsNeedRollback(service_data_control_, transaction_log_id_);
    return {};
}

// Suppress "AUTOSAR C++14 A15-5-3" rule findings. This rule states: "The std::terminate() function shall not be called
// implicitly". This is a false positive, no way for implicit calling std::terminate().
// coverity[autosar_cpp14_a15_5_3_violation : FALSE]
Result<void> TransactionLogRollbackExecutor::RollbackTransactionLogs() noexcept
{
    auto& lola_runtime = GetBindingRuntime<lola::IRuntime>(BindingType::kLoLa);
    auto& rollback_synchronization = lola_runtime.GetRollbackSynchronization();

    auto [lock, mutex_existed] = rollback_synchronization.GetMutex(skeleton_instance_identifier_);
    // Suppress Autosar C++14 A8-5-3 states that auto variables shall not be initialized using braced initialization.
    // This is a false positive, we don't use auto here
    // coverity[autosar_cpp14_a8_5_3_violation : FALSE]
    std::lock_guard map_lock{lock};

    // If another Proxy instance has already prepared the rollback for the given service_data_control_ (the special case
    // where we have more than one proxy instance in the same process, which uses the same service instance ->
    // service_data_control_), then we must not prepare rollback a 2nd time! If the mutex was not created (but an
    // existing one returned), this is the sign, that another proxy already did the PrepareRollback for the same
    // service_data_control_
    if (!mutex_existed)
    {
        const auto prepare_result = PrepareRollback(lola_runtime);
        if (!prepare_result.has_value())
        {
            return prepare_result;
        }
    }

    // Where a takeover requires the previous incarnation to be confirmed dead, ambiguous logs of that incarnation can
    // be resolved safely; otherwise (Linux/QNX) they keep the log stuck as before.
    const auto effective_policy =
        ambiguity_policy_.has_value()
            ? ambiguity_policy_.value()
            : ((lola_runtime.GetIncarnationAuthority().GetTakeoverPolicy() == TakeoverPolicy::kRequireDead)
                   ? RollbackAmbiguityPolicy::kQuarantine
                   : RollbackAmbiguityPolicy::kFail);

    for (auto& element : service_data_control_.event_controls_)
    {
        auto& event_control = element.second;
        ConsumerEventDataControlLocalView<> consumer_event_data_control_view{event_control.data_control};
        auto& transaction_log_set = event_control.transaction_log_set_;
        const auto rollback_result = transaction_log_set.RollbackProxyTransactions(
            transaction_log_id_,
            [&consumer_event_data_control_view](const TransactionLog::SlotIndexType slot_index) noexcept {
                consumer_event_data_control_view.DereferenceEventWithoutTransactionLogging(slot_index);
            },
            [&event_control](const TransactionLog::MaxSampleCountType subscription_max_sample_count) noexcept {
                event_control.subscription_control.Unsubscribe(subscription_max_sample_count);
            },
            effective_policy);
        if (!rollback_result.has_value())
        {
            return rollback_result;
        }
    }
    return {};
}

}  // namespace score::mw::com::impl::lola
