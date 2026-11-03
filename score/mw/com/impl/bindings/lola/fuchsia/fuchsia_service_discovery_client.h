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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FUCHSIA_SERVICE_DISCOVERY_CLIENT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FUCHSIA_SERVICE_DISCOVERY_CLIENT_H

#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_transport.h"
#include "score/mw/com/impl/enriched_instance_identifier.h"
#include "score/mw/com/impl/find_service_handle.h"
#include "score/mw/com/impl/find_service_handler.h"
#include "score/mw/com/impl/handle_type.h"
#include "score/mw/com/impl/i_service_discovery_client.h"
#include "score/mw/com/impl/instance_identifier.h"

#include "score/result/result.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief IServiceDiscoveryClient of LoLa on Fuchsia (design/fuchsia/README.md section 6.3). The Linux counterpart is
///        file system/inotify based, this one is a client of the FIDL protocols `ServiceRegistry` (skeleton side) and
///        `ServiceDiscovery`/`Search` (proxy side) of the discovery component, see IDiscoveryTransport.
///
/// \details Mapping of LoLa identifiers to the wire (identical to what the realm generator configures the discovery
///          component with):
///          - FIDL `service_id`   = LolaServiceTypeDeployment::service_id_ (uint16, widened to uint64)
///          - FIDL `instance_id`  = LolaServiceInstanceId (uint16, widened to uint32). Find-any = filter without
///                                  instance id, the instance ids are taken from the answers.
///          - FIDL `major/minor_version` = 0 (LoLa deployments carry no version)
///          - FIDL `Quality`      = QM for kASIL_QM, ASIL_B for kASIL_B. As on Linux, offering with kASIL_B offers
///                                  ASIL_B *and* QM; a search only returns offers of exactly its quality.
///          - FIDL `epoch`        = incarnation of this offer (strictly increasing per process, informational for
///                                  discovery; the attach epoch is authoritative).
///
///          Semantics kept from the Linux client: StartFindService calls the handler synchronously if something is
///          already offered, later only with the full set of handles and only if the set changed. After
///          StopFindService returned the handler is not called any more (it blocks while a handler runs on another
///          thread, and returns at once when called from the handler itself).
///
///          Differences: handler calls run on the transport thread. A lost search channel (discovery restart) is
///          re-established with back-off; the first snapshot after that is compared against the last delivered set, so
///          a restart that changes nothing is invisible and one that lost offers shows up as a changed set. A lost
///          registry channel makes the client offer everything again. Process death withdraws all offers at the
///          discovery component (channel closed), there is no persistent state like flag files.
class FuchsiaServiceDiscoveryClient final : public IServiceDiscoveryClient
{
  public:
    explicit FuchsiaServiceDiscoveryClient(std::unique_ptr<IDiscoveryTransport> transport);
    ~FuchsiaServiceDiscoveryClient() noexcept override;

    FuchsiaServiceDiscoveryClient(const FuchsiaServiceDiscoveryClient&) = delete;
    FuchsiaServiceDiscoveryClient& operator=(const FuchsiaServiceDiscoveryClient&) = delete;
    FuchsiaServiceDiscoveryClient(FuchsiaServiceDiscoveryClient&&) = delete;
    FuchsiaServiceDiscoveryClient& operator=(FuchsiaServiceDiscoveryClient&&) = delete;

    [[nodiscard]] Result<void> OfferService(const InstanceIdentifier instance_identifier) override;
    [[nodiscard]] Result<void> StopOfferService(
        const InstanceIdentifier instance_identifier,
        const IServiceDiscovery::QualityTypeSelector quality_type_selector) override;
    [[nodiscard]] Result<void> StartFindService(const FindServiceHandle find_service_handle,
                                                FindServiceHandler<HandleType> handler,
                                                const EnrichedInstanceIdentifier enriched_instance_identifier) override;
    [[nodiscard]] Result<void> StopFindService(const FindServiceHandle find_service_handle) override;
    [[nodiscard]] Result<ServiceHandleContainer<HandleType>> FindService(
        const EnrichedInstanceIdentifier enriched_instance_identifier) override;

  private:
    using OfferKey = std::pair<std::uint64_t, std::uint32_t>;

    struct Offer
    {
        std::uint64_t epoch;
        std::set<WireQuality> qualities;
        // Qualities that have to be offered again after the registry connection was lost.
        std::set<WireQuality> pending;
    };

    struct Search
    {
        FindServiceHandler<HandleType> handler;
        EnrichedInstanceIdentifier identifier;
        WireFilter filter;
        std::unique_ptr<ISearch> transport_search;
        std::unordered_set<HandleType> delivered;
        // Callbacks of an older transport search (before a restart) are ignored.
        std::uint64_t incarnation;
        std::chrono::milliseconds backoff;
        bool delivering;
        bool obsolete;
    };

    std::vector<HandleType> ToHandles(const EnrichedInstanceIdentifier& identifier,
                                      const WireFilter& filter,
                                      const std::vector<WireInstance>& instances) const;
    void BeginSearch(FindServiceHandle handle, Search& search);
    void OnSnapshot(FindServiceHandle handle, std::uint64_t incarnation, std::vector<WireInstance> instances);
    void OnSearchClosed(FindServiceHandle handle, std::uint64_t incarnation);
    void RestartSearch(FindServiceHandle handle, std::uint64_t incarnation);
    void Deliver(FindServiceHandle handle, std::vector<HandleType> handles);
    void OfferAgain(bool registry_lost);

    std::unique_ptr<IDiscoveryTransport> transport_;

    std::atomic<std::uint64_t> next_epoch_;

    std::mutex offer_mutex_;
    std::map<OfferKey, Offer> offers_;

    // Recursive so that Start/StopFindService may be called from a handler. Held while a handler runs.
    std::recursive_mutex find_mutex_;
    std::unordered_map<FindServiceHandle, Search> searches_;
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FUCHSIA_SERVICE_DISCOVERY_CLIENT_H
