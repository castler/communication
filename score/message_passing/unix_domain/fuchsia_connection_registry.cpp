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
#include "score/message_passing/unix_domain/fuchsia_connection_registry.h"

#ifdef __Fuchsia__

#include <lib/fdio/fd.h>
#include <unistd.h>
#include <zircon/syscalls.h>

#include <array>

namespace score::message_passing::detail
{

namespace
{

score::cpp::expected<std::array<std::int32_t, 2>, score::os::Error> CreateSocketPair() noexcept
{
    std::array<zx_handle_t, 2> handles{};
    if (zx_socket_create(ZX_SOCKET_STREAM, &handles[0], &handles[1]) != ZX_OK)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOMEM));
    }
    std::array<std::int32_t, 2> fds{-1, -1};
    for (std::size_t i = 0; i < fds.size(); ++i)
    {
        // fdio_fd_create() consumes the handle also on failure
        if (fdio_fd_create(handles[i], &fds[i]) != ZX_OK)
        {
            if (i == 1U)
            {
                std::ignore = ::close(fds[0]);
                return score::cpp::make_unexpected(score::os::Error::createFromErrno(EMFILE));
            }
            std::ignore = zx_handle_close(handles[1]);
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(EMFILE));
        }
    }
    return fds;
}

}  // namespace

FuchsiaConnectionRegistry& FuchsiaConnectionRegistry::Instance() noexcept
{
    static FuchsiaConnectionRegistry instance;
    return instance;
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
    for (const auto fd : found->second.pending)
    {
        std::ignore = ::close(fd);
    }
    std::ignore = ::close(found->second.wake_write_fd);
    listeners_.erase(found);
}

score::cpp::expected<std::int32_t, score::os::Error> FuchsiaConnectionRegistry::Accept(
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
    const std::int32_t fd = found->second.pending.front();
    found->second.pending.pop_front();
    return fd;
}

score::cpp::expected<std::int32_t, score::os::Error> FuchsiaConnectionRegistry::Connect(
    const std::string_view identifier) noexcept
{
    std::lock_guard<std::mutex> guard{mutex_};
    const auto found = listeners_.find(identifier);
    if (found == listeners_.end())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNREFUSED));
    }
    const auto fds = CreateSocketPair();
    if (!fds.has_value())
    {
        return score::cpp::make_unexpected(fds.error());
    }
    found->second.pending.push_back(fds.value()[1]);
    const std::uint8_t token{1U};
    std::ignore = ::write(found->second.wake_write_fd, &token, sizeof(token));
    return fds.value()[0];
}

}  // namespace score::message_passing::detail

#endif  // __Fuchsia__
