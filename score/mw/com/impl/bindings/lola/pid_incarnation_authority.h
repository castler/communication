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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_PID_INCARNATION_AUTHORITY_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_PID_INCARNATION_AUTHORITY_H

#include "score/mw/com/impl/bindings/lola/i_incarnation_authority.h"

#include <sys/types.h>

namespace score::mw::com::impl::lola
{

/// \brief Linux/QNX incarnation authority: the pid is the incarnation, a new incarnation always takes over and
/// there is no provider side rollback (the restarted consumer rolls back its own logs).
class PidIncarnationAuthority final : public IIncarnationAuthority
{
  public:
    explicit PidIncarnationAuthority(const pid_t pid) noexcept;

    IncarnationId Self() const noexcept override;
    Liveness Query(const IncarnationId incarnation) const noexcept override;
    TakeoverPolicy GetTakeoverPolicy() const noexcept override;
    bool SupportsProviderSideRollback() const noexcept override;

  private:
    IncarnationId self_;
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_PID_INCARNATION_AUTHORITY_H
