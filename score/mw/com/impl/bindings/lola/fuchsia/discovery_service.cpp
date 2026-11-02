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
#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_service.h"

#include "score/mw/log/logging.h"

#include <lib/fidl/cpp/wire/channel.h>

#include <algorithm>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace score::mw::com::impl::lola::fuchsia
{

namespace
{

constexpr const char* kLogContext = "lola_discovery";

using OfferKey = std::tuple<std::uint64_t, std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t>;

OfferKey MakeKey(const score_mw_com::ServiceInstanceId& id, const score_mw_com::Quality quality) noexcept
{
    return {id.service_id(), id.instance_id(), id.major_version(), id.minor_version(), static_cast<std::uint8_t>(quality)};
}

}  // namespace

class DiscoveryService::Impl
{
  public:
    struct Offer
    {
        score_mw_com::OfferedInstance instance;
        const void* owner;
    };

    class RegistryConnection final : public fidl::Server<score_mw_com::ServiceRegistry>
    {
      public:
        RegistryConnection(Impl& impl, Participant skeleton) : impl_{impl}, skeleton_{std::move(skeleton)} {}

        void Offer(OfferRequest& request, OfferCompleter::Sync& completer) override
        {
            if (skeleton_.service_ids.count(request.id().service_id()) == 0U)
            {
                score::mw::log::LogWarn(kLogContext) << "Application " << skeleton_.application_id
                                                     << " offers service " << request.id().service_id()
                                                     << " it is not configured for.";
                completer.Reply(fit::error(score_mw_com::OfferError::kNotConfigured));
                return;
            }
            const auto key = MakeKey(request.id(), request.quality());
            if (!impl_.offers_
                     .emplace(key,
                              Impl::Offer{score_mw_com::OfferedInstance{request.id(), request.quality(), request.epoch()},
                                          this})
                     .second)
            {
                completer.Reply(fit::error(score_mw_com::OfferError::kAlreadyOffered));
                return;
            }
            impl_.NotifySearches();
            completer.Reply(fit::ok());
        }

        void StopOffer(StopOfferRequest& request, StopOfferCompleter::Sync& completer) override
        {
            const auto it = impl_.offers_.find(MakeKey(request.id(), request.quality()));
            if (it != impl_.offers_.end() && it->second.owner == this)
            {
                impl_.offers_.erase(it);
                impl_.NotifySearches();
            }
            completer.Reply();
        }

        void Closed() noexcept
        {
            impl_.WithdrawAll(this);
        }

      private:
        Impl& impl_;
        Participant skeleton_;
    };

    class SearchConnection final : public fidl::Server<score_mw_com::Search>
    {
      public:
        SearchConnection(Impl& impl, Participant consumer, score_mw_com::InstanceFilter filter)
            : impl_{impl}, consumer_{std::move(consumer)}, filter_{std::move(filter)}
        {
            impl_.searches_.insert(this);
        }
        ~SearchConnection() override
        {
            impl_.searches_.erase(this);
        }

        void Watch(WatchCompleter::Sync& completer) override
        {
            if (pending_.has_value())
            {
                // A second outstanding hanging get is a protocol violation.
                completer.Close(ZX_ERR_BAD_STATE);
                return;
            }
            auto snapshot = impl_.Snapshot(filter_, consumer_);
            if (!last_.has_value() || (snapshot != *last_))
            {
                last_ = snapshot;
                completer.Reply(fidl::Response<score_mw_com::Search::Watch>{std::move(snapshot)});
                return;
            }
            pending_.emplace(completer.ToAsync());
        }

        void Notify()
        {
            if (!pending_.has_value())
            {
                return;
            }
            auto snapshot = impl_.Snapshot(filter_, consumer_);
            if (snapshot != last_)
            {
                last_ = snapshot;
                auto completer = std::move(*pending_);
                pending_.reset();
                completer.Reply(fidl::Response<score_mw_com::Search::Watch>{std::move(snapshot)});
            }
        }

      private:
        Impl& impl_;
        Participant consumer_;
        score_mw_com::InstanceFilter filter_;
        std::optional<std::vector<score_mw_com::OfferedInstance>> last_;
        std::optional<WatchCompleter::Async> pending_;
    };

    class DiscoveryConnection final : public fidl::Server<score_mw_com::ServiceDiscovery>
    {
      public:
        DiscoveryConnection(Impl& impl, Participant consumer) : impl_{impl}, consumer_{std::move(consumer)} {}

        void Find(FindRequest& request, FindCompleter::Sync& completer) override
        {
            completer.Reply(
                fidl::Response<score_mw_com::ServiceDiscovery::Find>{impl_.Snapshot(request.filter(), consumer_)});
        }

        void StartFind(StartFindRequest& request, StartFindCompleter::Sync&) override
        {
            fidl::BindServer(impl_.dispatcher_,
                             std::move(request.search()),
                             std::make_unique<SearchConnection>(impl_, consumer_, request.filter()));
        }

      private:
        Impl& impl_;
        Participant consumer_;
    };

    explicit Impl(async_dispatcher_t* dispatcher) : dispatcher_{dispatcher} {}

    std::vector<score_mw_com::OfferedInstance> Snapshot(const score_mw_com::InstanceFilter& filter,
                                                        const Participant& consumer) const
    {
        std::vector<score_mw_com::OfferedInstance> result;
        // std::map order: by (service, instance, version, quality), so equal sets compare equal.
        for (const auto& entry : offers_)
        {
            const auto& instance = entry.second.instance;
            const auto& id = instance.id();
            const bool matches =
                (consumer.service_ids.count(id.service_id()) != 0U) &&
                (!filter.service_id().has_value() || *filter.service_id() == id.service_id()) &&
                (!filter.instance_id().has_value() || *filter.instance_id() == id.instance_id()) &&
                (!filter.major_version().has_value() || *filter.major_version() == id.major_version()) &&
                (!filter.minor_version().has_value() || *filter.minor_version() == id.minor_version()) &&
                (!filter.quality().has_value() || *filter.quality() == instance.quality());
            if (matches)
            {
                result.push_back(instance);
            }
        }
        return result;
    }

    void NotifySearches()
    {
        // Notify() may reply but never changes searches_.
        for (auto* search : searches_)
        {
            search->Notify();
        }
    }

    void WithdrawAll(const void* owner)
    {
        const auto erased = std::erase_if(offers_, [owner](const auto& entry) { return entry.second.owner == owner; });
        if (erased != 0U)
        {
            NotifySearches();
        }
    }

    async_dispatcher_t* dispatcher_;
    std::map<OfferKey, Offer> offers_;
    std::set<SearchConnection*> searches_;
};

DiscoveryService::DiscoveryService(async_dispatcher_t* dispatcher) : impl_{std::make_unique<Impl>(dispatcher)} {}

DiscoveryService::~DiscoveryService() = default;

auto DiscoveryService::RegistryHandler(Participant skeleton) -> fidl::ProtocolHandler<score_mw_com::ServiceRegistry>
{
    return [impl = impl_.get(), skeleton = std::move(skeleton)](fidl::ServerEnd<score_mw_com::ServiceRegistry> server_end) {
        fidl::BindServer(impl->dispatcher_,
                         std::move(server_end),
                         std::make_unique<Impl::RegistryConnection>(*impl, skeleton),
                         [](Impl::RegistryConnection* connection,
                            fidl::UnbindInfo,
                            fidl::ServerEnd<score_mw_com::ServiceRegistry>) { connection->Closed(); });
    };
}

auto DiscoveryService::DiscoveryHandler(Participant consumer) -> fidl::ProtocolHandler<score_mw_com::ServiceDiscovery>
{
    return [impl = impl_.get(), consumer = std::move(consumer)](fidl::ServerEnd<score_mw_com::ServiceDiscovery> server_end) {
        fidl::BindServer(impl->dispatcher_,
                         std::move(server_end),
                         std::make_unique<Impl::DiscoveryConnection>(*impl, consumer));
    };
}

auto DiscoveryService::OfferCount() const noexcept -> std::size_t
{
    return impl_->offers_.size();
}

auto DiscoveryService::SearchCount() const noexcept -> std::size_t
{
    return impl_->searches_.size();
}

}  // namespace score::mw::com::impl::lola::fuchsia
