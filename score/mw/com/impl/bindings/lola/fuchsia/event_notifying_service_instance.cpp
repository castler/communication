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
#include "score/mw/com/impl/bindings/lola/fuchsia/event_notifying_service_instance.h"

#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

EventNotifyingServiceInstance::EventNotifyingServiceInstance(std::unique_ptr<IMessagePassingServiceInstance> inner,
                                                             NotifierSource notifier_source)
    : IMessagePassingServiceInstance{}, inner_{std::move(inner)}, notifier_source_{std::move(notifier_source)}
{
}

void EventNotifyingServiceInstance::NotifyEvent(const ElementFqId event_id) noexcept
{
    // Same-process handlers (and, where it exists, the message passing path) first, exactly as without the decorator.
    inner_->NotifyEvent(event_id);
    if (auto* const notifier = notifier_source_(); notifier != nullptr)
    {
        // A signal coalesces: the subscriber re-reads the sample queue, so one wake-up covers several publications.
        static_cast<void>(notifier->NotifySubscribers());
    }
}

IMessagePassingService::HandlerRegistrationNoType EventNotifyingServiceInstance::RegisterEventNotification(
    const ElementFqId event_id,
    std::weak_ptr<ScopedEventReceiveHandler> callback,
    const NodeId target_node_id) noexcept
{
    if (auto* const notifier = notifier_source_(); (notifier != nullptr) && notifier->HasProvider(event_id))
    {
        const auto registration_no = notifier->Register(event_id, callback);
        if (registration_no.has_value())
        {
            return *registration_no;
        }
    }
    return inner_->RegisterEventNotification(event_id, std::move(callback), target_node_id);
}

void EventNotifyingServiceInstance::ReregisterEventNotification(const ElementFqId event_id,
                                                                const NodeId target_node_id) noexcept
{
    if (auto* const notifier = notifier_source_(); (notifier != nullptr) && notifier->HasProvider(event_id))
    {
        notifier->Reregister(event_id);
        return;
    }
    inner_->ReregisterEventNotification(event_id, target_node_id);
}

void EventNotifyingServiceInstance::UnregisterEventNotification(
    const ElementFqId event_id,
    const IMessagePassingService::HandlerRegistrationNoType registration_no,
    const NodeId target_node_id) noexcept
{
    if ((registration_no & IEventNotifier::kRegistrationBit) != 0U)
    {
        if (auto* const notifier = notifier_source_(); notifier != nullptr)
        {
            notifier->Unregister(event_id, registration_no);
        }
        return;
    }
    inner_->UnregisterEventNotification(event_id, registration_no, target_node_id);
}

Result<void> EventNotifyingServiceInstance::RegisterOnServiceMethodSubscribedHandler(
    const SkeletonInstanceIdentifier skeleton_instance_identifier,
    IMessagePassingService::ServiceMethodSubscribedHandler subscribed_callback,
    IMessagePassingService::AllowedConsumerUids allowed_proxy_uids)
{
    return inner_->RegisterOnServiceMethodSubscribedHandler(
        skeleton_instance_identifier, std::move(subscribed_callback), std::move(allowed_proxy_uids));
}

Result<void> EventNotifyingServiceInstance::RegisterOnServiceMethodUnsubscribedHandler(
    const SkeletonInstanceIdentifier skeleton_instance_identifier,
    IMessagePassingService::ServiceMethodUnsubscribedHandler unsubscribed_callback)
{
    return inner_->RegisterOnServiceMethodUnsubscribedHandler(skeleton_instance_identifier,
                                                              std::move(unsubscribed_callback));
}

Result<void> EventNotifyingServiceInstance::RegisterMethodCallHandler(
    const ProxyMethodInstanceIdentifier proxy_method_instance_identifier,
    IMessagePassingService::MethodCallHandler method_call_callback,
    const PrincipalId allowed_proxy_uid)
{
    return inner_->RegisterMethodCallHandler(
        proxy_method_instance_identifier, std::move(method_call_callback), allowed_proxy_uid);
}

