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
#include "score/message_passing/fuchsia/connection_registry.h"

#include <fidl/score.messagepassing/cpp/fidl.h>

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fdio/fd.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <lib/zx/socket.h>
#include <unistd.h>

#include <array>
#include <future>
#include <optional>
#include <tuple>

namespace score::message_passing::detail
{

namespace
{

using Connector = score_messagepassing::Connector;
constexpr const char* kConnectorName = "score.messagepassing.Connector";

score::cpp::expected<std::int32_t, score::os::Error> SocketToFd(zx::socket socket) noexcept
{
    std::int32_t fd{-1};
    // fdio_fd_create() consumes the handle also on failure
    if (fdio_fd_create(socket.release(), &fd) != ZX_OK)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EMFILE));
    }
    return fd;
}

}  // namespace

/// One Connector channel: knows the principal of the endpoint it was opened on and the peer id of its client.
class FuchsiaConnectionRegistry::Session final : public fidl::Server<Connector>
{
  public:
    Session(FuchsiaConnectionRegistry& registry, const std::uint32_t principal) noexcept
        : registry_{registry}, principal_{principal}, peer_id_{registry.NewPeerId()}
    {
    }

    void Connect(ConnectRequest& request, ConnectCompleter::Sync& completer) override
    {
        auto fd = SocketToFd(std::move(request.socket()));
        if (!fd.has_value())
        {
            completer.Reply(fit::error(score_messagepassing::ConnectError::kNotFound));
            return;
        }
        if (!registry_.Enqueue(request.identifier(), fd.value(), principal_, peer_id_))
        {
            std::ignore = ::close(fd.value());
            completer.Reply(fit::error(score_messagepassing::ConnectError::kNotFound));
            return;
        }
        completer.Reply(fit::ok());
    }

  private:
    FuchsiaConnectionRegistry& registry_;
    std::uint32_t principal_;
    std::uint32_t peer_id_;
};

class FuchsiaConnectionRegistry::ConnectorClient
{
  public:
    explicit ConnectorClient(fidl::ClientEnd<Connector> end) noexcept : client{std::move(end)} {}
    fidl::SyncClient<Connector> client;
};

class FuchsiaConnectionRegistry::Service final
{
  public:
    explicit Service(FuchsiaConnectionRegistry& registry) noexcept
        : registry_{registry}, loop_{&kAsyncLoopConfigNeverAttachToThread}
    {
    }

    ~Service() noexcept
    {
        std::promise<void> destroyed;
        if (thread_started_ && (async::PostTask(loop_.dispatcher(), [this, &destroyed]() {
                                    outgoing_.reset();
                                    destroyed.set_value();
                                }) == ZX_OK))
        {
            destroyed.get_future().wait();
        }
        loop_.Shutdown();
    }

    bool Start(const std::set<std::uint32_t>& routed_principals) noexcept
    {
        if (loop_.StartThread("message_passing_connector") != ZX_OK)
        {
            return false;
        }
        thread_started_ = true;
        // OutgoingDirectory is thread-unsafe and must be created, used and destroyed on the dispatcher thread
        std::promise<bool> served;
        auto served_future = served.get_future();
        const auto posted = async::PostTask(loop_.dispatcher(), [this, &served, &routed_principals]() {
            outgoing_.emplace(loop_.dispatcher());
            bool added = AddEndpoint(std::string{kConnectorName}, kUnroutedPrincipal);
            for (const auto principal : routed_principals)
            {
                added = added && AddEndpoint(RoutedConnectorName(principal), principal);
            }
            if (!added)
            {
                served.set_value(false);
                return;
            }
            // Not available if the process was not started with an outgoing directory request, e.g. in a unit
            // test runner. Connections within the same process do not need the service, so this is not a failure.
            std::ignore = outgoing_->ServeFromStartupInfo();
            served.set_value(true);
        });
        return (posted == ZX_OK) && served_future.get();
    }

    // The posted task only touches the outgoing directory and must never take the registry mutex: the caller holds
    // it while waiting, and Session creation (on the dispatcher) takes it in NewPeerId().
    void AddRoutedPrincipal(const std::uint32_t principal) noexcept
    {
        if (!thread_started_)
        {
            return;
        }
        std::promise<void> added;
        if (async::PostTask(loop_.dispatcher(), [this, principal, &added]() {
                std::ignore = AddEndpoint(RoutedConnectorName(principal), principal);
                added.set_value();
            }) == ZX_OK)
        {
            added.get_future().wait();
        }
    }

  private:
    bool AddEndpoint(const std::string& name, const std::uint32_t principal) noexcept
    {
        auto handler = [this, principal](fidl::ServerEnd<Connector> server_end) {
            fidl::BindServer(loop_.dispatcher(), std::move(server_end), std::make_unique<Session>(registry_, principal));
        };
        return !outgoing_->AddUnmanagedProtocol<Connector>(std::move(handler), name).is_error();
    }

    FuchsiaConnectionRegistry& registry_;
    async::Loop loop_;
    bool thread_started_{false};
    std::optional<component::OutgoingDirectory> outgoing_;
};

std::string FuchsiaConnectionRegistry::RoutedConnectorName(const std::uint32_t principal)
{
    return std::string{kConnectorName} + "." + std::to_string(principal);
}

