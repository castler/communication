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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FAKE_DISCOVERY_TRANSPORT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FAKE_DISCOVERY_TRANSPORT_H

#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_transport.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>

namespace score::mw::com::impl::lola::fuchsia::test
{

/// Deterministic single threaded in-memory discovery: the "transport thread" is whoever calls Pump().
class FakeDiscoveryState
{
  public:
    using Key = std::tuple<std::uint64_t, std::uint32_t, WireQuality>;

    struct SearchRecord
    {
        WireFilter filter;
        IDiscoveryTransport::SnapshotHandler on_snapshot;
        IDiscoveryTransport::ClosedHandler on_closed;
        bool alive;
    };

    bool reachable{true};
    std::set<std::uint64_t> rejected_services;
    std::map<Key, std::uint64_t> offers;
    std::deque<std::function<void()>> tasks;
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> delayed;
    std::vector<std::chrono::milliseconds> delays_requested;
    std::map<int, SearchRecord> searches;
    std::function<void()> registry_lost;
    int next_search_id{0};
    int offer_calls{0};
    int stop_offer_calls{0};

    std::vector<WireInstance> Matching(const WireFilter& filter) const
    {
        std::vector<WireInstance> result;
        for (const auto& [key, epoch] : offers)
        {
            const auto& [service, instance, quality] = key;
            if (service == filter.service_id && quality == filter.quality &&
                (!filter.instance_id.has_value() || filter.instance_id.value() == instance))
            {
                result.push_back(WireInstance{service, instance, quality, epoch});
            }
        }
        return result;
    }

    /// Queues a fresh snapshot for every live search (what the hanging get does after a change).
    void Notify()
    {
        for (auto& [id, search] : searches)
        {
            if (search.alive)
            {
                const auto snapshot = Matching(search.filter);
                const int search_id = id;
                tasks.push_back([this, search_id, snapshot]() {
                    const auto it = searches.find(search_id);
                    if (it != searches.end() && it->second.alive)
                    {
                        it->second.on_snapshot(snapshot);
                    }
                });
            }
        }
    }

    /// Runs queued tasks until quiet. Delayed tasks are not run.
    void Pump()
    {
        while (!tasks.empty())
        {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }

    /// Runs the oldest delayed task.
    bool RunDelayed()
    {
        if (delayed.empty())
        {
            return false;
        }
        auto task = std::move(delayed.front().second);
        delayed.erase(delayed.begin());
        task();
        Pump();
        return true;
    }

    /// Discovery component restarts: all offers are gone, all searches closed, registry connection lost. With
    /// `reachable == false` it stays away until set to true again.
    void Restart(const bool stay_away = false)
    {
        offers.clear();
        reachable = !stay_away;
        for (auto& [id, search] : searches)
        {
            if (search.alive)
            {
                search.alive = false;
                auto closed = search.on_closed;
                tasks.push_back(closed);
            }
        }
        if (registry_lost)
        {
            tasks.push_back(registry_lost);
        }
    }

    // A restarted discovery learns about nothing: stale in-flight snapshots must not leak into the new incarnation.
    std::size_t LiveSearches() const
    {
        return static_cast<std::size_t>(
            std::count_if(searches.begin(), searches.end(), [](const auto& s) { return s.second.alive; }));
    }
};

class FakeDiscoveryTransport final : public IDiscoveryTransport
{
  public:
    explicit FakeDiscoveryTransport(FakeDiscoveryState& state) : state_{state} {}

    OfferStatus Offer(const WireInstance& instance) override
    {
        ++state_.offer_calls;
        if (!state_.reachable)
        {
            return OfferStatus::kUnavailable;
        }
        if (state_.rejected_services.count(instance.service_id) != 0U)
        {
            return OfferStatus::kRejected;
        }
        state_.offers[{instance.service_id, instance.instance_id, instance.quality}] = instance.epoch;
        state_.Notify();
        return OfferStatus::kOk;
    }

    void StopOffer(const WireInstance& instance) override
    {
        ++state_.stop_offer_calls;
        if (state_.offers.erase({instance.service_id, instance.instance_id, instance.quality}) != 0U)
        {
            state_.Notify();
        }
    }

    std::optional<std::vector<WireInstance>> Find(const WireFilter& filter) override
    {
        if (!state_.reachable)
        {
            return std::nullopt;
        }
        return state_.Matching(filter);
    }

    std::unique_ptr<ISearch> StartSearch(const WireFilter& filter,
                                         SnapshotHandler on_snapshot,
                                         ClosedHandler on_closed) override
    {
        const int id = state_.next_search_id++;
        state_.searches[id] = FakeDiscoveryState::SearchRecord{filter, on_snapshot, on_closed, true};
        if (!state_.reachable)
        {
            state_.searches[id].alive = false;
            state_.tasks.push_back(on_closed);
        }
        else
        {
            const auto snapshot = state_.Matching(filter);
            state_.tasks.push_back([state = &state_, id, snapshot]() {
                const auto it = state->searches.find(id);
                if (it != state->searches.end() && it->second.alive)
                {
                    it->second.on_snapshot(snapshot);
                }
            });
        }
        return std::make_unique<FakeSearch>(state_, id);
    }

    void SetRegistryLostHandler(std::function<void()> handler) override
    {
        state_.registry_lost = std::move(handler);
    }

    void PostDelayed(std::function<void()> task, const std::chrono::milliseconds delay) override
    {
        state_.delays_requested.push_back(delay);
        state_.delayed.emplace_back(delay, std::move(task));
    }

  private:
    class FakeSearch final : public ISearch
    {
      public:
        FakeSearch(FakeDiscoveryState& state, const int id) : state_{state}, id_{id} {}
        ~FakeSearch() override
        {
            state_.searches[id_].alive = false;
        }

      private:
        FakeDiscoveryState& state_;
        int id_;
    };

    FakeDiscoveryState& state_;
};

}  // namespace score::mw::com::impl::lola::fuchsia::test

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_FAKE_DISCOVERY_TRANSPORT_H
