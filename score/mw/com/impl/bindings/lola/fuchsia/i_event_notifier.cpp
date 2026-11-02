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
#include "score/mw/com/impl/bindings/lola/fuchsia/i_event_notifier.h"

#include <atomic>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{
std::atomic<IEventNotifier*> current_notifier{nullptr};
}  // namespace

auto IEventNotifier::Current() noexcept -> IEventNotifier*
{
    return current_notifier.load();
}

auto IEventNotifier::SetCurrent(IEventNotifier* notifier) noexcept -> void
{
    current_notifier.store(notifier);
}

}  // namespace score::mw::com::impl::lola::fuchsia