FuchsiaConnectionRegistry::FuchsiaConnectionRegistry(ServiceStarter service_starter) noexcept
    : service_starter_{std::move(service_starter)}
{
}
FuchsiaConnectionRegistry::~FuchsiaConnectionRegistry() noexcept = default;

FuchsiaConnectionRegistry& FuchsiaConnectionRegistry::Instance() noexcept
{
    // Intentionally leaked: the connector thread must not outlive the registry during static destruction.
    static auto* const instance = new FuchsiaConnectionRegistry();
    return *instance;
}

score::cpp::expected<std::int32_t, score::os::Error> FuchsiaConnectionRegistry::Listen(
    const std::string_view identifier) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    if (listeners_.find(identifier) != listeners_.end())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EADDRINUSE));
    }
    std::array<std::int32_t, 2> wake_fds{};
    if (::pipe(wake_fds.data()) != 0)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(errno));
    }
    if (service_ == nullptr)
    {
        auto service = std::make_unique<Service>(*this);
        const bool started = service_starter_ ? service_starter_() : service->Start(routed_principals_);
        if (!started)
        {
            std::ignore = ::close(wake_fds[0]);
            std::ignore = ::close(wake_fds[1]);
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(EIO));
        }
        service_ = std::move(service);
    }
    listeners_.emplace(std::string{identifier}, Listener{wake_fds[1], wake_fds[0], {}});
    return wake_fds[0];
}

void FuchsiaConnectionRegistry::ServeRoutedPrincipal(const std::uint32_t principal) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    if (routed_principals_.insert(principal).second && (service_ != nullptr))
    {
        service_->AddRoutedPrincipal(principal);
    }
}

std::uint32_t FuchsiaConnectionRegistry::NewPeerId() noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    return next_peer_id_++;
}

void FuchsiaConnectionRegistry::StopListening(const std::string_view identifier) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    const auto found = listeners_.find(identifier);
    if (found == listeners_.end())
    {
        return;
    }
    for (const auto& pending : found->second.pending)
    {
        std::ignore = ::close(pending.fd);
    }
    std::ignore = ::close(found->second.wake_write_fd);
    listeners_.erase(found);
}

score::cpp::expected<FuchsiaConnectionRegistry::AcceptedConnection, score::os::Error> FuchsiaConnectionRegistry::Accept(
    const std::string_view identifier) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    const auto found = listeners_.find(identifier);
    if ((found == listeners_.end()) || found->second.pending.empty())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EAGAIN));
    }
    std::uint8_t token{};
    std::ignore = ::read(found->second.wake_read_fd, &token, sizeof(token));
    const AcceptedConnection accepted = found->second.pending.front();
    found->second.pending.pop_front();
    return accepted;
}

bool FuchsiaConnectionRegistry::Enqueue(const std::string_view identifier,
                                        const std::int32_t server_fd,
                                        const std::uint32_t principal,
                                        const std::uint32_t node_id) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    const auto found = listeners_.find(identifier);
    if (found == listeners_.end())
    {
        return false;
    }
    found->second.pending.push_back({server_fd, next_connection_id_++, node_id, principal});
    const std::uint8_t token{1U};
    std::ignore = ::write(found->second.wake_write_fd, &token, sizeof(token));
    return true;
}

score::cpp::expected<std::int32_t, score::os::Error> FuchsiaConnectionRegistry::Connect(
    const std::string_view identifier,
    const std::uint32_t local_principal) noexcept
{
    zx::socket client_end;
    zx::socket server_end;
    if (zx::socket::create(ZX_SOCKET_STREAM, &client_end, &server_end) != ZX_OK)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOMEM));
    }

    bool local{false};
    {
        std::lock_guard<std::mutex> guard{mutex_};
        local = listeners_.find(identifier) != listeners_.end();
    }
    if (local)
    {
        auto server_fd = SocketToFd(std::move(server_end));
        if (!server_fd.has_value())
        {
            return score::cpp::make_unexpected(server_fd.error());
        }
        std::uint32_t node_id{};
        {
            std::lock_guard<std::mutex> guard{mutex_};
            const auto known = local_nodes_.find(local_principal);
            node_id = (known != local_nodes_.end()) ? known->second : (local_nodes_[local_principal] = next_peer_id_++);
        }
        if (!Enqueue(identifier, server_fd.value(), local_principal, node_id))
        {
            std::ignore = ::close(server_fd.value());
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
        }
        return SocketToFd(std::move(client_end));
    }

    // The channel is kept, so that all connections of this process share one peer id on the server. The server
    // derives the principal from the endpoint the channel was opened on; nothing identifying is sent.
    std::lock_guard<std::mutex> guard{connector_mutex_};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (connector_ == nullptr)
        {
            auto connector_end = component::Connect<Connector>();
            if (connector_end.is_error())
            {
                return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
            }
            connector_ = std::make_unique<ConnectorClient>(std::move(connector_end.value()));
        }
        auto result = connector_->client->Connect({{.identifier = std::string{identifier}, .socket = std::move(server_end)}});
        if (result.is_ok())
        {
            break;
        }
        if (result.error_value().is_domain_error() || (attempt == 1))
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
        }
        // The channel is broken (the server restarted): retry on a new channel, which is a new peer
        connector_.reset();
        client_end.reset();
        if (zx::socket::create(ZX_SOCKET_STREAM, &client_end, &server_end) != ZX_OK)
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOMEM));
        }
    }
    return SocketToFd(std::move(client_end));
}

}  // namespace score::message_passing::detail
