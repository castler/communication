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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_VMO_HANDLE_CLIENT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_VMO_HANDLE_CLIENT_H

#include "score/memory/shared/fuchsia/i_vmo_handle_provider.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_role.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/vmo.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Proxy side of Attach: an IVmoHandleProvider whose Acquire() fetches the handles over the routed
///        `score.mw.com.Endpoint` of the skeleton.
/// \details The first Acquire() performs the (blocking) Attach call, the data and the control handle arrive together
///          and are cached; each handle is handed out once. The proxy sends no identity, the skeleton derives it from
///          the endpoint. Errors are mapped to errno values: ENOENT NOT_FOUND / no such role, EACCES NOT_ALLOWED,
///          EPROTO INCOMPATIBLE_VERSION, ECONNRESET the channel is closed (skeleton gone *or* the endpoint is not
///          routed to this component, which is indistinguishable by design, see README A.1).
class FidlVmoHandleClient final : public score::memory::shared::fuchsia::IVmoHandleProvider
{
  public:
    struct AttachInfo
    {
        std::uint64_t epoch;
        std::uint64_t application_id;
        std::vector<std::uint8_t> layout;
        /// Rights of the handles exactly as they arrived (before anyone restricted them further).
        zx_rights_t data_rights;
        zx_rights_t control_rights;
        zx_rights_t liveness_rights;
    };

    explicit FidlVmoHandleClient(fidl::ClientEnd<score_mw_com::Endpoint> endpoint,
                                 KeyClassifier classifier = DefaultKeyClassifier);
    ~FidlVmoHandleClient() override;

    score::cpp::expected<zx::vmo, score::os::Error> Acquire(const std::string& key) noexcept override;
    /// \brief Not available on the proxy side (EPERM).
    score::cpp::expected_blank<score::os::Error> Publish(const std::string& key,
                                                         zx::vmo consumer_handle) noexcept override;
    void Withdraw(const std::string& key) noexcept override;

    /// \brief Outcome of the Attach call: ZX_OK, the transport status (e.g. ZX_ERR_PEER_CLOSED) or ZX_ERR_NOT_FOUND-like
    ///        domain errors reported through attach_error(). nullopt before the first Acquire().
    std::optional<zx_status_t> transport_status() const noexcept;
    std::optional<score_mw_com::AttachError> attach_error() const noexcept;
    const std::optional<AttachInfo>& attach_info() const noexcept;
    /// \brief Peer-closed on it means that the skeleton is gone.
    const zx::eventpair& liveness() const noexcept;

  private:
    zx_status_t EnsureAttached();

    fidl::SyncClient<score_mw_com::Endpoint> client_;
    KeyClassifier classifier_;
    std::mutex mutex_;
    bool attach_done_{false};
    std::optional<zx_status_t> transport_status_;
    std::optional<score_mw_com::AttachError> attach_error_;
    std::optional<AttachInfo> info_;
    std::map<VmoRole, zx::vmo> handles_;
    zx::eventpair liveness_;
    fidl::ClientEnd<score_mw_com::EventChannel> events_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FIDL_VMO_HANDLE_CLIENT_H
