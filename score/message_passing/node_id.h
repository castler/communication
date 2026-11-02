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
#ifndef SCORE_LIB_MESSAGE_PASSING_NODE_ID_H
#define SCORE_LIB_MESSAGE_PASSING_NODE_ID_H

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

namespace score::message_passing
{

/// \brief Opaque identity of one incarnation of a communication peer (a process in the role of client or server).
/// \details The value has no meaning for users; it can only be compared, hashed, printed and transferred on the wire
/// (ToWire()/FromWire()). It is trivially copyable and 8 bytes large, so that it can be stored in shared memory and
/// sent in messages.
/// On Linux/QNX the NodeId is derived from the pid of the peer (FromPid()), other platforms create it from their own
/// peer identity (e.g. a lease on Fuchsia). A default constructed NodeId is invalid and never names a peer.
class NodeId
{
  public:
    using WireType = std::uint64_t;

    constexpr NodeId() noexcept = default;

    explicit constexpr NodeId(const WireType value) noexcept : value_{value} {}

    /// \brief NodeId of a Linux/QNX process; byte-for-byte equivalent to using the pid as node identifier.
    static constexpr NodeId FromPid(const pid_t pid) noexcept
    {
        return NodeId{static_cast<WireType>(static_cast<std::make_unsigned_t<pid_t>>(pid))};
    }

    static constexpr NodeId FromWire(const WireType wire) noexcept
    {
        return NodeId{wire};
    }

    constexpr WireType ToWire() const noexcept
    {
        return value_;
    }

    constexpr WireType Raw() const noexcept
    {
        return value_;
    }

    /// \brief Inverse of FromPid(). Only meaningful for NodeIds created by FromPid() (Linux/QNX).
    constexpr pid_t ToPid() const noexcept
    {
        return static_cast<pid_t>(static_cast<std::make_unsigned_t<pid_t>>(value_));
    }

    constexpr bool IsValid() const noexcept
    {
        return value_ != 0U;
    }

    std::size_t Hash() const noexcept
    {
        return std::hash<WireType>{}(value_);
    }

    std::string ToString() const
    {
        return "node=" + std::to_string(value_);
    }

    friend constexpr bool operator==(const NodeId& lhs, const NodeId& rhs) noexcept
    {
        return lhs.value_ == rhs.value_;
    }

    friend constexpr bool operator!=(const NodeId& lhs, const NodeId& rhs) noexcept
    {
        return lhs.value_ != rhs.value_;
    }

    /// \brief Ordering is only provided so that NodeIds can be stored in ordered containers; it carries no semantics.
    friend constexpr bool operator<(const NodeId& lhs, const NodeId& rhs) noexcept
    {
        return lhs.value_ < rhs.value_;
    }

  private:
    WireType value_{0U};
};

static_assert(sizeof(NodeId) == 8U, "NodeId must be 8 bytes");
static_assert(std::is_trivially_copyable_v<NodeId>, "NodeId must be trivially copyable");
static_assert(std::is_standard_layout_v<NodeId>, "NodeId must be standard layout");

}  // namespace score::message_passing

template <>
struct std::hash<score::message_passing::NodeId>
{
    std::size_t operator()(const score::message_passing::NodeId& node_id) const noexcept
    {
        return node_id.Hash();
    }
};

#endif  // SCORE_LIB_MESSAGE_PASSING_NODE_ID_H
