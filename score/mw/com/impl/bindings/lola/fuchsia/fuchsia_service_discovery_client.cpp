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
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_service_discovery_client.h"

#include "score/mw/com/impl/com_error.h"
#include "score/mw/com/impl/configuration/lola_service_instance_id.h"
#include "score/mw/com/impl/configuration/lola_service_type_deployment.h"
#include "score/mw/log/logging.h"

#include <score/assert.hpp>
#include <score/utility.hpp>

#include <string_view>

#include <algorithm>
#include <chrono>
#include <limits>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr std::chrono::milliseconds kInitialBackoff{10};
constexpr std::chrono::milliseconds kMaxBackoff{500};
constexpr std::chrono::milliseconds kReofferRetry{100};

constexpr const char* kLogContext = "lola";

std::optional<WireQuality> ToWireQuality(const QualityType quality) noexcept
{
    switch (quality)
    {
        case QualityType::kASIL_QM:
            return WireQuality::kQm;
        case QualityType::kASIL_B:
            return WireQuality::kAsilB;
        case QualityType::kInvalid:
            [[fallthrough]];
        default:
            return std::nullopt;
    }
}

}  // namespace

FuchsiaServiceDiscoveryClient::FuchsiaServiceDiscoveryClient(std::unique_ptr<IDiscoveryTransport> transport)
    : IServiceDiscoveryClient{},
      transport_{std::move(transport)},
      next_epoch_{static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())}
{
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(transport_ != nullptr, "A discovery transport is required");
    transport_->SetRegistryLostHandler([this]() { OfferAgain(true); });
}

FuchsiaServiceDiscoveryClient::~FuchsiaServiceDiscoveryClient() noexcept
{
    {
        const std::lock_guard lock{find_mutex_};
        searches_.clear();
    }
    // Joins the transport thread; must not hold a lock a callback may wait for.
    transport_.reset();
}

auto FuchsiaServiceDiscoveryClient::OfferService(const InstanceIdentifier instance_identifier) -> Result<void>
{
    const EnrichedInstanceIdentifier enriched{instance_identifier};
    const auto service_id = enriched.GetBindingSpecificServiceId<LolaServiceTypeDeployment>();
    const auto instance_id = enriched.GetBindingSpecificInstanceId<LolaServiceInstanceId>();
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(instance_id.has_value(),
                                                      "Instance identifier must have instance id for service offer");
    if (!service_id.has_value())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Instance identifier has no LoLa service id");
    }

    // As on Linux: an ASIL-B provider also offers QM, QM first being the lower level.
    std::vector<WireQuality> qualities;
    switch (enriched.GetQualityType())
    {
        case QualityType::kASIL_B:
            qualities.push_back(WireQuality::kAsilB);
            [[fallthrough]];
        case QualityType::kASIL_QM:
            qualities.push_back(WireQuality::kQm);
            break;
        case QualityType::kInvalid:
            [[fallthrough]];
        default:
            return MakeUnexpected(ComErrc::kBindingFailure, "Unknown quality type of service");
    }

    const std::lock_guard lock{offer_mutex_};
    const OfferKey key{static_cast<std::uint64_t>(service_id.value()), static_cast<std::uint32_t>(instance_id.value())};
    if (offers_.find(key) != offers_.cend())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Service is already offered");
    }

    Offer offer{next_epoch_.fetch_add(1U), {}, {}};
    for (const auto quality : qualities)
    {
        const WireInstance wire{key.first, key.second, quality, offer.epoch};
        const auto status = transport_->Offer(wire);
        if (status != OfferStatus::kOk)
        {
            for (const auto done : offer.qualities)
            {
                transport_->StopOffer(WireInstance{key.first, key.second, done, offer.epoch});
            }
            score::mw::log::LogWarn(kLogContext)
                << "LoLa SD (Fuchsia): offer of service" << key.first << "instance" << key.second << "failed:"
                << (status == OfferStatus::kRejected ? std::string_view{"rejected by discovery (not configured)"}
                                                     : std::string_view{"discovery not reachable"});
            return MakeUnexpected(ComErrc::kServiceNotOffered, "Failed to offer service at the discovery component");
        }
        score::cpp::ignore = offer.qualities.insert(quality);
    }
    score::cpp::ignore = offers_.emplace(key, std::move(offer));
    return {};
}

