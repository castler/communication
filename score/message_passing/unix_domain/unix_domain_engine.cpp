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
#include "score/message_passing/unix_domain/unix_domain_engine.h"

#include "score/message_passing/unix_domain/fuchsia_connection_registry.h"
#include "score/message_passing/unix_domain/unix_domain_socket_address.h"

#include <array>
#include <future>
#include <tuple>

#include <unistd.h>

namespace score::message_passing
{

#ifdef __Fuchsia__
namespace
{

// Zircon socket descriptors support read()/write() but not sendmsg()/recvmsg()
score::cpp::expected_blank<score::os::Error> WriteAll(const std::int32_t fd,
                                                      const std::uint8_t* data,
                                                      std::size_t size) noexcept
{
    while (size > 0U)
    {
        const auto written = ::write(fd, data, size);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(errno));
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return {};
}

// \return the number of bytes read, which is less than size only if the peer has disconnected
score::cpp::expected<std::size_t, score::os::Error> ReadAll(const std::int32_t fd,
                                                            std::uint8_t* data,
                                                            const std::size_t size) noexcept
{
    std::size_t done = 0U;
    while (done < size)
    {
        const auto count = ::read(fd, data + done, size - done);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(errno));
        }
        if (count == 0)
        {
            break;
        }
        done += static_cast<std::size_t>(count);
    }
    return done;
}

}  // namespace
#endif


UnixDomainEngine::UnixDomainEngine(score::cpp::pmr::memory_resource* memory_resource, LoggingCallback logger) noexcept
    : memory_resource_{memory_resource},
      os_resources_{GetDefaultOsResources(memory_resource)},
      logger_{std::move(logger)},
      quit_flag_{false},
      poll_fds_{memory_resource},
      poll_endpoints_{memory_resource},
      posix_receive_buffer_{memory_resource}
{
    std::ignore = os_resources_.unistd->pipe(pipe_fds_.data());
#ifdef __Fuchsia__
    // poll() on Fuchsia reports POLLNVAL for negative descriptors instead of ignoring them, which would make
    // the poll loop spin. The write end of the wake-up pipe never becomes ready, so it parks the unused entries.
    unused_poll_fd_ = pipe_fds_[1];
#endif

    // Normally, during the application lifecycle initialization, LifeCycleManager blocks the SIGTERM on the main
    // thread and creates a separate thread that catches all the SIGTERM signals coming to the process. The other
    // threads created after that will inherit the sigmask of the main thread with SIGTERM blocked.
    // However, LifeCycleManager starts using Logging before it blocks SIGTERM on the main thread. When this happens,
    // Logging will initialize Message Passing, which will create the Message Passing background thread with a sigmask
    // inherited without SIGTERM being blocked yet.
    // Thus, we need to mask SIGTERM for this thread specifically, to let the LifeCycleManager desicated SIGTERM
    // thread do its job.
    sigset_t new_set;
    sigset_t old_set;
    // the signal functions below, used with the parameters below, can only return EOK
    score::cpp::ignore = os_resources_.signal->SigEmptySet(new_set);
    score::cpp::ignore = os_resources_.signal->AddTerminationSignal(new_set);
    score::cpp::ignore = os_resources_.signal->PthreadSigMask(SIG_BLOCK, new_set, old_set);
    {
        std::lock_guard acquire{thread_mutex_};  // postpone RunOnThread() till we assign thread_
        thread_ = std::thread([this]() noexcept {
            {
                std::lock_guard release{thread_mutex_};  // guarantees that this->thread_ is already assigned
            }
            RunOnThread();
        });
    }
    score::cpp::ignore = os_resources_.signal->PthreadSigMask(SIG_SETMASK, old_set);
}

UnixDomainEngine::~UnixDomainEngine() noexcept
{
    SendPipeEvent(PipeEvent::QUIT);
    thread_.join();
    std::ignore = os_resources_.unistd->close(pipe_fds_[0]);
    std::ignore = os_resources_.unistd->close(pipe_fds_[1]);
}

