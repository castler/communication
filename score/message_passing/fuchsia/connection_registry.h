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
class FuchsiaConnectionRegistry
{
  public:
    /// \brief Replaces starting of the Connector service, to inject failures in tests
    using ServiceStarter = std::function<bool()>;

    static FuchsiaConnectionRegistry& Instance() noexcept;

    explicit FuchsiaConnectionRegistry(ServiceStarter service_starter = {}) noexcept;

    ~FuchsiaConnectionRegistry() noexcept;
    FuchsiaConnectionRegistry(const FuchsiaConnectionRegistry&) = delete;
    FuchsiaConnectionRegistry& operator=(const FuchsiaConnectionRegistry&) = delete;

    /// \return a file descriptor that becomes readable when a connection is pending, to be polled by the server;
    ///         an error, without leaving a listener behind, if the Connector service cannot be started
    score::cpp::expected<std::int32_t, score::os::Error> Listen(std::string_view identifier) noexcept;
    void StopListening(std::string_view identifier) noexcept;

    /// \return the server end of a pending connection
    score::cpp::expected<std::int32_t, score::os::Error> Accept(std::string_view identifier) noexcept;

    /// \return the client end of a new connection, ECONNREFUSED if nobody listens on the identifier
    score::cpp::expected<std::int32_t, score::os::Error> Connect(std::string_view identifier) noexcept;

    /// \brief Queues the server end of a connection that arrived through the Connector protocol
    /// \return false if nobody listens on the identifier
    bool Enqueue(std::string_view identifier, std::int32_t server_fd) noexcept;

  private:
    class Service;

    struct Listener
    {
        std::int32_t wake_write_fd;
        std::int32_t wake_read_fd;  // owned by the listening server
        std::deque<std::int32_t> pending;
    };

    std::mutex mutex_;
    std::map<std::string, Listener, std::less<>> listeners_;
    std::unique_ptr<Service> service_;
    ServiceStarter service_starter_;
};

}  // namespace score::message_passing::detail

#endif  // SCORE_LIB_MESSAGE_PASSING_FUCHSIA_CONNECTION_REGISTRY_H
