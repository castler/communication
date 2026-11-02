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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_INTEGRITY_POLICY_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_INTEGRITY_POLICY_H

#include <cstdint>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Integrity level of an element, ordered (kQm < kAsilB). Mirrors score.mw.com.Quality; kept OS-neutral so the
///        policy can be unit tested on the host.
enum class Integrity : std::uint8_t
{
    kQm = 1U,
    kAsilB = 2U,
};

/// \brief Profile 1 of the slot refcount (design decision record y07): the control region is consumer-writable (CAS on
///        the slot refcount), so a consumer may only receive it if it is developed to at least the integrity of the
///        region, i.e. every writer of the region is trusted to that level (no freedom from interference inside it).
constexpr bool MayReceiveWritableControl(const Integrity consumer, const Integrity region) noexcept
{
    return static_cast<std::uint8_t>(consumer) >= static_cast<std::uint8_t>(region);
}

/// \brief Integrity of a service instance: the level of the highest control region it publishes.
constexpr Integrity ServiceIntegrity(const bool asil_b_control_published) noexcept
{
    return asil_b_control_published ? Integrity::kAsilB : Integrity::kQm;
}

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_INTEGRITY_POLICY_H
