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
#ifndef SCORE_LIB_MESSAGE_PASSING_IDENTITY_TYPES_H
#define SCORE_LIB_MESSAGE_PASSING_IDENTITY_TYPES_H

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace score::message_passing
{

/// \brief Opaque 64 bit identity value. Instantiations with different tags are unrelated, non-convertible types.
/// \details The raw value is only accessible via Raw(). Platform specific conversions (pid, uid) are provided by the
/// named factories below, so that code outside of the platform layer cannot treat an identity as an OS id by accident.
template <typename Tag>
class OpaqueId
{
  public:
    constexpr OpaqueId() noexcept = default;

    constexpr explicit OpaqueId(const std::uint64_t raw) noexcept : raw_{raw} {}

    constexpr std::uint64_t Raw() const noexcept
    {
        return raw_;
    }

    std::string ToString() const
    {
        return std::to_string(raw_);
    }

    friend constexpr bool operator==(const OpaqueId lhs, const OpaqueId rhs) noexcept
    {
        return lhs.raw_ == rhs.raw_;
    }

    friend constexpr bool operator!=(const OpaqueId lhs, const OpaqueId rhs) noexcept
    {
        return lhs.raw_ != rhs.raw_;
    }

    /// \brief Ordering is only provided to allow storage in ordered containers; it carries no semantics.
    friend constexpr bool operator<(const OpaqueId lhs, const OpaqueId rhs) noexcept
    {
        return lhs.raw_ < rhs.raw_;
    }

  private:
    std::uint64_t raw_{0U};
};

struct NodeIdTag;
struct PrincipalIdTag;

/// \brief Communication address/identity of a peer (pid on Linux/QNX).
using NodeId = OpaqueId<NodeIdTag>;

/// \brief Identity used for permission checks (uid on Linux/QNX).
using PrincipalId = OpaqueId<PrincipalIdTag>;

constexpr NodeId NodeIdFromPid(const pid_t pid) noexcept
{
    return NodeId{static_cast<std::uint64_t>(static_cast<std::uint32_t>(pid))};
}

constexpr pid_t NodeIdToPid(const NodeId node) noexcept
{
    return static_cast<pid_t>(static_cast<std::uint32_t>(node.Raw()));
}

constexpr PrincipalId PrincipalIdFromUid(const uid_t uid) noexcept
{
    return PrincipalId{static_cast<std::uint64_t>(uid)};
}

constexpr uid_t PrincipalIdToUid(const PrincipalId principal) noexcept
{
    return static_cast<uid_t>(principal.Raw());
}

}  // namespace score::message_passing

template <typename Tag>
struct std::hash<score::message_passing::OpaqueId<Tag>>
{
    std::size_t operator()(const score::message_passing::OpaqueId<Tag> id) const noexcept
    {
        return std::hash<std::uint64_t>{}(id.Raw());
    }
};

#endif  // SCORE_LIB_MESSAGE_PASSING_IDENTITY_TYPES_H
