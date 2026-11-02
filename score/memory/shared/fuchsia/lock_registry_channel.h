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
#ifndef SCORE_LIB_MEMORY_SHARED_FUCHSIA_LOCK_REGISTRY_CHANNEL_H
#define SCORE_LIB_MEMORY_SHARED_FUCHSIA_LOCK_REGISTRY_CHANNEL_H

#include "score/memory/shared/lock_provider/i_lock_registry.h"

#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/port.h>

#include <map>
#include <memory>
#include <thread>

namespace score::memory::shared::fuchsia
{

/// \brief Serves an ILockRegistry to one remote client over a channel (stand-in for the future broker).
/// \details Every granted lock is bound to an eventpair: the server keeps one end, the client the other. As soon as the
///          client closes its end, or its process dies and the kernel closes it, the server destroys the lock token and
///          so releases the lock. There is no pid and no file involved. Runs its own thread; destruction stops it and
///          releases everything that was granted. One server per client channel, the backing registry may be shared.
class LockRegistryServer
{
  public:
    LockRegistryServer(std::shared_ptr<ILockRegistry> backing, zx::channel server_end);
    ~LockRegistryServer();
    LockRegistryServer(const LockRegistryServer&) = delete;
    LockRegistryServer& operator=(const LockRegistryServer&) = delete;
    LockRegistryServer(LockRegistryServer&&) = delete;
    LockRegistryServer& operator=(LockRegistryServer&&) = delete;

  private:
    struct Held;

    void Run();
    bool DrainChannel();
    void Handle(const void* message, std::uint32_t size);

    std::shared_ptr<ILockRegistry> backing_;
    zx::channel channel_;
    zx::port port_;
    std::map<std::uint64_t, Held> held_;
    std::uint64_t next_key_;
    std::thread thread_;
};

/// \brief ILockRegistry that forwards to a LockRegistryServer. Locks are tokens that own the client end of an eventpair.
/// \details A failing transport (server gone) is reported as EIO from every operation; acquisitions then fail instead
///          of silently granting a lock.
class ChannelLockRegistryClient final : public ILockRegistry
{
  public:
    explicit ChannelLockRegistryClient(zx::channel client_end) noexcept;

    score::cpp::expected_blank<score::os::Error> CreateName(const std::string& name,
                                                            bool fail_if_exists) noexcept override;
    bool NameExists(const std::string& name) noexcept override;
    void RemoveName(const std::string& name) noexcept override;
    score::cpp::expected<std::unique_ptr<ILockToken>, score::os::Error> TryAcquire(const std::string& name,
                                                                                   LockKind kind) noexcept override;

  private:
    score::cpp::expected<std::int32_t, score::os::Error> Call(std::uint32_t op,
                                                              std::uint32_t arg,
                                                              const std::string& name,
                                                              zx::eventpair* handle) noexcept;

    zx::channel channel_;
};

}  // namespace score::memory::shared::fuchsia

#endif  // SCORE_LIB_MEMORY_SHARED_FUCHSIA_LOCK_REGISTRY_CHANNEL_H
