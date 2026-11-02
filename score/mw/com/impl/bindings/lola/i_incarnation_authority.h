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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_I_INCARNATION_AUTHORITY_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_I_INCARNATION_AUTHORITY_H

#include "score/mw/com/impl/bindings/lola/incarnation.h"

#include <cstdint>

namespace score::mw::com::impl::lola
{

/// \brief Liveness of an incarnation as known by the platform.
enum class Liveness : std::uint8_t
{
    kDead,
    kAlive,
    /// The platform cannot tell (e.g. Linux, where a pid carries no liveness information).
    kUnknown,
};

/// \brief Whether a new incarnation may take over the state of a previous incarnation of the same application.
enum class TakeoverPolicy : std::uint8_t
{
    /// A new incarnation takes over unconditionally (Linux/QNX: a restarted process has the same application id).
    kAlwaysTakeover,
    /// A new incarnation takes over only after the previous incarnation has been confirmed dead.
    kRequireDead,
};

/// \brief Platform specific source of incarnation identities and their liveness.
class IIncarnationAuthority
{
  public:
    IIncarnationAuthority() noexcept = default;
    virtual ~IIncarnationAuthority() noexcept = default;

    /// \brief Incarnation of the calling process.
    virtual IncarnationId Self() const noexcept = 0;

    /// \brief Liveness of the given incarnation.
    virtual Liveness Query(const IncarnationId incarnation) const noexcept = 0;

    virtual TakeoverPolicy GetTakeoverPolicy() const noexcept = 0;

    /// \brief Whether the provider side rolls back transaction logs of dead consumers on its own.
    virtual bool SupportsProviderSideRollback() const noexcept = 0;

  protected:
    IIncarnationAuthority(IIncarnationAuthority&&) noexcept = default;
    IIncarnationAuthority& operator=(IIncarnationAuthority&&) noexcept = default;
    IIncarnationAuthority(const IIncarnationAuthority&) noexcept = default;
    IIncarnationAuthority& operator=(const IIncarnationAuthority&) noexcept = default;
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_I_INCARNATION_AUTHORITY_H