auto FuchsiaServiceDiscoveryClient::StopOfferService(const InstanceIdentifier instance_identifier,
                                                     const IServiceDiscovery::QualityTypeSelector quality_type_selector)
    -> Result<void>
{
    const EnrichedInstanceIdentifier enriched{instance_identifier};
    const auto service_id = enriched.GetBindingSpecificServiceId<LolaServiceTypeDeployment>();
    const auto instance_id = enriched.GetBindingSpecificInstanceId<LolaServiceInstanceId>();
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(
        instance_id.has_value(), "Instance identifier must have instance id for service offer stop");
    if (!service_id.has_value())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Instance identifier has no LoLa service id");
    }

    const std::lock_guard lock{offer_mutex_};
    const OfferKey key{static_cast<std::uint64_t>(service_id.value()), static_cast<std::uint32_t>(instance_id.value())};
    const auto it = offers_.find(key);
    if (it == offers_.end())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Never offered or offer already stopped");
    }

    switch (quality_type_selector)
    {
        case IServiceDiscovery::QualityTypeSelector::kBoth:
            for (const auto quality : it->second.qualities)
            {
                transport_->StopOffer(WireInstance{key.first, key.second, quality, it->second.epoch});
            }
            score::cpp::ignore = offers_.erase(it);
            break;
        case IServiceDiscovery::QualityTypeSelector::kAsilQm:
            // Same as on Linux: only the QM offer is withdrawn, an ASIL-B offer stays until kBoth.
            score::cpp::ignore = it->second.pending.erase(WireQuality::kQm);
            if (it->second.qualities.erase(WireQuality::kQm) != 0U)
            {
                transport_->StopOffer(WireInstance{key.first, key.second, WireQuality::kQm, it->second.epoch});
            }
            break;
        default:
            return MakeUnexpected(ComErrc::kBindingFailure, "Unknown quality type of service");
    }
    return {};
}

auto FuchsiaServiceDiscoveryClient::ToHandles(const EnrichedInstanceIdentifier& identifier,
                                              const WireFilter& filter,
                                              const std::vector<WireInstance>& instances) const
    -> std::vector<HandleType>
{
    std::vector<HandleType> handles;
    std::set<std::uint32_t> seen;
    for (const auto& instance : instances)
    {
        if ((instance.service_id != filter.service_id) || (instance.quality != filter.quality) ||
            (filter.instance_id.has_value() && (instance.instance_id != filter.instance_id.value())))
        {
            continue;
        }
        if (instance.instance_id > std::numeric_limits<LolaServiceInstanceId::InstanceId>::max())
        {
            score::mw::log::LogWarn(kLogContext)
                << "LoLa SD (Fuchsia): ignoring instance id" << instance.instance_id << "(not a LoLa instance id)";
            continue;
        }
        if (!seen.insert(instance.instance_id).second)
        {
            continue;
        }
        handles.push_back(make_HandleType(identifier.GetInstanceIdentifier(),
                                          ServiceInstanceId{LolaServiceInstanceId{
                                              static_cast<LolaServiceInstanceId::InstanceId>(instance.instance_id)}}));
    }
    return handles;
}

namespace
{

std::optional<WireFilter> MakeFilter(const EnrichedInstanceIdentifier& identifier) noexcept
{
    const auto service_id = identifier.GetBindingSpecificServiceId<LolaServiceTypeDeployment>();
    const auto quality = ToWireQuality(identifier.GetQualityType());
    if (!service_id.has_value() || !quality.has_value())
    {
        return std::nullopt;
    }
    WireFilter filter{static_cast<std::uint64_t>(service_id.value()), std::nullopt, quality.value()};
    const auto instance_id = identifier.GetBindingSpecificInstanceId<LolaServiceInstanceId>();
    if (instance_id.has_value())
    {
        filter.instance_id = static_cast<std::uint32_t>(instance_id.value());
    }
    return filter;
}

}  // namespace

auto FuchsiaServiceDiscoveryClient::FindService(const EnrichedInstanceIdentifier enriched_instance_identifier)
    -> Result<ServiceHandleContainer<HandleType>>
{
    const auto filter = MakeFilter(enriched_instance_identifier);
    if (!filter.has_value())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Instance identifier does not have quality type set");
    }
    const auto instances = transport_->Find(filter.value());
    if (!instances.has_value())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Discovery component not reachable");
    }
    return ToHandles(enriched_instance_identifier, filter.value(), instances.value());
}

auto FuchsiaServiceDiscoveryClient::StartFindService(const FindServiceHandle find_service_handle,
                                                     FindServiceHandler<HandleType> handler,
                                                     const EnrichedInstanceIdentifier enriched_instance_identifier)
    -> Result<void>
{
    const auto filter = MakeFilter(enriched_instance_identifier);
    if (!filter.has_value())
    {
        return MakeUnexpected(ComErrc::kBindingFailure, "Instance identifier does not have quality type set");
    }

    const std::lock_guard lock{find_mutex_};

    // The synchronous part: what is offered right now is delivered before returning, like on Linux. If discovery
    // cannot be reached the search is still registered, it connects (and delivers) once discovery is there.
    std::vector<HandleType> known;
    const auto initial = transport_->Find(filter.value());
    if (initial.has_value())
    {
        known = ToHandles(enriched_instance_identifier, filter.value(), initial.value());
    }
    else
    {
        score::mw::log::LogWarn(kLogContext) << "LoLa SD (Fuchsia): discovery not reachable in StartFindService";
    }

    const auto emplaced = searches_.emplace(
        find_service_handle,
        Search{std::move(handler),
               enriched_instance_identifier,
               filter.value(),
               nullptr,
               std::unordered_set<HandleType>{known.cbegin(), known.cend()},
               0U,
               kInitialBackoff,
               false,
               false});
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(
        emplaced.second, "The FindServiceHandle should be unique for every call to StartFindService");

    BeginSearch(find_service_handle, emplaced.first->second);
    if (!known.empty())
    {
        Deliver(find_service_handle, std::move(known));
    }
    return {};
}

