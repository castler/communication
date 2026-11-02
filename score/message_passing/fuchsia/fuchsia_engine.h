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
#ifndef SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_ENGINE_H
#define SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_ENGINE_H

#include <score/callback.hpp>
#include <score/memory.hpp>
#include <score/span.hpp>
#include <score/vector.hpp>

#include "score/message_passing/i_shared_resource_engine.h"
#include "score/message_passing/timed_command_queue.h"
#include "score/os/sys_poll.h"
#include "score/os/unistd.h"

#include <array>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>

namespace score::message_passing
{

/// \brief Engine of the Fuchsia message_passing binding.
/// \details Connections are Zircon stream sockets. They are established through the score.messagepassing.Connector
///          FIDL protocol (see FuchsiaConnectionRegistry) and then used as plain descriptors: a background thread
///          polls them and a simple framing protocol (code, size, payload) is exchanged over them.
///          The class is shared via std::shared_ptr between the client and server factories, the servers and the
///          client connections.
class FuchsiaEngine final : public ISharedResourceEngine
{
  public:
    struct OsResources
    {
        score::cpp::pmr::unique_ptr<score::os::SysPoll> poll{};
        score::cpp::pmr::unique_ptr<score::os::Unistd> unistd{};
    };

    FuchsiaEngine(score::cpp::pmr::memory_resource* memory_resource, LoggingCallback logger = GetCerrLogger()) noexcept;
    ~FuchsiaEngine() noexcept override;

    FuchsiaEngine(const FuchsiaEngine&) = delete;
    FuchsiaEngine& operator=(const FuchsiaEngine&) = delete;

    static OsResources GetDefaultOsResources(score::cpp::pmr::memory_resource* const memory_resource) noexcept
    {
        return {score::os::SysPoll::Default(memory_resource), score::os::Unistd::Default(memory_resource)};
    }

    score::cpp::pmr::memory_resource* GetMemoryResource() noexcept override
    {
        return memory_resource_;
    }
    OsResources& GetOsResources() & noexcept
    {
        return os_resources_;
    }
    const LoggingCallback& GetLogger() & noexcept override
    {
        return logger_;
    }

    using FinalizeOwnerCallback = score::cpp::callback<void() /* noexcept */>;

    score::cpp::expected<std::int32_t, score::os::Error> TryOpenClientConnection(
        std::string_view identifier) noexcept override;

    void CloseClientConnection(std::int32_t client_fd) noexcept override;

    void RegisterPosixEndpoint(PosixEndpointEntry& endpoint) noexcept override;
    void UnregisterPosixEndpoint(PosixEndpointEntry& endpoint) noexcept override;

    void EnqueueCommand(CommandQueueEntry& entry,
                        const TimePoint until,
                        CommandCallback callback,
                        const void* const owner) noexcept override;

    // this call is blocking
    void CleanUpOwner(const void* const owner) noexcept override;

    // Never blocks and is safe to call concurrently. A frame is either accepted as a whole or rejected with EAGAIN
    // when the bounded per-connection queue of a slow peer is full; the connection stays open and frame-aligned.
    score::cpp::expected_blank<score::os::Error> SendProtocolMessage(
        const std::int32_t fd,
        std::uint8_t code,
        const score::cpp::span<const std::uint8_t> message) noexcept override;
    score::cpp::expected<score::cpp::span<const std::uint8_t>, score::os::Error> ReceiveProtocolMessage(
        const std::int32_t fd,
        std::uint8_t& code) noexcept override;

    bool IsOnCallbackThread() const noexcept override
    {
        return std::this_thread::get_id() == thread_.get_id();
    }

  private:
    enum class PipeEvent : uint8_t
    {
        QUIT,
        TIMER
    };

    struct CleanupCommand
    {
        FinalizeOwnerCallback callback;
        const void* owner;
    };

    struct SendQueue
    {
        std::vector<std::uint8_t> bytes;
        std::size_t head{0U};
    };

    // upper bound of unsent bytes per connection; must exceed the largest frame (3 + 65535 bytes)
    static constexpr std::size_t kMaxPendingBytes{128U * 1024U};

    void DiscardSendQueue(const std::int32_t fd) noexcept;
    void FlushSendQueue(const std::int32_t fd, const bool failed) noexcept;
    void CollectPendingSendFds(score::cpp::pmr::vector<pollfd>& fds) noexcept;
    void UnpollEndpoint(const std::size_t index) noexcept;
    void SendPipeEvent(PipeEvent pipe_event) noexcept;
    void ProcessPipeEvent() noexcept;
    void ProcessCleanup(const void* const owner) noexcept;
    void RunOnThread() noexcept;
    std::int32_t ProcessTimerQueue() noexcept;

    score::cpp::pmr::memory_resource* const memory_resource_;
    OsResources os_resources_;
    LoggingCallback logger_;

    std::array<std::int32_t, 2> pipe_fds_{};
    bool quit_flag_;
    std::thread thread_;
    std::mutex thread_mutex_;
    ISharedResourceEngine::PosixEndpointEntry command_endpoint_;

    // poll() on Fuchsia reports POLLNVAL for negative descriptors instead of ignoring them. Free slots are therefore
    // identified by a null endpoint and parked on the write end of the wake-up pipe, which never becomes ready.
    score::cpp::pmr::vector<pollfd> poll_fds_;
    score::cpp::pmr::vector<PosixEndpointEntry*> poll_endpoints_;
    score::cpp::pmr::vector<pollfd> poll_scratch_;

    std::mutex send_mutex_;
    std::map<std::int32_t, SendQueue> send_queues_;

    detail::TimedCommandQueue timer_queue_;
    score::containers::intrusive_list<PosixEndpointEntry> posix_endpoint_list_;
    score::cpp::pmr::vector<std::uint8_t> posix_receive_buffer_;
};

}  // namespace score::message_passing

#endif  // SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_ENGINE_H
