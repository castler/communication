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
#include "score/message_passing/fuchsia/fuchsia_engine.h"

#include "score/message_passing/fuchsia/connection_registry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <future>
#include <limits>
#include <tuple>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace score::message_passing
{

namespace
{

// Zircon socket descriptors support read() and write(), but not sendmsg() and recvmsg()

// Writes as much as the socket accepts without blocking, requires O_NONBLOCK.
// \return the number of bytes written; may be less than size if the socket is full
score::cpp::expected<std::size_t, score::os::Error> WriteSome(const std::int32_t fd,
                                                              const std::uint8_t* data,
                                                              const std::size_t size) noexcept
{
    std::size_t done = 0U;
    while (done < size)
    {
        const auto written = ::write(fd, data + done, size - done);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                break;
            }
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(errno));
        }
        done += static_cast<std::size_t>(written);
    }
    return done;
}

void EnsureNonBlocking(const std::int32_t fd) noexcept
{
    const auto flags = ::fcntl(fd, F_GETFL);
    if ((flags >= 0) && ((flags & O_NONBLOCK) == 0))
    {
        std::ignore = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
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
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                // descriptors are non-blocking for the sake of the writers; the peer is mid-frame, so wait for the rest
                pollfd wait{fd, POLLIN, 0};
                std::ignore = ::poll(&wait, 1, -1);
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

FuchsiaEngine::FuchsiaEngine(score::cpp::pmr::memory_resource* memory_resource,
                             LoggingCallback logger,
                             const std::uint32_t consumer_id) noexcept
    : memory_resource_{memory_resource},
      consumer_id_{consumer_id},
      os_resources_{GetDefaultOsResources(memory_resource)},
      logger_{std::move(logger)},
      quit_flag_{false},
      poll_fds_{memory_resource},
      poll_endpoints_{memory_resource},
      poll_scratch_{memory_resource},
      posix_receive_buffer_{memory_resource}
{
    std::ignore = os_resources_.unistd->pipe(pipe_fds_.data());

    {
        std::lock_guard acquire{thread_mutex_};  // postpone RunOnThread() till we assign thread_
        thread_ = std::thread([this]() noexcept {
            {
                std::lock_guard release{thread_mutex_};  // guarantees that this->thread_ is already assigned
            }
            RunOnThread();
        });
    }
}

FuchsiaEngine::~FuchsiaEngine() noexcept
{
    SendPipeEvent(PipeEvent::QUIT);
    thread_.join();
    std::ignore = os_resources_.unistd->close(pipe_fds_[0]);
    std::ignore = os_resources_.unistd->close(pipe_fds_[1]);
}

score::cpp::expected<std::int32_t, score::os::Error> FuchsiaEngine::TryOpenClientConnection(
    std::string_view identifier) noexcept
{
    return detail::FuchsiaConnectionRegistry::Instance().Connect(identifier, consumer_id_);
}

void FuchsiaEngine::CloseClientConnection(std::int32_t client_fd) noexcept
{
    DiscardSendQueue(client_fd);
    std::ignore = os_resources_.unistd->close(client_fd);
}

void FuchsiaEngine::RegisterPosixEndpoint(PosixEndpointEntry& endpoint) noexcept
{
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD(IsOnCallbackThread());

    DiscardSendQueue(endpoint.fd);  // the descriptor number may be a reused one

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

void FuchsiaEngine::UnregisterPosixEndpoint(PosixEndpointEntry& endpoint) noexcept
{
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD(IsOnCallbackThread());

    const auto found = std::find(poll_endpoints_.begin(), poll_endpoints_.end(), &endpoint);
    if (found != poll_endpoints_.end())
    {
        const auto i = static_cast<std::size_t>(std::distance(poll_endpoints_.begin(), found));
        UnpollEndpoint(i);
    }
}

void FuchsiaEngine::UnpollEndpoint(const std::size_t index) noexcept
{
    PosixEndpointEntry& endpoint = *poll_endpoints_[index];
    std::ignore = posix_endpoint_list_.erase(posix_endpoint_list_.iterator_to(endpoint));
    poll_endpoints_[index] = nullptr;
    DiscardSendQueue(endpoint.fd);
    poll_fds_[index] = {pipe_fds_[1], 0, 0};
    if (!endpoint.disconnect.empty())
    {
        endpoint.disconnect();
    }
}

void FuchsiaEngine::EnqueueCommand(CommandQueueEntry& entry,
                                   const TimePoint until,
                                   CommandCallback callback,
                                   const void* const owner) noexcept
{
    timer_queue_.RegisterTimedEntry(entry, until, std::move(callback), owner);
    SendPipeEvent(PipeEvent::TIMER);
}

void FuchsiaEngine::CleanUpOwner(const void* const owner) noexcept
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
        std::promise<void> done;
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

score::cpp::expected_blank<score::os::Error> FuchsiaEngine::SendProtocolMessage(
    const std::int32_t fd,
    std::uint8_t code,
    const score::cpp::span<const std::uint8_t> message) noexcept
{
    // frame: code, 16 bit little endian size, payload
    if (message.size() > std::numeric_limits<std::uint16_t>::max())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EMSGSIZE));
    }
    const auto size = static_cast<std::uint16_t>(message.size());
    score::cpp::pmr::vector<std::uint8_t> frame{memory_resource_};
    frame.reserve(sizeof(code) + sizeof(size) + message.size());
    frame.push_back(code);
    frame.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>(size >> 8U));
    frame.insert(frame.end(), message.begin(), message.end());

    // Writers never block: a frame is either fully accepted (written or queued) or fully rejected with EAGAIN, so
    // the byte stream always consists of whole frames. The queue is bounded by kMaxPendingBytes; on overflow the new
    // frame is dropped and reported to the caller, so that a stalled peer costs bounded memory and the connection
    // stays usable. Writes are serialized by the mutex, which is held only for non-blocking operations.
    std::lock_guard<std::mutex> guard{send_mutex_};
    EnsureNonBlocking(fd);
    auto& queue = send_queues_[fd];
    const auto pending = queue.bytes.size() - queue.head;
    if (pending > 0U)
    {
        if (pending + frame.size() > kMaxPendingBytes)
        {
            return score::cpp::make_unexpected(score::os::Error::createFromErrno(EAGAIN));
        }
        queue.bytes.insert(queue.bytes.end(), frame.begin(), frame.end());
        return {};
    }
    const auto written = WriteSome(fd, frame.data(), frame.size());
    if (!written.has_value())
    {
        std::ignore = send_queues_.erase(fd);
        return score::cpp::make_unexpected(written.error());
    }
    if (written.value() < frame.size())
    {
        // kMaxPendingBytes is larger than any frame, so the remainder always fits
        queue.bytes.assign(frame.begin() + static_cast<std::ptrdiff_t>(written.value()), frame.end());
        queue.head = 0U;
        SendPipeEvent(PipeEvent::TIMER);  // wake the engine thread to poll for POLLOUT
    }
    else
    {
        std::ignore = send_queues_.erase(fd);
    }
    return {};
}

