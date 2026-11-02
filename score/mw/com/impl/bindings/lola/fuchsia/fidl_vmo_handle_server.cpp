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
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"

#include "score/mw/log/logging.h"

#include <lib/fidl/cpp/wire/channel.h>
#include <zircon/syscalls.h>

#include <cerrno>
#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr const char* kLogContext = "lola_fidl";

/// Exactly `declared | TRANSFER`: the kernel needs TRANSFER to send the handle, the FIDL encoder then reduces it to the
/// declared rights. Nothing beyond that leaves the process even if the original has more.
score::cpp::expected<zx::vmo, zx_status_t> DuplicateForSending(const zx::vmo& original, const zx_rights_t declared)
{
    const zx_rights_t required = declared | ZX_RIGHT_TRANSFER;
    zx_info_handle_basic_t info{};
    zx_status_t status = original.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(status);
    }
    if ((info.rights & required) != required)
    {
        return score::cpp::make_unexpected(ZX_ERR_ACCESS_DENIED);
    }
    zx::vmo duplicate;
    status = original.duplicate(required, &duplicate);
    if (status != ZX_OK)
    {
        return score::cpp::make_unexpected(status);
    }
    return duplicate;
}

}  // namespace

struct FidlVmoHandleServer::State
{
    struct Published
    {
        std::string key;
        zx::vmo handle;
    };
    struct Session
    {
        zx::eventpair local_liveness;
        zx::eventpair local_notification;
    };

    explicit State(KeyClassifier key_classifier) : classifier{std::move(key_classifier)} {}

    /// Caller holds `mutex`. Drops the sessions whose proxy closed its notification handle (died or opted out).
    std::size_t PruneLocked()
    {
        std::size_t removed{0U};
        for (auto it = sessions.begin(); it != sessions.end();)
        {
            zx_signals_t observed{0U};
            const zx_status_t status =
                it->local_notification.wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::time::infinite_past(), &observed);
            if ((status == ZX_OK) || ((observed & ZX_EVENTPAIR_PEER_CLOSED) != 0U))
            {
                it = sessions.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
        return removed;
    }

    mutable std::mutex mutex;
    KeyClassifier classifier;
    std::map<VmoRole, Published> published;
    std::uint64_t epoch{0U};
    std::vector<std::uint8_t> layout;
    std::optional<std::set<std::uint64_t>> allowed;
    std::map<std::uint64_t, std::size_t> attach_counts;
    std::vector<Session> sessions;
};

class FidlVmoHandleServer::Connection final : public fidl::Server<score_mw_com::Endpoint>
{
  public:
    Connection(std::shared_ptr<State> state, const std::uint64_t application_id)
        : state_{std::move(state)}, application_id_{application_id}
    {
    }

    void Attach(AttachCompleter::Sync& completer) override
    {
        // At most one Attach per connection: a second one would hand out a further set of handles to the same peer.
        if (attached_)
        {
            completer.Reply(fit::error(score_mw_com::AttachError::kNotAllowed));
            return;
        }
        const std::lock_guard<std::mutex> lock{state_->mutex};
        if (state_->allowed.has_value() && (state_->allowed->count(application_id_) == 0U))
        {
            score::mw::log::LogWarn(kLogContext) << "Attach on the endpoint of application " << application_id_
                                                 << " refused: not in the allowed set.";
            completer.Reply(fit::error(score_mw_com::AttachError::kNotAllowed));
            return;
        }
        const auto data = state_->published.find(VmoRole::kData);
        const auto control = state_->published.find(VmoRole::kControl);
        if (data == state_->published.end() || control == state_->published.end())
        {
            completer.Reply(fit::error(score_mw_com::AttachError::kNotFound));
            return;
        }
        auto data_copy = DuplicateForSending(data->second.handle, kDataRights);
        auto control_copy = DuplicateForSending(control->second.handle, kControlRights);
        zx::eventpair local;
        zx::eventpair remote;
        zx::eventpair notify_local;
        zx::eventpair notify_remote;
        if (!data_copy.has_value() || !control_copy.has_value() || (zx::eventpair::create(0U, &local, &remote) != ZX_OK) ||
            (remote.replace(kLivenessRights | ZX_RIGHT_TRANSFER, &remote) != ZX_OK) ||
            (zx::eventpair::create(0U, &notify_local, &notify_remote) != ZX_OK) ||
            (notify_remote.replace(kNotificationRights | ZX_RIGHT_TRANSFER, &notify_remote) != ZX_OK))
        {
            score::mw::log::LogError(kLogContext) << "Attach: published handles lack the rights that Endpoint declares.";
            completer.Reply(fit::error(score_mw_com::AttachError::kNotFound));
            return;
        }

        attached_ = true;
        ++state_->attach_counts[application_id_];
        state_->PruneLocked();
        state_->sessions.push_back(State::Session{std::move(local), std::move(notify_local)});
        completer.Reply(fit::ok(score_mw_com::EndpointAttachResponse{std::move(data_copy).value(),
                                                                     std::move(control_copy).value(),
                                                                     state_->layout,
                                                                     state_->epoch,
                                                                     application_id_,
                                                                     std::move(remote),
                                                                     std::move(notify_remote)}));
    }

