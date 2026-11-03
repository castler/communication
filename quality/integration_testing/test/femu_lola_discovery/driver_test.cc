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
#include <fidl/score.mw.com.test.lola/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

// Driver: a skeleton component and a proxy component, each running the real LoLa discovery client, find each other
// through the discovery component. Scenarios: start-find-before-offer, stop-offer, re-offer, skeleton crash and offer
// restart, find-once, start-find-after-offer (synchronous handler call), stop-find, offer of a foreign service id.

namespace
{
using Ids = std::vector<std::uint16_t>;
constexpr std::uint32_t kTimeoutMs = 20000U;

int failures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

using Agent = fidl::SyncClient<score_mw_com_test_lola::Agent>;

Agent Connect(const char* path)
{
    auto client = component::Connect<score_mw_com_test_lola::Agent>(path);
    if (client.is_error())
    {
        std::printf("FAIL connect %s: %d\n", path, client.status_value());
        std::exit(1);
    }
    return Agent{std::move(client.value())};
}

bool Offer(Agent& skeleton, const std::uint16_t instance)
{
    auto result = skeleton->Offer({instance});
    return result.is_ok() && result->ok();
}

bool StopOffer(Agent& skeleton, const std::uint16_t instance)
{
    auto result = skeleton->StopOffer({instance});
    return result.is_ok() && result->ok();
}

struct Waited
{
    bool reached;
    std::uint32_t calls;
    Ids instances;
};

Waited WaitCalls(Agent& proxy, const std::uint32_t min_calls)
{
    auto result = proxy->WaitCalls({{min_calls, kTimeoutMs}});
    if (result.is_error())
    {
        return {false, 0U, {}};
    }
    return {result->reached(), result->calls(), result->instances()};
}

std::uint32_t Calls(Agent& proxy)
{
    auto result = proxy->Calls();
    return result.is_ok() ? result->calls() : 9999U;
}
}  // namespace

int main()
{
    auto skeleton = Connect("/svc/agent_skeleton");
    auto proxy = Connect("/svc/agent_proxy");

    // Start-find-before-offer: nothing offered, no handler call.
    {
        auto started = proxy->StartFind();
        CHECK(started.is_ok() && started->calls() == 0U && started->instances().empty());
    }

    // Offer: the handler reports the instance.
    CHECK(Offer(skeleton, 1U));
    auto waited = WaitCalls(proxy, 1U);
    CHECK(waited.reached && waited.calls == 1U && waited.instances == Ids{1U});

    // Offering the same instance twice is an error, a second instance is fine and shows up as a changed set.
    CHECK(!Offer(skeleton, 1U));
    CHECK(Offer(skeleton, 2U));
    waited = WaitCalls(proxy, 2U);
    CHECK(waited.reached && waited.instances.size() == 2U);
    CHECK(StopOffer(skeleton, 2U));
    waited = WaitCalls(proxy, 3U);
    CHECK(waited.reached && waited.instances == Ids{1U});

    // Find-once sees the same.
    {
        auto once = proxy->FindOnce();
        CHECK(once.is_ok() && once->ok() && once->instances() == Ids{1U});
    }

    // Stop-offer: the handler reports the empty set.
    CHECK(StopOffer(skeleton, 1U));
    waited = WaitCalls(proxy, 4U);
    CHECK(waited.reached && waited.instances.empty());
    CHECK(!StopOffer(skeleton, 1U));

    // Offer again.
    CHECK(Offer(skeleton, 1U));
    waited = WaitCalls(proxy, 5U);
    CHECK(waited.reached && waited.instances == Ids{1U});

    // Offer restart: the skeleton process dies, discovery withdraws the offer, the restarted skeleton offers again.
    {
        auto crash = skeleton->Crash();
        CHECK(crash.is_ok());
    }
    waited = WaitCalls(proxy, 6U);
    CHECK(waited.reached && waited.instances.empty());
    // The component may still be shutting down: a connection routed to the old instance is closed, retry.
    bool reoffered = false;
    for (int attempt = 0; attempt < 100 && !reoffered; ++attempt)
    {
        skeleton = Connect("/svc/agent_skeleton");
        reoffered = Offer(skeleton, 1U);
        if (!reoffered)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CHECK(reoffered);
    waited = WaitCalls(proxy, 7U);
    CHECK(waited.reached && waited.instances == Ids{1U});

    // A service id that the discovery component is not configured for for this skeleton is refused.
    {
        auto foreign = skeleton->OfferForeign();
        CHECK(foreign.is_ok() && !foreign->ok());
    }

    // Stop-find: no handler calls after it returned.
    {
        auto stopped = proxy->StopFind();
        CHECK(stopped.is_ok() && stopped->calls() == 7U);
    }
    CHECK(StopOffer(skeleton, 1U));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(Calls(proxy) == 7U);

    // Start-find-after-offer: the handler is called before StartFindService returns.
    CHECK(Offer(skeleton, 1U));
    {
        auto started = proxy->StartFind();
        CHECK(started.is_ok() && started->calls() == 8U && started->instances() == Ids{1U});
    }
    CHECK(StopOffer(skeleton, 1U));

    std::printf(failures == 0 ? "LOLA_DISCOVERY_TEST PASS\n" : "LOLA_DISCOVERY_TEST FAIL\n");
    return failures == 0 ? 0 : 1;
}
