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
#include "score/memory/shared/fuchsia/lock_registry_channel.h"

#include "score/mw/log/logging.h"

#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>
#include <zircon/types.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace score::memory::shared::fuchsia
{

namespace
{

enum Op : std::uint32_t
{
    kCreateName = 1U,
    kNameExists = 2U,
    kRemoveName = 3U,
    kTryAcquire = 4U
};

struct RequestHeader
{
    zx_txid_t txid;
    std::uint32_t op;
    std::uint32_t arg;
    std::uint32_t name_size;
};

struct Reply
{
    zx_txid_t txid;
    std::int32_t error;
};

constexpr std::uint32_t kMaxNameSize = 1024U;
constexpr std::uint32_t kMaxRequestSize = static_cast<std::uint32_t>(sizeof(RequestHeader)) + kMaxNameSize;
constexpr std::uint64_t kChannelKey = 0U;
constexpr std::uint64_t kFirstHeldKey = 1U;

constexpr zx_time_t kCallTimeout = ZX_SEC(10);

class EventpairToken final : public ILockToken
{
  public:
    explicit EventpairToken(zx::eventpair handle) noexcept : handle_{std::move(handle)} {}

  private:
    zx::eventpair handle_;
};

score::os::Error TransportError() noexcept
{
    return score::os::Error::createFromErrno(EIO);
}

}  // namespace

struct LockRegistryServer::Held
{
    std::unique_ptr<ILockToken> token;
    zx::eventpair server_end;
};

LockRegistryServer::LockRegistryServer(std::shared_ptr<ILockRegistry> backing, zx::channel server_end)
    : backing_{std::move(backing)}, channel_{std::move(server_end)}, port_{}, held_{}, next_key_{kFirstHeldKey}, thread_{}
{
    if (zx::port::create(0U, &port_) != ZX_OK ||
        channel_.wait_async(port_, kChannelKey, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, 0U) != ZX_OK)
    {
        score::mw::log::LogError("shm") << "LockRegistryServer: could not set up the port";
        return;
    }
    thread_ = std::thread{[this] {
        Run();
    }};
}

LockRegistryServer::~LockRegistryServer()
{
    if (thread_.joinable())
    {
        zx_port_packet_t stop{};
        stop.type = ZX_PKT_TYPE_USER;
        static_cast<void>(port_.queue(&stop));
        thread_.join();
    }
}

void LockRegistryServer::Run()
{
    while (true)
    {
        zx_port_packet_t packet{};
        if (port_.wait(zx::time::infinite(), &packet) != ZX_OK || packet.type == ZX_PKT_TYPE_USER)
        {
            return;
        }
        if (packet.key == kChannelKey)
        {
            const bool peer_closed = (packet.signal.observed & ZX_CHANNEL_PEER_CLOSED) != 0U;
            const bool alive = DrainChannel();
            if (!alive || peer_closed ||
                channel_.wait_async(port_, kChannelKey, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, 0U) != ZX_OK)
            {
                return;
            }
        }
        else
        {
            // The client end of the eventpair is gone (released, or the client died): release the lock.
            held_.erase(packet.key);
        }
    }
}

bool LockRegistryServer::DrainChannel()
{
    std::array<std::uint8_t, kMaxRequestSize> buffer{};
    while (true)
    {
        std::uint32_t actual_bytes{};
        const zx_status_t status = channel_.read(0U, buffer.data(), nullptr, buffer.size(), 0U, &actual_bytes, nullptr);
        if (status == ZX_ERR_SHOULD_WAIT)
        {
            return true;
        }
        if (status != ZX_OK)
        {
            return false;
        }
        Handle(buffer.data(), actual_bytes);
    }
}

void LockRegistryServer::Handle(const void* const message, const std::uint32_t size)
{
    RequestHeader header{};
    if (size < sizeof(header))
    {
        return;
    }
    std::memcpy(&header, message, sizeof(header));
    Reply reply{header.txid, 0};
    zx::eventpair handle_for_client;

    if (header.name_size > kMaxNameSize || size != sizeof(header) + header.name_size)
    {
        reply.error = EINVAL;
    }
    else
    {
        const std::string name{static_cast<const char*>(message) + sizeof(header), header.name_size};
        switch (header.op)
        {
            case kCreateName:
            {
                const auto result = backing_->CreateName(name, header.arg != 0U);
                reply.error = result.has_value() ? 0 : result.error().GetOsDependentErrorCode();
                break;
            }
            case kNameExists:
                reply.error = backing_->NameExists(name) ? 0 : ENOENT;
                break;
            case kRemoveName:
                backing_->RemoveName(name);
                break;
            case kTryAcquire:
            {
                auto result = backing_->TryAcquire(name, header.arg != 0U ? LockKind::kExclusive : LockKind::kShared);
                if (!result.has_value())
                {
                    reply.error = result.error().GetOsDependentErrorCode();
                    break;
                }
                if (result.value() == nullptr)
                {
                    break;
                }
                zx::eventpair server_end;
                const std::uint64_t key = next_key_++;
                if (zx::eventpair::create(0U, &server_end, &handle_for_client) != ZX_OK ||
                    server_end.wait_async(port_, key, ZX_EVENTPAIR_PEER_CLOSED, 0U) != ZX_OK)
                {
                    reply.error = ENOMEM;
                    handle_for_client.reset();
                    break;
                }
                held_[key] = Held{std::move(result).value(), std::move(server_end)};
                break;
            }
            default:
                reply.error = ENOSYS;
                break;
        }
    }

    zx_handle_t handle = handle_for_client.release();
    const std::uint32_t handle_count = handle != ZX_HANDLE_INVALID ? 1U : 0U;
    // The kernel consumes the handle even if the write fails; the closed peer then releases the lock via the port.
    static_cast<void>(channel_.write(0U, &reply, sizeof(reply), &handle, handle_count));
}

ChannelLockRegistryClient::ChannelLockRegistryClient(zx::channel client_end) noexcept : channel_{std::move(client_end)} {}

auto ChannelLockRegistryClient::Call(const std::uint32_t op,
                                     const std::uint32_t arg,
                                     const std::string& name,
                                     zx::eventpair* const handle) noexcept
    -> score::cpp::expected<std::int32_t, score::os::Error>
{
    if (name.size() > kMaxNameSize)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENAMETOOLONG));
    }
    std::array<std::uint8_t, kMaxRequestSize> request{};
    const RequestHeader header{0U, op, arg, static_cast<std::uint32_t>(name.size())};
    std::memcpy(request.data(), &header, sizeof(header));
    std::memcpy(request.data() + sizeof(header), name.data(), name.size());

    Reply reply{};
    zx_handle_t reply_handle = ZX_HANDLE_INVALID;
    zx_channel_call_args_t args{};
    args.wr_bytes = request.data();
    args.wr_num_bytes = static_cast<std::uint32_t>(sizeof(header) + name.size());
    args.rd_bytes = &reply;
    args.rd_num_bytes = sizeof(reply);
    args.rd_handles = &reply_handle;
    args.rd_num_handles = 1U;
    std::uint32_t actual_bytes{};
    std::uint32_t actual_handles{};
    const zx_status_t status = channel_.call(0U, zx::deadline_after(zx::duration{kCallTimeout}), &args, &actual_bytes, &actual_handles);
    if (status != ZX_OK || actual_bytes != sizeof(reply))
    {
        if (status == ZX_OK && actual_handles == 1U)
        {
            zx_handle_close(reply_handle);
        }
        return score::cpp::make_unexpected(TransportError());
    }
    if (actual_handles == 1U && handle != nullptr)
    {
        handle->reset(reply_handle);
    }
    else if (actual_handles == 1U)
    {
        zx_handle_close(reply_handle);
    }
    return reply.error;
}

