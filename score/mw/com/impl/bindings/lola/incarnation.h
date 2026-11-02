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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_INCARNATION_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_INCARNATION_H

#include "score/message_passing/node_id.h"

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>

namespace score::mw::com::impl::lola
{

/// \brief Identifies one incarnation of one process-role (a proxy process or a skeleton process).
/// \details Opaque, trivially copyable, 8 bytes, hence safe to be stored in shared memory. The value 0 is "none".
/// An IncarnationId must not be reused while any shared memory that stored it can still be read. No ordering
/// semantics are provided.
class IncarnationId
{
  public:
    using RawType = std::uint64_t;

    constexpr IncarnationId() noexcept = default;

    static constexpr IncarnationId None() noexcept
    {
        return IncarnationId{};
    }

    /// \brief Linux/QNX: the pid of the process is its incarnation.
    static constexpr IncarnationId FromPid(const pid_t pid) noexcept
    {
        return IncarnationId{static_cast<RawType>(static_cast<std::make_unsigned_t<pid_t>>(pid))};
    }

    /// \brief Platforms with brokered incarnations (e.g. Fuchsia): the lease issued by the broker.
    static constexpr IncarnationId FromLease(const RawType lease) noexcept
    {
        return IncarnationId{lease};
    }

    static constexpr IncarnationId FromRaw(const RawType raw) noexcept
    {
        return IncarnationId{raw};
    }

    constexpr RawType Raw() const noexcept
    {
        return value_;
    }

    constexpr bool IsValid() const noexcept
    {
        return value_ != 0U;
    }

    /// \brief Inverse of FromPid(). Only meaningful for incarnations created by FromPid() (Linux/QNX).
    constexpr pid_t ToPid() const noexcept
    {
        return static_cast<pid_t>(static_cast<std::make_unsigned_t<pid_t>>(value_));
    }

    /// \brief Address of this incarnation for message passing. Linux/QNX: NodeId == IncarnationId{pid}.
    constexpr message_passing::NodeId ToNodeId() const noexcept
    {
        return message_passing::NodeId::FromWire(value_);
    }

    static constexpr IncarnationId FromNodeId(const message_passing::NodeId node_id) noexcept
    {
        return IncarnationId{node_id.ToWire()};
    }

    std::size_t Hash() const noexcept
    {
        return std::hash<RawType>{}(value_);
    }

    std::string ToString() const
    {
        return "incarnation=" + std::to_string(value_);
    }

    friend constexpr bool operator==(const IncarnationId& lhs, const IncarnationId& rhs) noexcept
    {
        return lhs.value_ == rhs.value_;
    }

    friend constexpr bool operator!=(const IncarnationId& lhs, const IncarnationId& rhs) noexcept
    {
        return lhs.value_ != rhs.value_;
    }

  private:
    explicit constexpr IncarnationId(const RawType value) noexcept : value_{value} {}

    RawType value_{0U};
};

/// \brief Generation of the skeleton-side data of a service instance.
/// \details Strictly increasing per service instance on platforms with brokered epochs; on Linux/QNX it equals the
/// IncarnationId (pid) of the skeleton. 0 means "unpublished", the maximum value means "fenced".
class SkeletonEpoch
{
  public:
    using RawType = std::uint64_t;

    constexpr SkeletonEpoch() noexcept = default;

    static constexpr SkeletonEpoch Unpublished() noexcept
    {
        return SkeletonEpoch{};
    }

    static constexpr SkeletonEpoch Fenced() noexcept
    {
        return SkeletonEpoch{std::numeric_limits<RawType>::max()};
    }

    static constexpr SkeletonEpoch FromRaw(const RawType raw) noexcept
    {
        return SkeletonEpoch{raw};
    }

    /// \brief Linux/QNX: the epoch of the skeleton data is the incarnation of the skeleton that wrote it.
    static constexpr SkeletonEpoch FromIncarnation(const IncarnationId incarnation) noexcept
    {
        return SkeletonEpoch{incarnation.Raw()};
    }

    constexpr RawType Raw() const noexcept
    {
        return value_;
    }

    constexpr bool IsPublished() const noexcept
    {
        return value_ != 0U;
    }

    constexpr bool IsFenced() const noexcept
    {
        return value_ == std::numeric_limits<RawType>::max();
    }

    std::size_t Hash() const noexcept
    {
        return std::hash<RawType>{}(value_);
    }

    std::string ToString() const
    {
        return "epoch=" + std::to_string(value_);
    }

    friend constexpr bool operator==(const SkeletonEpoch& lhs, const SkeletonEpoch& rhs) noexcept
    {
        return lhs.value_ == rhs.value_;
    }

    friend constexpr bool operator!=(const SkeletonEpoch& lhs, const SkeletonEpoch& rhs) noexcept
    {
        return lhs.value_ != rhs.value_;
    }

  private:
    explicit constexpr SkeletonEpoch(const RawType value) noexcept : value_{value} {}

    RawType value_{0U};
};

static_assert(sizeof(IncarnationId) == 8U, "IncarnationId must be 8 bytes");
static_assert(sizeof(SkeletonEpoch) == 8U, "SkeletonEpoch must be 8 bytes");
static_assert(std::is_trivially_copyable_v<IncarnationId>, "IncarnationId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<SkeletonEpoch>, "SkeletonEpoch must be trivially copyable");
static_assert(std::is_standard_layout_v<IncarnationId>, "IncarnationId must be standard layout");
static_assert(std::is_standard_layout_v<SkeletonEpoch>, "SkeletonEpoch must be standard layout");

}  // namespace score::mw::com::impl::lola

template <>
struct std::hash<score::mw::com::impl::lola::IncarnationId>
{
    std::size_t operator()(const score::mw::com::impl::lola::IncarnationId& id) const noexcept
    {
        return id.Hash();
    }
};

template <>
struct std::hash<score::mw::com::impl::lola::SkeletonEpoch>
{
    std::size_t operator()(const score::mw::com::impl::lola::SkeletonEpoch& epoch) const noexcept
    {
        return epoch.Hash();
    }
};

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_INCARNATION_H
