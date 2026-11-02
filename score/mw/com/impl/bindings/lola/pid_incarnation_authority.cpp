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
#include "score/mw/com/impl/bindings/lola/pid_incarnation_authority.h"

namespace score::mw::com::impl::lola
{

PidIncarnationAuthority::PidIncarnationAuthority(const pid_t pid) noexcept : self_{IncarnationId::FromPid(pid)} {}

IncarnationId PidIncarnationAuthority::Self() const noexcept
{
    return self_;
}

Liveness PidIncarnationAuthority::Query(const IncarnationId) const noexcept
{
    return Liveness::kUnknown;
}

TakeoverPolicy PidIncarnationAuthority::GetTakeoverPolicy() const noexcept
{
    return TakeoverPolicy::kAlwaysTakeover;
}

bool PidIncarnationAuthority::SupportsProviderSideRollback() const noexcept
{
    return false;
}

}  // namespace score::mw::com::impl::lola
