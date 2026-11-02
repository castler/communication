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

class FuchsiaConnectionRegistry::Service final : public fidl::Server<Connector>
{
  public:
    explicit Service(FuchsiaConnectionRegistry& registry) noexcept
        : registry_{registry}, loop_{&kAsyncLoopConfigNeverAttachToThread}
    {
    }

    ~Service() noexcept override
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

    bool Start() noexcept
    {
        if (loop_.StartThread("message_passing_connector") != ZX_OK)
        {
            return false;
        }
        thread_started_ = true;
        // OutgoingDirectory is thread-unsafe and must be created, used and destroyed on the dispatcher thread
        std::promise<bool> served;
        auto served_future = served.get_future();
        const auto posted = async::PostTask(loop_.dispatcher(), [this, &served]() {
            outgoing_.emplace(loop_.dispatcher());
            if (outgoing_
                    ->AddUnmanagedProtocol<Connector>(
                        bindings_.CreateHandler(this, loop_.dispatcher(), fidl::kIgnoreBindingClosure))
                    .is_error())
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

    void Connect(ConnectRequest& request, ConnectCompleter::Sync& completer) override
    {
        auto fd = SocketToFd(std::move(request.socket()));
        if (!fd.has_value())
        {
            completer.Reply(fit::error(score_messagepassing::ConnectError::kNotFound));
            return;
        }
        if (!registry_.Enqueue(request.identifier(), fd.value(), request.consumer_id()))
        {
            std::ignore = ::close(fd.value());
            completer.Reply(fit::error(score_messagepassing::ConnectError::kNotFound));
            return;
        }
        completer.Reply(fit::ok());
    }

  private:
    FuchsiaConnectionRegistry& registry_;
    async::Loop loop_;
    bool thread_started_{false};
    std::optional<component::OutgoingDirectory> outgoing_;
    fidl::ServerBindingGroup<Connector> bindings_;
};

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
        const bool started = service_starter_ ? service_starter_() : service->Start();
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
                                        const std::uint32_t consumer_id) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    const auto found = listeners_.find(identifier);
    if (found == listeners_.end())
    {
        return false;
    }
    found->second.pending.push_back({server_fd, next_connection_id_++, consumer_id});
    const std::uint8_t token{1U};
    std::ignore = ::write(found->second.wake_write_fd, &token, sizeof(token));
    return true;
}

score::cpp::expected<std::int32_t, score::os::Error> FuchsiaConnectionRegistry::Connect(
    const std::string_view identifier,
    const std::uint32_t consumer_id) noexcept
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
        if (!Enqueue(identifier, server_fd.value(), consumer_id))
        {
            std::ignore = ::close(server_fd.value());
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
        }
        return SocketToFd(std::move(client_end));
    }

    auto connector_end = component::Connect<Connector>();
    if (connector_end.is_error())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
    }
    fidl::SyncClient<Connector> connector{std::move(connector_end.value())};
    auto result = connector->Connect(
        {{.identifier = std::string{identifier}, .socket = std::move(server_end), .consumer_id = consumer_id}});
    if (result.is_error())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
    }
    return SocketToFd(std::move(client_end));
}

}  // namespace score::message_passing::detail