  private:
    std::shared_ptr<State> state_;
    std::uint64_t application_id_;
    bool attached_{false};
};

FidlVmoHandleServer::FidlVmoHandleServer(async_dispatcher_t* dispatcher, KeyClassifier classifier)
    : IVmoHandleProvider{}, dispatcher_{dispatcher}, state_{std::make_shared<State>(std::move(classifier))}
{
}

FidlVmoHandleServer::~FidlVmoHandleServer() = default;

auto FidlVmoHandleServer::Acquire(const std::string&) noexcept -> score::cpp::expected<zx::vmo, score::os::Error>
{
    return score::cpp::make_unexpected(score::os::Error::createFromErrno(ENOENT));
}

auto FidlVmoHandleServer::Publish(const std::string& key, zx::vmo consumer_handle) noexcept
    -> score::cpp::expected_blank<score::os::Error>
{
    const auto role = state_->classifier(key);
    if (!role.has_value())
    {
        score::mw::log::LogError(kLogContext) << "Region " << key << " has no role in Attach (data/control).";
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EINVAL));
    }
    const std::lock_guard<std::mutex> lock{state_->mutex};
    if (!state_->published.emplace(*role, State::Published{key, std::move(consumer_handle)}).second)
    {
        return score::cpp::make_unexpected(score::os::Error::createFromErrno(EEXIST));
    }
    return {};
}

auto FidlVmoHandleServer::Withdraw(const std::string& key) noexcept -> void
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    for (auto it = state_->published.begin(); it != state_->published.end(); ++it)
    {
        if (it->second.key == key)
        {
            state_->published.erase(it);
            return;
        }
    }
}

auto FidlVmoHandleServer::SetEpoch(const std::uint64_t epoch) noexcept -> void
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    state_->epoch = epoch;
}

auto FidlVmoHandleServer::SetLayout(std::vector<std::uint8_t> layout) noexcept -> void
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    state_->layout = std::move(layout);
}

auto FidlVmoHandleServer::SetAllowedApplications(std::optional<std::set<std::uint64_t>> allowed) noexcept -> void
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    state_->allowed = std::move(allowed);
}

auto FidlVmoHandleServer::CreateHandler(const std::uint64_t application_id)
    -> fidl::ProtocolHandler<score_mw_com::Endpoint>
{
    return [dispatcher = dispatcher_, state = state_, application_id](fidl::ServerEnd<score_mw_com::Endpoint> server_end) {
        fidl::BindServer(dispatcher, std::move(server_end), std::make_unique<Connection>(state, application_id));
    };
}

auto FidlVmoHandleServer::DuplicateForBroker(const VmoRole role) const noexcept -> std::optional<zx::vmo>
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    const auto found = state_->published.find(role);
    if (found == state_->published.end())
    {
        return std::nullopt;
    }
    auto copy = DuplicateForSending(found->second.handle, role == VmoRole::kData ? kDataRights : kControlRights);
    if (!copy.has_value())
    {
        return std::nullopt;
    }
    return std::move(copy).value();
}

auto FidlVmoHandleServer::NotifySubscribers() noexcept -> std::size_t
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    std::size_t signalled{0U};
    for (auto it = state_->sessions.begin(); it != state_->sessions.end();)
    {
        // Never blocks: a signal is a level that coalesces, a subscriber that has not consumed the last one simply
        // sees one wake-up for several publications.
        const zx_status_t status = it->local_notification.signal_peer(0U, ZX_USER_SIGNAL_0);
        if (status == ZX_ERR_PEER_CLOSED)
        {
            it = state_->sessions.erase(it);
            continue;
        }
        if (status == ZX_OK)
        {
            ++signalled;
        }
        ++it;
    }
    return signalled;
}

auto FidlVmoHandleServer::SubscriberCount() noexcept -> std::size_t
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    state_->PruneLocked();
    return state_->sessions.size();
}

auto FidlVmoHandleServer::AttachCount(const std::uint64_t application_id) const noexcept -> std::size_t
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    const auto it = state_->attach_counts.find(application_id);
    return it == state_->attach_counts.end() ? 0U : it->second;
}

auto FidlVmoHandleServer::AttachCount() const noexcept -> std::size_t
{
    const std::lock_guard<std::mutex> lock{state_->mutex};
    std::size_t sum{0U};
    for (const auto& entry : state_->attach_counts)
    {
        sum += entry.second;
    }
    return sum;
}

}  // namespace score::mw::com::impl::lola::fuchsia
