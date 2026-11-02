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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_VMO_HANDLE_SERVER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_VMO_HANDLE_SERVER_H

#include "score/memory/shared/fuchsia/i_vmo_handle_provider.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_role.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/vmo.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Skeleton side of Attach: an IVmoHandleProvider that serves the published handles of one service instance
///        over the `score.mw.com.Endpoint` protocol.
/// \details Install it in a VmoSharedMemoryFactory: Create() publishes the data and the control region (classified by
///          key, see vmo_role.h) and every Attach() on a served endpoint then sends the proxy one duplicate of each
///          with exactly the rights declared in the FIDL file. The provider keeps the full set of originals, so
///          Attach can be called by any number of proxies.
///
///          Identity: a handler is created per routed endpoint, with the application id that endpoint stands for
///          (generated from the realm). Nothing the caller sends is used for identity.
///
///          Threading: Publish/Withdraw/setters may be called from any thread; Attach runs on `dispatcher`. The object
///          must outlive the bindings, i.e. the dispatcher, created from it.
class FidlVmoHandleServer final : public score::memory::shared::fuchsia::IVmoHandleProvider
{
  public:
    explicit FidlVmoHandleServer(async_dispatcher_t* dispatcher, KeyClassifier classifier = DefaultKeyClassifier);
    ~FidlVmoHandleServer() override;

    /// \brief Not used on the skeleton side.
    score::cpp::expected<zx::vmo, score::os::Error> Acquire(const std::string& key) noexcept override;
    score::cpp::expected_blank<score::os::Error> Publish(const std::string& key,
                                                         zx::vmo consumer_handle) noexcept override;
    void Withdraw(const std::string& key) noexcept override;

    /// \brief Skeleton incarnation, returned by Attach and compared by the proxy with the one from discovery.
    void SetEpoch(const std::uint64_t epoch) noexcept;
    /// \brief Opaque layout metadata returned by Attach (at most MAX_LAYOUT_SIZE bytes).
    void SetLayout(std::vector<std::uint8_t> layout) noexcept;
    /// \brief strict_permissions: when set, Attach on an endpoint of any other application id is refused with
    ///        NOT_ALLOWED (defence in depth against a mis-generated realm).
    void SetAllowedApplications(std::optional<std::set<std::uint64_t>> allowed) noexcept;

    /// \brief Handler to hand to OutgoingDirectory::AddProtocol for the endpoint of `application_id`.
    fidl::ProtocolHandler<score_mw_com::Endpoint> CreateHandler(const std::uint64_t application_id);

    /// \brief Event notification: call after the slot was published in the data region (store, release-store of the
    ///        index, then this). Signals USER_0 on every subscriber, never blocks and never waits for a subscriber;
    ///        subscribers whose notification handle is closed are unsubscribed. Thread-safe. Returns the number of
    ///        subscribers signalled.
    std::size_t NotifySubscribers() noexcept;
    /// \brief Number of live subscribers (sessions whose proxy still holds its notification handle).
    std::size_t SubscriberCount() noexcept;

    /// \brief Number of successful Attach calls on the endpoint of `application_id` / of all endpoints.
    std::size_t AttachCount(const std::uint64_t application_id) const noexcept;
    std::size_t AttachCount() const noexcept;

  private:
    class Connection;
    struct State;

    async_dispatcher_t* dispatcher_;
    std::shared_ptr<State> state_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_VMO_HANDLE_SERVER_H
