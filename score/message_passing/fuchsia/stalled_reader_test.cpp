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

#include <lib/fdio/fd.h>
#include <lib/zx/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

using namespace score::message_passing;

// Several threads send through one connection whose peer does not read at all. The senders must never block, the
// overflow must be reported as EAGAIN, and once the peer reads, exactly the accepted frames must arrive intact.

namespace
{

constexpr std::uint8_t kThreads = 4U;
constexpr std::uint16_t kMessagesPerThread = 400U;
constexpr auto kMaxSendDuration = std::chrono::milliseconds{250};

std::vector<std::uint8_t> MakePayload(const std::uint8_t sender, const std::uint16_t seq)
{
    const std::size_t length = 2U + (static_cast<std::size_t>(seq) * 37U) % 2000U;
    std::vector<std::uint8_t> payload(length, static_cast<std::uint8_t>(seq * 7U + sender));
    payload[0] = static_cast<std::uint8_t>(seq & 0xFFU);
    payload[1] = static_cast<std::uint8_t>(seq >> 8U);
    return payload;
}

bool ReadExactly(const int fd, std::uint8_t* data, std::size_t size)
{
    while (size > 0U)
    {
        pollfd wait{fd, POLLIN, 0};
        if (::poll(&wait, 1, 5000) <= 0)
        {
            std::cout << "timeout waiting for data" << std::endl;
            return false;
        }
        const auto count = ::read(fd, data, size);
        if (count <= 0)
        {
            return false;
        }
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace

int main()
{
    FuchsiaEngine engine{score::cpp::pmr::get_default_resource()};

    zx::socket writer_end;
    zx::socket reader_end;
    int writer_fd{-1};
    int reader_fd{-1};
    if (zx::socket::create(ZX_SOCKET_STREAM, &writer_end, &reader_end) != ZX_OK ||
        fdio_fd_create(writer_end.release(), &writer_fd) != ZX_OK ||
        fdio_fd_create(reader_end.release(), &reader_fd) != ZX_OK)
    {
        std::cout << "socket setup failed" << std::endl;
        return 1;
    }

    std::array<std::vector<std::uint16_t>, kThreads> accepted;
    std::array<std::uint32_t, kThreads> rejected{};
    std::array<std::chrono::nanoseconds, kThreads> slowest{};
    std::array<bool, kThreads> unexpected_error{};

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> senders;
    for (std::uint8_t t = 0U; t < kThreads; ++t)
    {
        senders.emplace_back([&, t]() {
            for (std::uint16_t seq = 0U; seq < kMessagesPerThread; ++seq)
            {
                const auto payload = MakePayload(t, seq);
                const auto before = std::chrono::steady_clock::now();
                const auto result = engine.SendProtocolMessage(writer_fd, t, payload);
                slowest[t] = std::max(slowest[t],
                                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - before));
                if (result.has_value())
                {
                    accepted[t].push_back(seq);
                }
                else if (result.error() == score::os::Error::createFromErrno(EAGAIN))
                {
                    ++rejected[t];
                }
                else
                {
                    unexpected_error[t] = true;
                }
            }
        });
    }
    for (auto& sender : senders)
    {
        sender.join();
    }
    const auto total_time = std::chrono::steady_clock::now() - start;

    std::size_t total_accepted = 0U;
    std::uint32_t total_rejected = 0U;
    for (std::uint8_t t = 0U; t < kThreads; ++t)
    {
        total_accepted += accepted[t].size();
        total_rejected += rejected[t];
        if (unexpected_error[t] || slowest[t] > kMaxSendDuration)
        {
            std::cout << "sender " << int{t} << " failed: unexpected error " << unexpected_error[t] << ", slowest "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(slowest[t]).count() << " ms"
                      << std::endl;
            return 2;
        }
    }
    std::cout << "stalled phase took " << std::chrono::duration_cast<std::chrono::milliseconds>(total_time).count()
              << " ms, accepted " << total_accepted << ", rejected " << total_rejected << std::endl;
    if (total_rejected == 0U || total_accepted == 0U)
    {
        std::cout << "expected both accepted and rejected frames" << std::endl;
        return 3;
    }

    // the peer starts reading: the queue must be flushed and contain exactly the accepted frames, each intact
    std::array<std::size_t, kThreads> next{};
    for (std::size_t i = 0U; i < total_accepted; ++i)
    {
        std::array<std::uint8_t, 3> header{};
        if (!ReadExactly(reader_fd, header.data(), header.size()))
        {
            std::cout << "failed to read header of frame " << i << std::endl;
            return 4;
        }
        const std::uint8_t sender = header[0];
        const std::size_t size = static_cast<std::size_t>(header[1]) | (static_cast<std::size_t>(header[2]) << 8U);
        std::vector<std::uint8_t> payload(size);
        if (sender >= kThreads || !ReadExactly(reader_fd, payload.data(), size))
        {
            std::cout << "corrupt frame header at " << i << std::endl;
            return 5;
        }
        if (next[sender] >= accepted[sender].size() ||
            payload != MakePayload(sender, accepted[sender][next[sender]]))
        {
            std::cout << "corrupt or out of order frame " << i << " from sender " << int{sender} << std::endl;
            return 6;
        }
        ++next[sender];
    }
    pollfd extra{reader_fd, POLLIN, 0};
    if (::poll(&extra, 1, 200) != 0)
    {
        std::cout << "unexpected extra data after the accepted frames" << std::endl;
        return 7;
    }

    // after draining, the connection accepts frames again
    const std::array<std::uint8_t, 2> tail{0xAB, 0xCD};
    if (!engine.SendProtocolMessage(writer_fd, 9, tail).has_value())
    {
        std::cout << "send after drain failed" << std::endl;
        return 8;
    }
    std::array<std::uint8_t, 5> tail_frame{};
    if (!ReadExactly(reader_fd, tail_frame.data(), tail_frame.size()) || tail_frame[0] != 9 || tail_frame[1] != 2 ||
        tail_frame[2] != 0 || tail_frame[3] != 0xAB || tail_frame[4] != 0xCD)
    {
        std::cout << "frame after drain corrupt" << std::endl;
        return 9;
    }

    engine.CloseClientConnection(writer_fd);
    ::close(reader_fd);
    std::cout << "stalled reader OK" << std::endl;
    return 0;
}
