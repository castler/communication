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
#ifndef SCORE_LIB_MESSAGE_PASSING_UNIX_DOMAIN_FUCHSIA_CONNECTION_REGISTRY_H
#define SCORE_LIB_MESSAGE_PASSING_UNIX_DOMAIN_FUCHSIA_CONNECTION_REGISTRY_H

#ifdef __Fuchsia__

#include <score/expected.hpp>

#include "score/os/errno.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace score::message_passing::detail
{

/// \brief Replacement for the AF_UNIX connection establishment on Fuchsia, which has no AF_UNIX support.
/// \details Connections are Zircon stream sockets wrapped into file descriptors, so that the poll based engine and
///          the stream framing protocol can be used unchanged. The registry is the rendezvous point that hands the
///          server end of a freshly created socket pair to the server listening on the same identifier.
///          It only connects endpoints within one process. Connecting across components needs the server end
///          to be handed over through a FIDL protocol; this is the only place that has to be replaced for that.
class FuchsiaConnectionRegistry
{
  public:
    static FuchsiaConnectionRegistry& Instance() noexcept;

    /// \return a file descriptor that becomes readable when a connection is pending, to be polled by the server
    score::cpp::expected<std::int32_t, score::os::Error> Listen(std::string_view identifier) noexcept;
    void StopListening(std::string_view identifier) noexcept;

    /// \return the server end of a pending connection
    score::cpp::expected<std::int32_t, score::os::Error> Accept(std::string_view identifier) noexcept;

    /// \return the client end of a new connection, ECONNREFUSED if nobody listens on the identifier
    score::cpp::expected<std::int32_t, score::os::Error> Connect(std::string_view identifier) noexcept;

  private:
    struct Listener
    {
        std::int32_t wake_write_fd;
        std::int32_t wake_read_fd;  // owned by the listening server
        std::deque<std::int32_t> pending;
    };

    std::mutex mutex_;
    std::map<std::string, Listener, std::less<>> listeners_;
};

}  // namespace score::message_passing::detail

#endif  // __Fuchsia__

#endif  // SCORE_LIB_MESSAGE_PASSING_UNIX_DOMAIN_FUCHSIA_CONNECTION_REGISTRY_H