score::cpp::expected<std::int32_t, score::os::Error> UnixDomainEngine::TryOpenClientConnection(
    std::string_view identifier) noexcept
{
#ifdef __Fuchsia__
    // no AF_UNIX on Fuchsia
    return detail::FuchsiaConnectionRegistry::Instance().Connect(identifier);
#else
    const auto fd_expected = os_resources_.socket->socket(score::os::Socket::Domain::kUnix, SOCK_STREAM, 0);
    if (!fd_expected.has_value())
    {
        return score::cpp::make_unexpected(fd_expected.error());
    }

    const std::int32_t client_fd = fd_expected.value();
    detail::UnixDomainSocketAddress addr{identifier, true};

    const auto connect_expected = os_resources_.socket->connect(client_fd, addr.data(), addr.size());
    if (!connect_expected.has_value())
    {
        std::ignore = os_resources_.unistd->close(client_fd);
        return score::cpp::make_unexpected(connect_expected.error());
    }
    return client_fd;
#endif
}

void UnixDomainEngine::CloseClientConnection(std::int32_t client_fd) noexcept
{
    std::ignore = os_resources_.unistd->close(client_fd);
}

void UnixDomainEngine::RegisterPosixEndpoint(PosixEndpointEntry& endpoint) noexcept
{
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD(IsOnCallbackThread());

    if (posix_receive_buffer_.size() < endpoint.max_receive_size)
    {
        posix_receive_buffer_.resize(endpoint.max_receive_size);
    }

    std::int16_t events = 0;
    if (!endpoint.input.empty())
    {
        // NOLINTNEXTLINE(hicpp-signed-bitwise): POLL* are non-negative POSIX constants and pollfd::events is signed
        events |= POLLIN;
    }
    if (!endpoint.output.empty())
    {
        // TODO: not used/not supported yet
        // NOLINTNEXTLINE(hicpp-signed-bitwise): POLL* are non-negative POSIX constants and pollfd::events is signed
        events |= POLLOUT;
    }
    const auto found = std::find(poll_endpoints_.begin(), poll_endpoints_.end(), nullptr);
    if (found != poll_endpoints_.end())
    {
        const auto i = static_cast<std::size_t>(std::distance(poll_endpoints_.begin(), found));
        poll_fds_[i] = {endpoint.fd, events, 0};
        poll_endpoints_[i] = &endpoint;
    }
    else
    {
        std::ignore = poll_fds_.emplace_back(pollfd{endpoint.fd, events, 0});
        std::ignore = poll_endpoints_.emplace_back(&endpoint);
    }
    posix_endpoint_list_.push_back(endpoint);
}

void UnixDomainEngine::UnregisterPosixEndpoint(PosixEndpointEntry& endpoint) noexcept
{
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD(IsOnCallbackThread());

    const auto found = std::find(poll_endpoints_.begin(), poll_endpoints_.end(), &endpoint);
    if (found != poll_endpoints_.end())
    {
        const auto i = static_cast<std::size_t>(std::distance(poll_endpoints_.begin(), found));
        UnpollEndpoint(i);
    }
}

void UnixDomainEngine::UnpollEndpoint(const std::size_t index) noexcept
{
    PosixEndpointEntry& endpoint = *poll_endpoints_[index];
    std::ignore = posix_endpoint_list_.erase(posix_endpoint_list_.iterator_to(endpoint));
    poll_endpoints_[index] = nullptr;
    poll_fds_[index].fd = unused_poll_fd_;
    poll_fds_[index].events = 0;
    poll_fds_[index].revents = 0;
    if (!endpoint.disconnect.empty())
    {
        endpoint.disconnect();
    }
}

void UnixDomainEngine::EnqueueCommand(CommandQueueEntry& entry,
                                      const TimePoint until,
                                      CommandCallback callback,
                                      const void* const owner) noexcept
{
    timer_queue_.RegisterTimedEntry(entry, until, std::move(callback), owner);
    SendPipeEvent(PipeEvent::TIMER);
}

void UnixDomainEngine::CleanUpOwner(const void* const owner) noexcept
{
    if (owner == nullptr)
    {
        return;
    }
    if (IsOnCallbackThread())
    {
        ProcessCleanup(owner);
    }
    else
    {
        std::promise<void> done;  // TODO: maybe switch to NonAllocatingFuture
        detail::TimedCommandQueue::Entry cleanup_command;
        timer_queue_.RegisterImmediateEntry(
            cleanup_command,
            [this, owner, &done](auto) noexcept {
                ProcessCleanup(owner);
                done.set_value();
            },
            owner);
        SendPipeEvent(PipeEvent::TIMER);
        done.get_future().wait();
    }
}