auto FuchsiaServiceDiscoveryClient::StopFindService(const FindServiceHandle find_service_handle) -> Result<void>
{
    // Waits for a handler running on the transport thread; from within a handler the mutex is already ours.
    const std::lock_guard lock{find_mutex_};
    const auto it = searches_.find(find_service_handle);
    if (it == searches_.end())
    {
        return {};
    }
    it->second.obsolete = true;
    it->second.transport_search.reset();
    if (!it->second.delivering)
    {
        score::cpp::ignore = searches_.erase(it);
    }
    return {};
}

auto FuchsiaServiceDiscoveryClient::BeginSearch(const FindServiceHandle handle, Search& search) -> void
{
    const auto incarnation = ++search.incarnation;
    search.transport_search = transport_->StartSearch(
        search.filter,
        [this, handle, incarnation](std::vector<WireInstance> instances) {
            OnSnapshot(handle, incarnation, std::move(instances));
        },
        [this, handle, incarnation]() { OnSearchClosed(handle, incarnation); });
}

auto FuchsiaServiceDiscoveryClient::Deliver(const FindServiceHandle handle, std::vector<HandleType> handles) -> void
{
    auto it = searches_.find(handle);
    it->second.delivering = true;
    // The handler may call Start/StopFindService (recursive lock); unordered_map references stay valid.
    it->second.handler(std::move(handles), handle);
    it = searches_.find(handle);
    it->second.delivering = false;
    if (it->second.obsolete)
    {
        score::cpp::ignore = searches_.erase(it);
    }
}

auto FuchsiaServiceDiscoveryClient::OnSnapshot(const FindServiceHandle handle,
                                               const std::uint64_t incarnation,
                                               std::vector<WireInstance> instances) -> void
{
    const std::lock_guard lock{find_mutex_};
    const auto it = searches_.find(handle);
    if ((it == searches_.end()) || it->second.obsolete || (it->second.incarnation != incarnation))
    {
        return;
    }
    auto& search = it->second;
    search.backoff = kInitialBackoff;

    auto handles = ToHandles(search.identifier, search.filter, instances);
    std::unordered_set<HandleType> current{handles.cbegin(), handles.cend()};
    if (current == search.delivered)
    {
        return;
    }
    search.delivered = std::move(current);
    Deliver(handle, std::move(handles));
}

auto FuchsiaServiceDiscoveryClient::OnSearchClosed(const FindServiceHandle handle, const std::uint64_t incarnation)
    -> void
{
    const std::lock_guard lock{find_mutex_};
    const auto it = searches_.find(handle);
    if ((it == searches_.end()) || it->second.obsolete || (it->second.incarnation != incarnation))
    {
        return;
    }
    auto& search = it->second;
    search.transport_search.reset();
    const auto delay = search.backoff;
    search.backoff = std::min(search.backoff * 2, kMaxBackoff);
    score::mw::log::LogDebug(kLogContext) << "LoLa SD (Fuchsia): search channel closed, restarting in"
                                          << static_cast<std::int64_t>(delay.count()) << "ms";
    transport_->PostDelayed([this, handle, incarnation]() { RestartSearch(handle, incarnation); }, delay);
}

auto FuchsiaServiceDiscoveryClient::RestartSearch(const FindServiceHandle handle, const std::uint64_t incarnation)
    -> void
{
    const std::lock_guard lock{find_mutex_};
    const auto it = searches_.find(handle);
    if ((it == searches_.end()) || it->second.obsolete || (it->second.incarnation != incarnation))
    {
        return;
    }
    // The first snapshot of the new search is compared against what was delivered last.
    BeginSearch(handle, it->second);
}

auto FuchsiaServiceDiscoveryClient::OfferAgain(const bool registry_lost) -> void
{
    const std::lock_guard lock{offer_mutex_};
    bool failed = false;
    for (auto& [key, offer] : offers_)
    {
        if (registry_lost)
        {
            offer.pending = offer.qualities;
        }
        for (auto it = offer.pending.begin(); it != offer.pending.end();)
        {
            if (transport_->Offer(WireInstance{key.first, key.second, *it, offer.epoch}) == OfferStatus::kOk)
            {
                it = offer.pending.erase(it);
            }
            else
            {
                failed = true;
                ++it;
            }
        }
    }
    if (failed)
    {
        transport_->PostDelayed([this]() { OfferAgain(false); }, kReofferRetry);
    }
}

}  // namespace score::mw::com::impl::lola::fuchsia
