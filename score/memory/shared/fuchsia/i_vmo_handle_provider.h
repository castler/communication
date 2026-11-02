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
#ifndef SCORE_LIB_MEMORY_SHARED_FUCHSIA_I_VMO_HANDLE_PROVIDER_H
#define SCORE_LIB_MEMORY_SHARED_FUCHSIA_I_VMO_HANDLE_PROVIDER_H

#include "score/os/errno.h"

#include <lib/zx/vmo.h>
#include <score/expected.hpp>

#include <string>

namespace score::memory::shared::fuchsia
{

/// \brief Out-of-band transport for the VMO handles that back a shared-memory resource on Fuchsia.
/// \details Fuchsia has no named shared memory: a region is only reachable through a zx handle which has to be
///          transferred to the consumer (e.g. over a FIDL channel or as a startup handle). The factory does not care
///          how that happens, it only talks to this interface. The key is the path of a named region, or
///          "anon:<id>" for an anonymous one.
class IVmoHandleProvider
{
  public:
    IVmoHandleProvider() noexcept = default;
    virtual ~IVmoHandleProvider() = default;

    /// \brief Proxy side: yields the handle for `key`. Ownership moves to the caller.
    /// \return ENOENT if no such region is known (yet).
    virtual score::cpp::expected<zx::vmo, score::os::Error> Acquire(const std::string& key) noexcept = 0;

    /// \brief Skeleton side: offers the consumer handle of a freshly created region (reduced rights plus TRANSFER so it
    ///        can be sent, plus DUPLICATE so the provider can hand out one duplicate per consumer, see
    ///        VmoSharedMemoryResource::DuplicatePublishHandle; the provider must not let DUPLICATE leave the process).
    ///        Ownership moves to the provider.
    virtual score::cpp::expected_blank<score::os::Error> Publish(const std::string& key,
                                                                 zx::vmo consumer_handle) noexcept = 0;

    /// \brief Skeleton side: stops offering `key` (the counterpart of unlinking a named region on Linux).
    virtual void Withdraw(const std::string& key) noexcept = 0;

  protected:
    IVmoHandleProvider(const IVmoHandleProvider&) noexcept = default;
    IVmoHandleProvider(IVmoHandleProvider&&) noexcept = default;
    IVmoHandleProvider& operator=(const IVmoHandleProvider&) noexcept = default;
    IVmoHandleProvider& operator=(IVmoHandleProvider&&) noexcept = default;
};

}  // namespace score::memory::shared::fuchsia

#endif  // SCORE_LIB_MEMORY_SHARED_FUCHSIA_I_VMO_HANDLE_PROVIDER_H
