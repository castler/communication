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

#include "score/mw/com/impl/service_element_type.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{
namespace
{

const ElementFqId kEvent{0x10U, 1U, 2U, ServiceElementType::EVENT};
const ElementFqId kOtherInstanceEvent{0x10U, 1U, 3U, ServiceElementType::EVENT};
constexpr IMessagePassingServiceInstance::NodeId kNode{};

struct InnerCalls
{
    int notify_event{0};
    int register_event{0};
    int reregister_event{0};
    int unregister_event{0};
    int outdated{0};
    int existence_registered{0};
    std::optional<IMessagePassingService::HandlerStatusChangeCallback> status_callback;
};

class FakeInner final : public IMessagePassingServiceInstance
{
  public:
    explicit FakeInner(InnerCalls& calls) : calls_{calls} {}

    void NotifyEvent(const ElementFqId) noexcept override { ++calls_.notify_event; }
    IMessagePassingService::HandlerRegistrationNoType RegisterEventNotification(
        const ElementFqId,
        std::weak_ptr<ScopedEventReceiveHandler>,
        const NodeId) noexcept override
    {
        ++calls_.register_event;
        return static_cast<IMessagePassingService::HandlerRegistrationNoType>(calls_.register_event);
    }
    void ReregisterEventNotification(const ElementFqId, const NodeId) noexcept override { ++calls_.reregister_event; }
    void UnregisterEventNotification(const ElementFqId,
                                     const IMessagePassingService::HandlerRegistrationNoType,
                                     const NodeId) noexcept override
    {
        ++calls_.unregister_event;
    }
    Result<void> RegisterOnServiceMethodSubscribedHandler(const SkeletonInstanceIdentifier,
                                                          IMessagePassingService::ServiceMethodSubscribedHandler,
                                                          IMessagePassingService::AllowedConsumerUids) override
    {
        return {};
    }
    Result<void> RegisterOnServiceMethodUnsubscribedHandler(
        const SkeletonInstanceIdentifier,
        IMessagePassingService::ServiceMethodUnsubscribedHandler) override
    {
        return {};
    }
    Result<void> RegisterMethodCallHandler(const ProxyMethodInstanceIdentifier,
                                           IMessagePassingService::MethodCallHandler,
                                           const PrincipalId) override
    {
        return {};
    }
    void UnregisterOnServiceMethodSubscribedHandler(SkeletonInstanceIdentifier) override {}
    void UnregisterOnServiceMethodUnsubscribedHandler(SkeletonInstanceIdentifier) override {}
    void UnregisterMethodCallHandler(ProxyMethodInstanceIdentifier) override {}
    void NotifyOutdatedNodeId(const NodeId, const NodeId) noexcept override { ++calls_.outdated; }
    void RegisterEventNotificationExistenceChangedCallback(
        const ElementFqId,
        IMessagePassingService::HandlerStatusChangeCallback callback) noexcept override
    {
        ++calls_.existence_registered;
        calls_.status_callback = std::move(callback);
    }
    void UnregisterEventNotificationExistenceChangedCallback(const ElementFqId) noexcept override {}
    Result<void> SubscribeServiceMethod(const SkeletonInstanceIdentifier&,
                                        const ProxyInstanceIdentifier&,
                                        const NodeId) override
    {
        return {};
    }
    Result<void> UnsubscribeServiceMethod(const SkeletonInstanceIdentifier&,
                                          const ProxyInstanceIdentifier&,
                                          const NodeId) override
    {
        return {};
    }
    Result<void> CallMethod(const ProxyMethodInstanceIdentifier&, const std::size_t, const NodeId) override
    {
        return {};
    }

  private:
    InnerCalls& calls_;
};

class FakeNotifier final : public IEventNotifier
{
  public:
    std::size_t NotifySubscribers() noexcept override
    {
        ++notified;
        return 1U;
    }
    bool HasProvider(const ElementFqId event_id) const noexcept override
    {
        return event_id.instance_id_ == kEvent.instance_id_;
    }
    std::optional<HandlerRegistrationNo> Register(const ElementFqId,
                                                  std::weak_ptr<ScopedEventReceiveHandler> handler) noexcept override
    {
        handlers.push_back(std::move(handler));
        return kRegistrationBit | static_cast<HandlerRegistrationNo>(handlers.size());
    }
    void Unregister(const ElementFqId, const HandlerRegistrationNo number) noexcept override
    {
        unregistered.push_back(number);
    }
    void Reregister(const ElementFqId) noexcept override { ++reregistered; }

    int notified{0};
    int reregistered{0};
    std::vector<std::weak_ptr<ScopedEventReceiveHandler>> handlers;
    std::vector<HandlerRegistrationNo> unregistered;
};

class EventNotifyingServiceInstanceTest : public ::testing::Test
{
  protected:
    EventNotifyingServiceInstance MakeUnit(IEventNotifier* notifier)
    {
        return EventNotifyingServiceInstance{std::make_unique<FakeInner>(calls_), [notifier]() { return notifier; }};
    }