score::cpp::expected_blank<score::os::Error> UnixDomainEngine::SendProtocolMessage(
    const std::int32_t fd,
    std::uint8_t code,
    const score::cpp::span<const std::uint8_t> message) noexcept
{
#ifdef __Fuchsia__
    const auto size = static_cast<std::uint16_t>(message.size());
    score::cpp::pmr::vector<std::uint8_t> frame{memory_resource_};
    frame.reserve(sizeof(code) + sizeof(size) + message.size());
    frame.push_back(code);
    frame.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>(size >> 8U));
    frame.insert(frame.end(), message.begin(), message.end());
    return WriteAll(fd, frame.data(), frame.size());
#else
    struct msghdr msg{};
    constexpr auto kVectorCount = 3UL;
    auto size = static_cast<std::uint16_t>(message.size());
    std::array<iovec, kVectorCount> io{};
    // Deviation of MISRA RULE-6-8-3: codeql::misra_deviation_next_line(unix-domain-iovec-stack-buffers)
    io[0].iov_base = &code;
    io[0].iov_len = sizeof(code);
    // Deviation of MISRA RULE-6-8-3: codeql::misra_deviation_next_line(unix-domain-iovec-stack-buffers)
    io[1].iov_base = &size;
    io[1].iov_len = sizeof(size);
    io[2].iov_base = const_cast<std::uint8_t*>(message.data());
    io[2].iov_len = message.size();
    msg.msg_iov = io.data();
    msg.msg_iovlen = kVectorCount;

    using MessageFlag = ::score::os::Socket::MessageFlag;
    const auto result_expected =
        os_resources_.socket->sendmsg(fd, &msg, MessageFlag::kWaitAll | MessageFlag::kNoSignal);
    if (result_expected.has_value())
    {
        return {};
    }
    return score::cpp::make_unexpected(result_expected.error());
#endif
}

score::cpp::expected<score::cpp::span<const std::uint8_t>, score::os::Error> UnixDomainEngine::ReceiveProtocolMessage(
    const std::int32_t fd,
    std::uint8_t& code) noexcept
{
#ifdef __Fuchsia__
    std::array<std::uint8_t, sizeof(code) + sizeof(std::uint16_t)> header{};
    const auto header_expected = ReadAll(fd, header.data(), header.size());
    if (!header_expected.has_value())
    {
        return score::cpp::make_unexpected(header_expected.error());
    }
    if (header_expected.value() != header.size())
    {
        // other side disconnected
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EPIPE));
    }
    code = header[0];
    const std::size_t message_size = static_cast<std::size_t>(header[1]) | (static_cast<std::size_t>(header[2]) << 8U);
    if (message_size == 0U)
    {
        return {};
    }
    if (message_size > posix_receive_buffer_.size())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EMSGSIZE));
    }
    const auto payload_expected = ReadAll(fd, posix_receive_buffer_.data(), message_size);
    if (!payload_expected.has_value())
    {
        return score::cpp::make_unexpected(payload_expected.error());
    }
    if (payload_expected.value() != message_size)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EIO));
    }
    return score::cpp::span<const std::uint8_t>{posix_receive_buffer_.data(), message_size};
