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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_INCARNATION_HEADER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_INCARNATION_HEADER_H

#include <atomic>
#include <cstdint>

namespace score::mw::com::impl::lola::fuchsia
{

constexpr std::uint64_t kEpochUnpublished = 0U;
/// Written by the broker into the control region when the incarnation is fenced.
constexpr std::uint64_t kEpochTombstone = UINT64_MAX;
constexpr std::uint64_t kIncarnationMagic = 0x4c4f4c4149434e31U;  // "LOLAICN1"

/// \brief First object of the data region (read-only for proxies) and of the control region. The skeleton publishes the
///        epoch it got from the broker with release semantics as its *last* write; the broker later overwrites only
///        the control copy with kEpochTombstone. A proxy that read epoch e at attach treats any other value as fenced.
struct IncarnationHeader
{
    std::atomic<std::uint64_t> epoch;
    std::uint64_t magic;
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "the header word is shared between processes");

inline void PublishIncarnation(IncarnationHeader& header, const std::uint64_t epoch) noexcept
{
    header.magic = kIncarnationMagic;
    header.epoch.store(epoch, std::memory_order_release);
}

/// \brief True while the region still belongs to the live incarnation `epoch`.
inline bool IsIncarnationLive(const IncarnationHeader& header, const std::uint64_t epoch) noexcept
{
    return (header.epoch.load(std::memory_order_acquire) == epoch) && (header.magic == kIncarnationMagic);
}

inline bool IsIncarnationFenced(const IncarnationHeader& header) noexcept
{
    return header.epoch.load(std::memory_order_acquire) == kEpochTombstone;
}

inline void FenceIncarnation(IncarnationHeader& header) noexcept
{
    header.epoch.store(kEpochTombstone, std::memory_order_release);
}

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_INCARNATION_HEADER_H