    std::shared_ptr<ScopedEventReceiveHandler> MakeHandler()
    {
        return std::make_shared<ScopedEventReceiveHandler>(scope_, []() noexcept {});
    }

    safecpp::Scope<> scope_;
    InnerCalls calls_;
    FakeNotifier notifier_;
};

TEST_F(EventNotifyingServiceInstanceTest, NotifyEventReachesInnerAndSubscribers)
{
    auto unit = MakeUnit(&notifier_);
    unit.NotifyEvent(kEvent);
    EXPECT_EQ(calls_.notify_event, 1);
    EXPECT_EQ(notifier_.notified, 1);
}

TEST_F(EventNotifyingServiceInstanceTest, TransparentWithoutNotifier)
{
    auto unit = MakeUnit(nullptr);
    unit.NotifyEvent(kEvent);
    auto handler = MakeHandler();
    const auto number = unit.RegisterEventNotification(kEvent, handler, kNode);
    EXPECT_EQ(number, 1U);
    unit.ReregisterEventNotification(kEvent, kNode);
    unit.UnregisterEventNotification(kEvent, number, kNode);
    EXPECT_EQ(calls_.notify_event, 1);
    EXPECT_EQ(calls_.register_event, 1);
    EXPECT_EQ(calls_.reregister_event, 1);
    EXPECT_EQ(calls_.unregister_event, 1);
}

TEST_F(EventNotifyingServiceInstanceTest, RegistrationOfAttachedProviderGoesToNotifierOnly)
{
    auto unit = MakeUnit(&notifier_);
    auto handler = MakeHandler();
    const auto number = unit.RegisterEventNotification(kEvent, handler, kNode);
    EXPECT_NE(number & IEventNotifier::kRegistrationBit, 0U);
    EXPECT_EQ(calls_.register_event, 0);
    ASSERT_EQ(notifier_.handlers.size(), 1U);
    EXPECT_EQ(notifier_.handlers[0].lock(), handler);

    unit.ReregisterEventNotification(kEvent, kNode);
    EXPECT_EQ(notifier_.reregistered, 1);
    EXPECT_EQ(calls_.reregister_event, 0);

    unit.UnregisterEventNotification(kEvent, number, kNode);
    ASSERT_EQ(notifier_.unregistered.size(), 1U);
    EXPECT_EQ(notifier_.unregistered[0], number);
    EXPECT_EQ(calls_.unregister_event, 0);
}

TEST_F(EventNotifyingServiceInstanceTest, RegistrationOfUnknownProviderStaysWithInner)
{
    auto unit = MakeUnit(&notifier_);
    auto handler = MakeHandler();
    const auto number = unit.RegisterEventNotification(kOtherInstanceEvent, handler, kNode);
    EXPECT_EQ(number & IEventNotifier::kRegistrationBit, 0U);
    EXPECT_EQ(calls_.register_event, 1);
    EXPECT_TRUE(notifier_.handlers.empty());
    unit.ReregisterEventNotification(kOtherInstanceEvent, kNode);
    EXPECT_EQ(calls_.reregister_event, 1);
    unit.UnregisterEventNotification(kOtherInstanceEvent, number, kNode);
    EXPECT_EQ(calls_.unregister_event, 1);
}

TEST_F(EventNotifyingServiceInstanceTest, NotificationIsOnFromTheStartAndStaysOn)
{
    auto unit = MakeUnit(&notifier_);
    std::vector<bool> seen;
    unit.RegisterEventNotificationExistenceChangedCallback(
        kEvent, [&seen](const bool has_handlers) noexcept { seen.push_back(has_handlers); });
    ASSERT_EQ(calls_.existence_registered, 1);
    EXPECT_EQ(seen, std::vector<bool>{true});
    ASSERT_TRUE(calls_.status_callback.has_value());
    (*calls_.status_callback)(false);
    EXPECT_EQ(seen, std::vector<bool>{true});
    (*calls_.status_callback)(true);
    EXPECT_EQ(seen, (std::vector<bool>{true, true}));
}

TEST_F(EventNotifyingServiceInstanceTest, ExistenceCallbackIsUntouchedWithoutNotifier)
{
    auto unit = MakeUnit(nullptr);
    std::vector<bool> seen;
    unit.RegisterEventNotificationExistenceChangedCallback(
        kEvent, [&seen](const bool has_handlers) noexcept { seen.push_back(has_handlers); });
    EXPECT_TRUE(seen.empty());
    (*calls_.status_callback)(true);
    (*calls_.status_callback)(false);
    EXPECT_EQ(seen, (std::vector<bool>{true, false}));
}

TEST_F(EventNotifyingServiceInstanceTest, OutdatedNodeIdIsPassedOn)
{
    auto unit = MakeUnit(&notifier_);
    unit.NotifyOutdatedNodeId(kNode, kNode);
    EXPECT_EQ(calls_.outdated, 1);
}

}  // namespace
}  // namespace score::mw::com::impl::lola::fuchsia
