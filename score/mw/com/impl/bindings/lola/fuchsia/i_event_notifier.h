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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_I_EVENT_NOTIFIER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_I_EVENT_NOTIFIER_H

#include "score/mw/com/impl/bindings/lola/element_fq_id.h"
#include "score/mw/com/impl/scoped_event_receive_handler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Event update notification of LoLa on Fuchsia: the notification eventpair that Attach hands to every
///        subscriber replaces the NotifyEventUpdate/RegisterEventNotification messages of the message passing service.
/// \details The signal is per service instance (one eventpair per Attach), not per event, and it coalesces. A wake-up
///        therefore means "something was published": every receive handler registered for the instance is invoked and
///        re-reads its sample queue (GetNewSamples), so a coalesced signal cannot lose an event, at worst a handler
///        finds nothing new.
class IEventNotifier
{
  public:
    using HandlerRegistrationNo = std::uint32_t;

    /// Registration numbers handed out by a notifier have this bit set, so that the decorating message passing
    /// service can tell them from the numbers of the instance it decorates.
    static constexpr HandlerRegistrationNo kRegistrationBit{0x80000000U};

    IEventNotifier() noexcept = default;
    virtual ~IEventNotifier() noexcept = default;
    IEventNotifier(const IEventNotifier&) = delete;
    IEventNotifier& operator=(const IEventNotifier&) = delete;
    IEventNotifier(IEventNotifier&&) = delete;
    IEventNotifier& operator=(IEventNotifier&&) = delete;

    /// \brief Skeleton side: wakes every subscriber. Never blocks. Returns the number of subscribers signalled.
    virtual std::size_t NotifySubscribers() noexcept = 0;

    /// \brief Proxy side: true if the service instance of `event_id` is attached (alive or lost) in this process.
    virtual bool HasProvider(const ElementFqId event_id) const noexcept = 0;

    /// \brief Registers a receive handler for `event_id`. nullopt if the provider is not attached.
    virtual std::optional<HandlerRegistrationNo> Register(const ElementFqId event_id,
                                                          std::weak_ptr<ScopedEventReceiveHandler> handler) noexcept = 0;

    virtual void Unregister(const ElementFqId event_id, const HandlerRegistrationNo registration_no) noexcept = 0;

    /// \brief The provider was restarted: attaches again if the old notification is dead, the registrations stay.
    virtual void Reregister(const ElementFqId event_id) noexcept = 0;

    /// \brief The notifier of this process (installed by VmoShmRuntime), nullptr if there is none.
    static IEventNotifier* Current() noexcept;
    static void SetCurrent(IEventNotifier* notifier) noexcept;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_I_EVENT_NOTIFIER_H
