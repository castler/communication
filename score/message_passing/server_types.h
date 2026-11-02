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
#ifndef SCORE_LIB_MESSAGE_PASSING_SERVER_TYPES_H
#define SCORE_LIB_MESSAGE_PASSING_SERVER_TYPES_H

#include "score/message_passing/i_connection_handler.h"
#include "score/message_passing/identity_types.h"

#include <score/callback.hpp>
#include <score/memory.hpp>

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace score::message_passing
{

using UserData = std::variant<void*, std::uintptr_t, score::cpp::pmr::unique_ptr<IConnectionHandler>>;

using ConnectCallback = score::cpp::callback<score::cpp::expected<UserData, score::os::Error>(
    IServerConnection& connection) /* noexcept */>;

using DisconnectCallback = score::cpp::callback<void(IServerConnection& connection) /* noexcept */>;

using MessageCallback = score::cpp::callback<score::cpp::expected_blank<score::os::Error>(
    IServerConnection& connection,
    score::cpp::span<const std::uint8_t> message) /* noexcept */>;

/// \brief Opaque identity of the peer of a server connection.
/// \details On Linux/QNX it wraps the OS credentials (pid, uid, gid) of the peer. On Fuchsia, where channels carry no
/// credentials, it carries the connection (node) id and the per-consumer (principal) id assigned at registration.
class ClientIdentity
{
  public:
    using PrincipalId = ::score::message_passing::PrincipalId;
    using NodeId = ::score::message_passing::NodeId;
    using AllowedPrincipals = std::optional<std::set<PrincipalId>>;

    static ClientIdentity FromCredentials(const pid_t pid, const uid_t uid, const gid_t gid) noexcept
    {
        return ClientIdentity{Kind::kCredentials, NodeIdFromPid(pid), PrincipalIdFromUid(uid), gid};
    }

    /// \brief Identity derived from the endpoint, used on Fuchsia.
    /// \param connection_id identifies this connection incarnation (the node); differs for every new connection
    /// \param consumer_id capability identity (the principal) assigned when the consumer registered
    static ClientIdentity FromEndpoint(const std::uint32_t connection_id, const std::uint32_t consumer_id) noexcept
    {
        return ClientIdentity{Kind::kEndpoint, NodeId{connection_id}, PrincipalId{consumer_id}, 0};
    }

    /// \brief Checks the identity against a configured list of allowed principals; no list means everybody is allowed.
    bool IsAllowed(const AllowedPrincipals& allowed) const noexcept
    {
        return (!allowed.has_value()) || (allowed->count(principal_) != 0U);
    }

    bool IsAllowed(const PrincipalId allowed) const noexcept
    {
        return principal_ == allowed;
    }

    /// \brief Key identifying the peer as a communication partner (pid on Linux/QNX, connection id on Fuchsia).
    NodeId GetNodeId() const noexcept
    {
        return node_;
    }

    /// \brief Key used for permission checks (uid on Linux/QNX, consumer id on Fuchsia).
    PrincipalId GetPrincipalId() const noexcept
    {
        return principal_;
    }

    std::size_t Hash() const noexcept
    {
        std::size_t result = std::hash<int>{}(static_cast<int>(kind_));
        const auto combine = [&result](const std::size_t value) noexcept {
            result ^= value + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        };
        combine(std::hash<std::uint64_t>{}(node_.Raw()));
        combine(std::hash<std::uint64_t>{}(principal_.Raw()));
        combine(std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(group_)));
        return result;
    }

    std::string ToString() const
    {
        if (kind_ == Kind::kEndpoint)
        {
            return "connection=" + node_.ToString() + ", consumer=" + principal_.ToString();
        }
        return "pid=" + std::to_string(NodeIdToPid(node_)) + ", uid=" + std::to_string(PrincipalIdToUid(principal_)) +
               ", gid=" + std::to_string(group_);
    }

    friend bool operator==(const ClientIdentity& lhs, const ClientIdentity& rhs) noexcept
    {
        return (lhs.kind_ == rhs.kind_) && (lhs.node_ == rhs.node_) && (lhs.principal_ == rhs.principal_) &&
               (lhs.group_ == rhs.group_);
    }

    friend bool operator!=(const ClientIdentity& lhs, const ClientIdentity& rhs) noexcept
    {
        return !(lhs == rhs);
    }

  private:
    enum class Kind : std::uint8_t
    {
        kCredentials,
        kEndpoint,
    };

    ClientIdentity(const Kind kind, const NodeId node, const PrincipalId principal, const gid_t group) noexcept
        : kind_{kind}, node_{node}, principal_{principal}, group_{group}
    {
    }

    Kind kind_;
    NodeId node_;
    PrincipalId principal_;
    gid_t group_;
};

}  // namespace score::message_passing

template <>
struct std::hash<score::message_passing::ClientIdentity>
{
    std::size_t operator()(const score::message_passing::ClientIdentity& identity) const noexcept
    {
        return identity.Hash();
    }
};

#endif  // SCORE_LIB_MESSAGE_PASSING_SERVER_TYPES_H