void EventNotifyingServiceInstance::UnregisterOnServiceMethodSubscribedHandler(
    SkeletonInstanceIdentifier skeleton_instance_identifier)
{
    inner_->UnregisterOnServiceMethodSubscribedHandler(skeleton_instance_identifier);
}

void EventNotifyingServiceInstance::UnregisterOnServiceMethodUnsubscribedHandler(
    SkeletonInstanceIdentifier skeleton_instance_identifier)
{
    inner_->UnregisterOnServiceMethodUnsubscribedHandler(skeleton_instance_identifier);
}

void EventNotifyingServiceInstance::UnregisterMethodCallHandler(
    ProxyMethodInstanceIdentifier proxy_method_instance_identifier)
{
    inner_->UnregisterMethodCallHandler(proxy_method_instance_identifier);
}

void EventNotifyingServiceInstance::NotifyOutdatedNodeId(const NodeId outdated_node_id,
                                                         const NodeId target_node_id) noexcept
{
    inner_->NotifyOutdatedNodeId(outdated_node_id, target_node_id);
}

void EventNotifyingServiceInstance::RegisterEventNotificationExistenceChangedCallback(
    const ElementFqId event_id,
    IMessagePassingService::HandlerStatusChangeCallback callback) noexcept
{
    // The skeleton is never told when a proxy registers a handler (the eventpair is created by Attach), so as long as a
    // notifier exists the notification is on from the start and a "no handlers" report of the decorated instance must
    // not switch it off again. Signalling without a subscriber costs one lock and an empty loop.
    auto shared = std::make_shared<IMessagePassingService::HandlerStatusChangeCallback>(std::move(callback));
    inner_->RegisterEventNotificationExistenceChangedCallback(
        event_id,
        IMessagePassingService::HandlerStatusChangeCallback{[this, shared](const bool has_handlers) noexcept {
            if (has_handlers || (notifier_source_() == nullptr))
            {
                (*shared)(has_handlers);
            }
        }});
    if (notifier_source_() != nullptr)
    {
        (*shared)(true);
    }
}

void EventNotifyingServiceInstance::UnregisterEventNotificationExistenceChangedCallback(
    const ElementFqId event_id) noexcept
{
    inner_->UnregisterEventNotificationExistenceChangedCallback(event_id);
}

Result<void> EventNotifyingServiceInstance::SubscribeServiceMethod(
    const SkeletonInstanceIdentifier& skeleton_instance_identifier,
    const ProxyInstanceIdentifier& proxy_instance_identifier,
    const NodeId target_node_id)
{
    return inner_->SubscribeServiceMethod(skeleton_instance_identifier, proxy_instance_identifier, target_node_id);
}

Result<void> EventNotifyingServiceInstance::UnsubscribeServiceMethod(
    const SkeletonInstanceIdentifier& skeleton_instance_identifier,
    const ProxyInstanceIdentifier& proxy_instance_identifier,
    const NodeId target_node_id)
{
    return inner_->UnsubscribeServiceMethod(skeleton_instance_identifier, proxy_instance_identifier, target_node_id);
}

Result<void> EventNotifyingServiceInstance::CallMethod(
    const ProxyMethodInstanceIdentifier& proxy_method_instance_identifier,
    const std::size_t queue_position,
    const NodeId target_node_id)
{
    return inner_->CallMethod(proxy_method_instance_identifier, queue_position, target_node_id);
}

EventNotifyingServiceInstanceFactory::EventNotifyingServiceInstanceFactory(
    std::unique_ptr<IMessagePassingServiceInstanceFactory> inner)
    : IMessagePassingServiceInstanceFactory{}, inner_{std::move(inner)}
{
}

std::unique_ptr<IMessagePassingServiceInstance> EventNotifyingServiceInstanceFactory::Create(
    ClientQualityType client_quality_type,
    AsilSpecificCfg config,
    score::message_passing::IServerFactory& server_factory,
    score::message_passing::IClientFactory& client_factory,
    score::concurrency::Executor& executor) const noexcept
{
    return std::make_unique<EventNotifyingServiceInstance>(
        inner_->Create(client_quality_type, std::move(config), server_factory, client_factory, executor));
}

}  // namespace score::mw::com::impl::lola::fuchsia
