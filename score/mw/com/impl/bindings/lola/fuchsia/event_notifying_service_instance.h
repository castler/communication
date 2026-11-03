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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_EVENT_NOTIFYING_SERVICE_INSTANCE_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_EVENT_NOTIFYING_SERVICE_INSTANCE_H

#include "score/mw/com/impl/bindings/lola/fuchsia/i_event_notifier.h"
#include "score/mw/com/impl/bindings/lola/messaging/i_message_passing_service_instance.h"
#include "score/mw/com/impl/bindings/lola/messaging/i_message_passing_service_instance_factory.h"

#include <functional>
#include <memory>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Decorates a message passing service instance: the event notification subset (NotifyEvent,
///        Register/Reregister/UnregisterEventNotification, existence-changed callbacks) goes through the Fuchsia
///        notification eventpair, everything else (methods, same-process notification, outdated node ids) is passed
///        on unchanged.
/// \details Skeleton: NotifyEvent notifies the same-process handlers through the decorated instance and signals the
///          subscribers' eventpairs. Proxy: a registration for an event of an attached provider is served by the
///          notifier (no message to the skeleton); any other registration goes to the decorated instance.
///          Without a notifier (IEventNotifier::Current() == nullptr) the decorator is transparent.
class EventNotifyingServiceInstance final : public IMessagePassingServiceInstance
{
  public:
    using NotifierSource = std::function<IEventNotifier*()>;

    EventNotifyingServiceInstance(std::unique_ptr<IMessagePassingServiceInstance> inner,
                                  NotifierSource notifier_source = &IEventNotifier::Current);

    void NotifyEvent(const ElementFqId event_id) noexcept override;

    IMessagePassingService::HandlerRegistrationNoType RegisterEventNotification(
        const ElementFqId event_id,
        std::weak_ptr<ScopedEventReceiveHandler> callback,
        const NodeId target_node_id) noexcept override;

    void ReregisterEventNotification(const ElementFqId event_id, const NodeId target_node_id) noexcept override;

    void UnregisterEventNotification(const ElementFqId event_id,
                                     const IMessagePassingService::HandlerRegistrationNoType registration_no,
                                     const NodeId target_node_id) noexcept override;

    Result<void> RegisterOnServiceMethodSubscribedHandler(
        const SkeletonInstanceIdentifier skeleton_instance_identifier,
        IMessagePassingService::ServiceMethodSubscribedHandler subscribed_callback,
        IMessagePassingService::AllowedConsumerUids allowed_proxy_uids) override;

    Result<void> RegisterOnServiceMethodUnsubscribedHandler(
        const SkeletonInstanceIdentifier skeleton_instance_identifier,
        IMessagePassingService::ServiceMethodUnsubscribedHandler unsubscribed_callback) override;

    Result<void> RegisterMethodCallHandler(const ProxyMethodInstanceIdentifier proxy_method_instance_identifier,
                                           IMessagePassingService::MethodCallHandler method_call_callback,
                                           const PrincipalId allowed_proxy_uid) override;

    void UnregisterOnServiceMethodSubscribedHandler(SkeletonInstanceIdentifier skeleton_instance_identifier) override;

    void UnregisterOnServiceMethodUnsubscribedHandler(SkeletonInstanceIdentifier skeleton_instance_identifier) override;

    void UnregisterMethodCallHandler(ProxyMethodInstanceIdentifier proxy_method_instance_identifier) override;

    void NotifyOutdatedNodeId(const NodeId outdated_node_id, const NodeId target_node_id) noexcept override;

    void RegisterEventNotificationExistenceChangedCallback(
        const ElementFqId event_id,
        IMessagePassingService::HandlerStatusChangeCallback callback) noexcept override;

    void UnregisterEventNotificationExistenceChangedCallback(const ElementFqId event_id) noexcept override;

    Result<void> SubscribeServiceMethod(const SkeletonInstanceIdentifier& skeleton_instance_identifier,
                                        const ProxyInstanceIdentifier& proxy_instance_identifier,
                                        const NodeId target_node_id) override;

    Result<void> UnsubscribeServiceMethod(const SkeletonInstanceIdentifier& skeleton_instance_identifier,
                                          const ProxyInstanceIdentifier& proxy_instance_identifier,
                                          const NodeId target_node_id) override;

    Result<void> CallMethod(const ProxyMethodInstanceIdentifier& proxy_method_instance_identifier,
                            const std::size_t queue_position,
                            const NodeId target_node_id) override;

  private:
    std::unique_ptr<IMessagePassingServiceInstance> inner_;
    NotifierSource notifier_source_;
};

/// \brief Wraps every instance the decorated factory creates in an EventNotifyingServiceInstance.
class EventNotifyingServiceInstanceFactory final : public IMessagePassingServiceInstanceFactory
{
  public:
    explicit EventNotifyingServiceInstanceFactory(std::unique_ptr<IMessagePassingServiceInstanceFactory> inner);

    [[nodiscard]] std::unique_ptr<IMessagePassingServiceInstance> Create(
        ClientQualityType client_quality_type,
        AsilSpecificCfg config,
        score::message_passing::IServerFactory& server_factory,
        score::message_passing::IClientFactory& client_factory,
        score::concurrency::Executor& executor) const noexcept override;

  private:
    std::unique_ptr<IMessagePassingServiceInstanceFactory> inner_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_EVENT_NOTIFYING_SERVICE_INSTANCE_H