#else
    struct msghdr msg{};
    constexpr auto kVectorCount = 2UL;
    std::uint16_t size{};
    std::array<iovec, kVectorCount> io{};
    // Deviation of MISRA RULE-6-8-3: codeql::misra_deviation_next_line(unix-domain-iovec-stack-buffers)
    io[0].iov_base = &code;
    io[0].iov_len = sizeof(code);
    // Deviation of MISRA RULE-6-8-3: codeql::misra_deviation_next_line(unix-domain-iovec-stack-buffers)
    io[1].iov_base = &size;
    io[1].iov_len = sizeof(size);
    msg.msg_iov = io.data();
    msg.msg_iovlen = kVectorCount;

    using MessageFlag = ::score::os::Socket::MessageFlag;
    auto size_expected = os_resources_.socket->recvmsg(fd, &msg, MessageFlag::kWaitAll | MessageFlag::kNoSignal);
    if (!size_expected.has_value())
    {
        return score::cpp::make_unexpected(size_expected.error());
    }
    if (size_expected.value() == 0)
    {
        // other side disconnected
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EPIPE));
    }
    // size itself must stay std::uint16_t: it is read directly into raw memory via the iovec above
    // (io[1].iov_len = sizeof(size)), matching the exact 2-byte wire field written by SendProtocolMessage.
    // Widening it (e.g. to std::size_t, whose width is even platform/bitness-dependent) would change the
    // number of bytes read off the wire and break the protocol. Instead, widen only a local copy used for
    // the comparisons below -- size's full range (0..65535) is always exactly representable in std::size_t.
    const std::size_t message_size = size;
    if (message_size == 0U)
    {
        return {};
    }
    if (message_size > posix_receive_buffer_.size())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EMSGSIZE));
    }

    io[0].iov_base = posix_receive_buffer_.data();
    io[0].iov_len = message_size;
    msg.msg_iovlen = 1UL;

    using MessageFlag = ::score::os::Socket::MessageFlag;
    size_expected = os_resources_.socket->recvmsg(fd, &msg, MessageFlag::kWaitAll | MessageFlag::kNoSignal);
    if (!size_expected.has_value())
    {
        return score::cpp::make_unexpected(size_expected.error());
    }
    // size_expected.value() is a recvmsg() byte count, which is guaranteed non-negative whenever has_value()
    // is true (already checked above), so the signed-to-unsigned cast below never reinterprets a negative
    // value.
    // coverity[autosar_cpp14_a4_7_1_violation]
    if (static_cast<std::size_t>(size_expected.value()) != message_size)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EIO));
    }
    return score::cpp::span<const std::uint8_t>{posix_receive_buffer_.data(), size};
#endif
}

void UnixDomainEngine::SendPipeEvent(PipeEvent pipe_event) noexcept
{
    std::ignore = os_resources_.unistd->write(pipe_fds_[1], &pipe_event, sizeof(pipe_event));
}

void UnixDomainEngine::ProcessPipeEvent() noexcept
{
    PipeEvent pipe_event{PipeEvent::QUIT};
    std::ignore = os_resources_.unistd->read(pipe_fds_[0], &pipe_event, sizeof(pipe_event));
    if (pipe_event == PipeEvent::TIMER)
    {
        // Intentionally empty. Just wake up to recalculate poll timeout.
    }
    else
    {
        quit_flag_ = true;
    }
}

void UnixDomainEngine::ProcessCleanup(const void* const owner) noexcept
{
    for (std::size_t i = 0; i < poll_fds_.size(); ++i)
    {
        if (poll_endpoints_[i] == nullptr)
        {
            continue;
        }
        if (poll_endpoints_[i]->owner == owner)
        {
            UnpollEndpoint(i);
        }
    }
    timer_queue_.CleanUpOwner(owner);
}

void UnixDomainEngine::RunOnThread() noexcept
{
    command_endpoint_.owner = this;
    command_endpoint_.fd = pipe_fds_[0];
    command_endpoint_.input = [this]() noexcept {
        ProcessPipeEvent();
    };
    command_endpoint_.output = {};
    command_endpoint_.disconnect = {};
    RegisterPosixEndpoint(command_endpoint_);

    while (!quit_flag_)
    {
        std::int32_t timeout = ProcessTimerQueue();
        const auto num_expected = os_resources_.poll->poll(poll_fds_.data(), poll_fds_.size(), timeout);
        if ((num_expected.has_value()) && (num_expected.value() > 0))
        {
            for (std::size_t i = 0; i < poll_fds_.size(); ++i)
            {
                // some platforms report POLLNVAL for the unregistered (negative) descriptors
                if ((poll_fds_[i].revents != 0) && (poll_endpoints_[i] != nullptr))
                {
                    PosixEndpointEntry& endpoint = *poll_endpoints_[i];
                    endpoint.input();
                }
            }
        }
    }

    UnregisterPosixEndpoint(command_endpoint_);
}

std::int32_t UnixDomainEngine::ProcessTimerQueue() noexcept
{
    const auto now = Clock::now();
    const auto then = timer_queue_.ProcessQueue(now);
    if (then == TimePoint{})
    {
        return -1;
    }
    const auto distance_ms = std::chrono::duration_cast<std::chrono::milliseconds>(then - Clock::now()).count();
    if (distance_ms >= INT32_MAX)
    {
        return INT32_MAX;
    }
    // At this point distance_ms < INT32_MAX, so adding 1 cannot overflow.
    return static_cast<std::int32_t>(distance_ms + 1);
}

}  // namespace score::message_passing
