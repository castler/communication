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
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"

#include "score/mw/log/logging.h"

#include <zircon/syscalls.h>

#include <cerrno>
#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr const char* kLogContext = "lola_fidl";

template <typename Object>
zx_rights_t RightsOf(const Object& object) noexcept
{
    zx_info_handle_basic_t info{};
    if (object.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr) != ZX_OK)
    {
        return 0U;
    }
    return info.rights;
}

int ErrnoFor(const score_mw_com::AttachError error) noexcept
{
    if (error == score_mw_com::AttachError::kNotAllowed)
    {
        return EACCES;
    }
    if (error == score_mw_com::AttachError::kInsufficientIntegrity)
    {
        return EPERM;
    }
    if (error == score_mw_com::AttachError::kIncompatibleVersion)
    {
        return EPROTO;
    }
    return ENOENT;
}

}  // namespace

FidlVmoHandleClient::FidlVmoHandleClient(fidl::ClientEnd<score_mw_com::Endpoint> endpoint, KeyClassifier classifier)
    : IVmoHandleProvider{}, client_{std::move(endpoint)}, classifier_{std::move(classifier)}
{
}

FidlVmoHandleClient::~FidlVmoHandleClient() = default;

auto FidlVmoHandleClient::EnsureAttached() -> zx_status_t
{
    if (attach_done_)
    {
        return transport_status_.value_or(ZX_ERR_INTERNAL);
    }
    attach_done_ = true;

    auto result = client_->Attach();
    if (result.is_error())
    {
        if (result.error_value().is_framework_error())
        {
            transport_status_ = result.error_value().framework_error().status();
            score::mw::log::LogWarn(kLogContext) << "Attach failed on the transport: " << *transport_status_;
        }
        else
        {
            transport_status_ = ZX_OK;
            attach_error_ = result.error_value().domain_error();
        }
        return *transport_status_;
    }

    auto& response = result.value();
    transport_status_ = ZX_OK;
    info_ = AttachInfo{response.epoch(),
                       response.application_id(),
                       response.layout(),
                       RightsOf(response.data_vmo()),
                       RightsOf(response.control_vmo()),
                       RightsOf(response.liveness())};
    handles_.emplace(VmoRole::kData, std::move(response.data_vmo()));
    handles_.emplace(VmoRole::kControl, std::move(response.control_vmo()));
    if (response.control_asil_b_vmo().is_valid())
    {
        handles_.emplace(VmoRole::kControlAsilB, std::move(response.control_asil_b_vmo()));
    }
    liveness_ = std::move(response.liveness());
    notification_ = std::move(response.notification());
    return ZX_OK;
}

auto FidlVmoHandleClient::Attach() noexcept -> zx_status_t
{
    const std::lock_guard<std::mutex> lock{mutex_};
    const zx_status_t status = EnsureAttached();
    if (status != ZX_OK)
    {
        return status;
    }
    return attach_error_.has_value() ? ZX_ERR_ACCESS_DENIED : ZX_OK;
}

auto FidlVmoHandleClient::WaitForNotification(const zx::time deadline) noexcept -> zx_status_t
{
    if (!notification_.is_valid())
    {
        return ZX_ERR_BAD_STATE;
    }
    zx_signals_t observed{0U};
    const zx_status_t status =
        notification_.wait_one(ZX_USER_SIGNAL_0 | ZX_EVENTPAIR_PEER_CLOSED, deadline, &observed);
    if ((status != ZX_OK) && (status != ZX_ERR_TIMED_OUT))
    {
        return status;
    }
    if ((observed & ZX_USER_SIGNAL_0) != 0U)
    {
        // Clear before the caller reads the VMO: a publication after the clear raises the signal again.
        const zx_status_t clear = notification_.signal(ZX_USER_SIGNAL_0, 0U);
        return (clear == ZX_OK) ? ZX_OK : clear;
    }
    return ((observed & ZX_EVENTPAIR_PEER_CLOSED) != 0U) ? ZX_ERR_PEER_CLOSED : ZX_ERR_TIMED_OUT;
}

auto FidlVmoHandleClient::ReleaseNotification() noexcept -> void
{
    notification_.reset();
}

auto FidlVmoHandleClient::Acquire(const std::string& key) noexcept -> score::cpp::expected<zx::vmo, score::os::Error>
{
    const auto role = classifier_(key);
    if (!role.has_value())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
    }
    const std::lock_guard<std::mutex> lock{mutex_};
    const zx_status_t status = EnsureAttached();
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ECONNRESET));
    }
    if (attach_error_.has_value())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ErrnoFor(*attach_error_)));
    }
    const auto it = handles_.find(*role);
    if (it == handles_.end())
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
    }
    zx::vmo handle = std::move(it->second);
    handles_.erase(it);
    return handle;
}

auto FidlVmoHandleClient::Publish(const std::string&, zx::vmo) noexcept -> score::cpp::expected_blank<score::os::Error>
{
    return score::cpp::make_unexpected(score::os::Error::createFromErrno(EPERM));
}

auto FidlVmoHandleClient::Withdraw(const std::string&) noexcept -> void {}

auto FidlVmoHandleClient::transport_status() const noexcept -> std::optional<zx_status_t>
{
    return transport_status_;
}

auto FidlVmoHandleClient::attach_error() const noexcept -> std::optional<score_mw_com::AttachError>
{
    return attach_error_;
}

auto FidlVmoHandleClient::attach_info() const noexcept -> const std::optional<AttachInfo>&
{
    return info_;
}

auto FidlVmoHandleClient::liveness() const noexcept -> const zx::eventpair&
{
    return liveness_;
}

}  // namespace score::mw::com::impl::lola::fuchsia
