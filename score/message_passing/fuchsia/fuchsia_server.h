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
#ifndef SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_SERVER_H
#define SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_SERVER_H

#include "score/message_passing/i_server.h"

#include "score/message_passing/fuchsia/fuchsia_engine.h"
#include "score/message_passing/fuchsia/fuchsia_server_factory.h"
#include "score/message_passing/i_server_connection.h"

#include <score/string.hpp>

#include <mutex>
#include <optional>

namespace score::message_passing::detail
{

/// \brief Server of the Fuchsia binding: accepts the connections that clients establish through the
///        score.messagepassing.Connector FIDL protocol.
class FuchsiaServer final : public IServer
{
  public:
    class ServerConnection final : public IServerConnection
    {
      public:
        ServerConnection(FuchsiaServer& server, std::int32_t fd, ClientIdentity client_identity) noexcept;

        // User methods
        const ClientIdentity& GetClientIdentity() const& noexcept override;
        UserData& GetUserData() noexcept override;

        score::cpp::expected_blank<score::os::Error> Reply(
            score::cpp::span<const std::uint8_t> message) noexcept override;
        score::cpp::expected_blank<score::os::Error> Notify(
            score::cpp::span<const std::uint8_t> message) noexcept override;
        void RequestDisconnect() noexcept override;

        // Server methods
        void AcceptConnection(UserData&& data, score::cpp::pmr::unique_ptr<ServerConnection>&& self) noexcept;
        bool ProcessInput();

        ~ServerConnection() noexcept;

        ServerConnection(const ServerConnection&) = delete;
        ServerConnection& operator=(const ServerConnection&) = delete;
        ServerConnection(ServerConnection&&) = delete;
        ServerConnection& operator=(ServerConnection&&) = delete;

      private:
        FuchsiaServer& server_;
        std::optional<UserData> user_data_;
        ClientIdentity client_identity_;
        std::int32_t fd_;
        ISharedResourceEngine::PosixEndpointEntry endpoint_;
        score::cpp::pmr::unique_ptr<ServerConnection> self_;
    };

    FuchsiaServer(std::shared_ptr<FuchsiaEngine> engine,
                  const ServiceProtocolConfig& protocol_config,
                  const IServerFactory::ServerConfig& server_config) noexcept;
    ~FuchsiaServer() noexcept override;

    FuchsiaServer(const FuchsiaServer&) = delete;
    FuchsiaServer& operator=(const FuchsiaServer&) = delete;
    FuchsiaServer(FuchsiaServer&&) = delete;
    FuchsiaServer& operator=(FuchsiaServer&&) = delete;

    score::cpp::expected_blank<score::os::Error> StartListening(
        ConnectCallback connect_callback,
        DisconnectCallback disconnect_callback,
        MessageCallback sent_callback,
        MessageCallback sent_with_reply_callback) noexcept override;

    void StopListening() noexcept override;

  private:
    void ProcessConnect() noexcept;

    std::shared_ptr<FuchsiaEngine> engine_;
    const score::cpp::pmr::string identifier_;
    const std::uint32_t max_request_size_;
    const std::uint32_t max_reply_size_;
    const std::uint32_t max_notify_size_;

    // becomes readable when a client connection is pending in the connection registry
    std::int32_t wake_fd_;
    std::recursive_mutex connection_setup_mutex_;

    ConnectCallback connect_callback_;
    DisconnectCallback disconnect_callback_;
    MessageCallback sent_callback_;
    MessageCallback sent_with_reply_callback_;

    ISharedResourceEngine::CommandQueueEntry listener_command_;
    ISharedResourceEngine::PosixEndpointEntry listener_endpoint_;
};

}  // namespace score::message_passing::detail

#endif  // SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_SERVER_H