void FuchsiaEngine::DiscardSendQueue(const std::int32_t fd) noexcept
{
    std::lock_guard<std::mutex> guard{send_mutex_};
    std::ignore = send_queues_.erase(fd);
}

void FuchsiaEngine::FlushSendQueue(const std::int32_t fd, const bool failed) noexcept
{
    std::lock_guard<std::mutex> guard{send_mutex_};
    const auto found = send_queues_.find(fd);
    if (found == send_queues_.end())
    {
        return;
    }
    auto& queue = found->second;
    const auto written =
        failed ? score::cpp::expected<std::size_t, score::os::Error>{score::cpp::make_unexpected(
                     score::os::Error::createFromErrno(EPIPE))}
               : WriteSome(fd, queue.bytes.data() + queue.head, queue.bytes.size() - queue.head);
    if (!written.has_value())
    {
        // the peer is gone; the reading side will notice the disconnect
        send_queues_.erase(found);
        return;
    }
    queue.head += written.value();
    if (queue.head == queue.bytes.size())
    {
        send_queues_.erase(found);
    }
}

void FuchsiaEngine::CollectPendingSendFds(score::cpp::pmr::vector<pollfd>& fds) noexcept
{
    std::lock_guard<std::mutex> guard{send_mutex_};
    for (const auto& entry : send_queues_)
    {
        if (entry.second.bytes.size() > entry.second.head)
        {
            // NOLINTNEXTLINE(hicpp-signed-bitwise): POLL* are non-negative POSIX constants and pollfd::events is signed
            std::ignore = fds.emplace_back(pollfd{entry.first, POLLOUT, 0});
        }
    }
}

score::cpp::expected<score::cpp::span<const std::uint8_t>, score::os::Error> FuchsiaEngine::ReceiveProtocolMessage(
    const std::int32_t fd,
    std::uint8_t& code) noexcept
{
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
}

void FuchsiaEngine::SendPipeEvent(PipeEvent pipe_event) noexcept
{
    std::ignore = os_resources_.unistd->write(pipe_fds_[1], &pipe_event, sizeof(pipe_event));
}

void FuchsiaEngine::ProcessPipeEvent() noexcept
{
    PipeEvent pipe_event{PipeEvent::QUIT};
    std::ignore = os_resources_.unistd->read(pipe_fds_[0], &pipe_event, sizeof(pipe_event));
    if (pipe_event != PipeEvent::TIMER)
    {
        quit_flag_ = true;
    }
    // for TIMER, just wake up to recalculate the poll timeout
}

void FuchsiaEngine::ProcessCleanup(const void* const owner) noexcept
{
    for (std::size_t i = 0; i < poll_fds_.size(); ++i)
    {
        if ((poll_endpoints_[i] != nullptr) && (poll_endpoints_[i]->owner == owner))
        {
            UnpollEndpoint(i);
        }
    }
    timer_queue_.CleanUpOwner(owner);
}

void FuchsiaEngine::RunOnThread() noexcept
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
        const std::int32_t timeout = ProcessTimerQueue();
        // connections with queued output are additionally polled for POLLOUT, even if not registered as endpoints
        const auto endpoint_count = poll_fds_.size();
        poll_scratch_.assign(poll_fds_.begin(), poll_fds_.end());
        CollectPendingSendFds(poll_scratch_);
        const auto num_expected = os_resources_.poll->poll(poll_scratch_.data(), poll_scratch_.size(), timeout);
        if ((num_expected.has_value()) && (num_expected.value() > 0))
        {
            for (std::size_t i = endpoint_count; i < poll_scratch_.size(); ++i)
            {
                if (poll_scratch_[i].revents != 0)
                {
                    // NOLINTNEXTLINE(hicpp-signed-bitwise): POLL* are non-negative POSIX constants
                    FlushSendQueue(poll_scratch_[i].fd, (poll_scratch_[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0);
                }
            }
            for (std::size_t i = 0; i < endpoint_count; ++i)
            {
                // an endpoint may get unregistered by a previous callback in this very iteration
                if ((poll_scratch_[i].revents != 0) && (poll_endpoints_[i] != nullptr))
                {
                    poll_endpoints_[i]->input();
                }
            }
        }
    }

    UnregisterPosixEndpoint(command_endpoint_);
}

std::int32_t FuchsiaEngine::ProcessTimerQueue() noexcept
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
    return static_cast<std::int32_t>(distance_ms + 1);
}

}  // namespace score::message_passing