auto ChannelLockRegistryClient::CreateName(const std::string& name, const bool fail_if_exists) noexcept
    -> score::cpp::expected_blank<score::os::Error>
{
    const auto result = Call(kCreateName, fail_if_exists ? 1U : 0U, name, nullptr);
    if (!result.has_value())
    {
        return score::cpp::make_unexpected(result.error());
    }
    if (result.value() != 0)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(result.value()));
    }
    return {};
}

bool ChannelLockRegistryClient::NameExists(const std::string& name) noexcept
{
    const auto result = Call(kNameExists, 0U, name, nullptr);
    return result.has_value() && result.value() == 0;
}

void ChannelLockRegistryClient::RemoveName(const std::string& name) noexcept
{
    static_cast<void>(Call(kRemoveName, 0U, name, nullptr));
}

auto ChannelLockRegistryClient::TryAcquire(const std::string& name, const LockKind kind) noexcept
    -> score::cpp::expected<std::unique_ptr<ILockToken>, score::os::Error>
{
    zx::eventpair handle;
    const auto result = Call(kTryAcquire, kind == LockKind::kExclusive ? 1U : 0U, name, &handle);
    if (!result.has_value())
    {
        return score::cpp::make_unexpected(result.error());
    }
    if (result.value() != 0)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(result.value()));
    }
    if (!handle.is_valid())
    {
        return std::unique_ptr<ILockToken>{};
    }
    return std::unique_ptr<ILockToken>{std::make_unique<EventpairToken>(std::move(handle))};
}

}  // namespace score::memory::shared::fuchsia
