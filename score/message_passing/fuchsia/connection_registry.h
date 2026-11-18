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
#ifndef SCORE_LIB_MESSAGE_PASSING_FUCHSIA_CONNECTION_REGISTRY_H
#define SCORE_LIB_MESSAGE_PASSING_FUCHSIA_CONNECTION_REGISTRY_H

#include <score/expected.hpp>

#include "score/os/errno.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace score::message_passing::detail
{

/// \brief Connection establishment for Fuchsia, which has no AF_UNIX support.
/// \details Connections are Zircon stream sockets wrapped into file descriptors, which the FuchsiaEngine polls and
///          frames like any other stream.
///          A server registers its identifier here and publishes the score.messagepassing.Connector FIDL protocol
///          in the outgoing directory of its component. A client connecting to an identifier that is not served
///          by the same process creates a socket pair and sends one end through the Connector protocol found in
///          its incoming namespace. FIDL is only involved in establishing the connection.
///
///          Identity is never supplied by the connecting client: the Connector protocol carries no identity, the
///          server derives it from the endpoint the connection arrived on.
///          - principal: the serving process publishes one Connector endpoint per principal it knows
///            (ServeRoutedPrincipal) under RoutedConnectorName(principal). The realm routes exactly that endpoint to
///            the consumer, renamed to the standard protocol name (`offer: protocol
///            "score.messagepassing.Connector.7", from: "#provider", as: "score.messagepassing.Connector"`), so the
///            principal is the application/consumer id the realm assigned to the consumer. Connections through the
///            standard endpoint without per-consumer routing get kUnroutedPrincipal, which is never part of an
///            allowed-consumer list.
///          - node: a peer id assigned by the server per Connector channel. A client process keeps one channel for
///            its lifetime, so all its connections share the node, while another or a restarted process gets a new
///            one. This is a process-lifetime token, not an incarnation counter of the broker.
///          Connections inside one process are trusted: they carry the principal given to Connect and a node per
///          principal.
class FuchsiaConnectionRegistry
{
  public:
    /// \brief Replaces starting of the Connector service, to inject failures in tests
    using ServiceStarter = std::function<bool()>;

    /// \brief Principal of connections that arrive through the standard endpoint without per-consumer routing
    static constexpr std::uint32_t kUnroutedPrincipal{0xFFFFFFFFU};

    /// \brief Name of the Connector endpoint that the realm routes to the consumer with id `principal`
    static std::string RoutedConnectorName(std::uint32_t principal);

    static FuchsiaConnectionRegistry& Instance() noexcept;

    explicit FuchsiaConnectionRegistry(ServiceStarter service_starter = {}) noexcept;

    ~FuchsiaConnectionRegistry() noexcept;
    FuchsiaConnectionRegistry(const FuchsiaConnectionRegistry&) = delete;
    FuchsiaConnectionRegistry& operator=(const FuchsiaConnectionRegistry&) = delete;

    /// \return a file descriptor that becomes readable when a connection is pending, to be polled by the server;
    ///         an error, without leaving a listener behind, if the Connector service cannot be started
    score::cpp::expected<std::int32_t, score::os::Error> Listen(std::string_view identifier) noexcept;
    void StopListening(std::string_view identifier) noexcept;

    /// \brief Publishes the endpoint routed to the consumer with id `principal`; connections arriving on it are
    ///        accepted with that principal. Idempotent, may be called before or after Listen.
    void ServeRoutedPrincipal(std::uint32_t principal) noexcept;

    /// \brief The server end of a pending connection with the identities attached to it
    struct AcceptedConnection
    {
        std::int32_t fd;
        std::uint32_t connection_id;  ///< unique per connection, diagnostics only
        std::uint32_t node_id;        ///< identifies the peer process
        std::uint32_t principal_id;   ///< derived from the endpoint the connection arrived on
    };

    /// \return the server end of a pending connection
    score::cpp::expected<AcceptedConnection, score::os::Error> Accept(std::string_view identifier) noexcept;

    /// \param local_principal principal presented to a server in the same process; ignored for servers in other
    ///        processes, which derive the principal from the routed endpoint
    /// \return the client end of a new connection, ECONNREFUSED if nobody listens on the identifier
    score::cpp::expected<std::int32_t, score::os::Error> Connect(std::string_view identifier,
                                                                 std::uint32_t local_principal = 0U) noexcept;

    /// \brief Queues the server end of a connection that arrived through a Connector endpoint
    /// \return false if nobody listens on the identifier
    bool Enqueue(std::string_view identifier,
                 std::int32_t server_fd,
                 std::uint32_t principal,
                 std::uint32_t node_id) noexcept;

    /// \return a new peer id, to be assigned to one Connector channel
    std::uint32_t NewPeerId() noexcept;

  private:
    class Service;
    class Session;
    class ConnectorClient;

    struct Listener
    {
        std::int32_t wake_write_fd;
        std::int32_t wake_read_fd;  // owned by the listening server
        std::deque<AcceptedConnection> pending;
    };

    std::mutex mutex_;
    std::uint32_t next_connection_id_{1U};
    std::uint32_t next_peer_id_{1U};
    std::set<std::uint32_t> routed_principals_;
    std::map<std::uint32_t, std::uint32_t> local_nodes_;
    std::map<std::string, Listener, std::less<>> listeners_;
    std::unique_ptr<Service> service_;
    ServiceStarter service_starter_;

    std::mutex connector_mutex_;
    std::unique_ptr<ConnectorClient> connector_;  // one channel per process: its peer id is the node of the process
};

}  // namespace score::message_passing::detail

#endif  // SCORE_LIB_MESSAGE_PASSING_FUCHSIA_CONNECTION_REGISTRY_H
