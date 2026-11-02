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

    auto events = fidl::CreateEndpoints<score_mw_com::EventChannel>();
    if (events.is_error())
    {
        transport_status_ = events.status_value();
        return *transport_status_;
    }
    auto result = client_->Attach(score_mw_com::EndpointAttachRequest{std::move(events->server)});
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
    liveness_ = std::move(response.liveness());
    events_ = std::move(events->client);
    return ZX_OK;
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
