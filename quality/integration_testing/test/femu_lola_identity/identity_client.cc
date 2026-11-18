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
#include "quality/integration_testing/test/femu_lola_identity/identity_protocol.h"
#include "score/message_passing/client_factory.h"

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

// Consumer of the identity provider. The role decides what is verified:
//   --role=allowed  consumer 7 (child component): is served and seen as principal 7
//   --role=denied   consumer 8 (child component), which also claims to be 7 in its factory: must be rejected
//   --role=test     the test component, consumer 7: checks its identity, tries to spoof, reconnects, and finally
//                   asks the provider what it has seen from all consumers
namespace
{

using namespace score::message_passing;
namespace identity = score::mw::com::identity_test;

int failures{0};

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cout << "FAILED: " << what << std::endl;
        ++failures;
    }
}

score::cpp::pmr::unique_ptr<IClientConnection> Connect(ClientFactory& factory)
{
    ServiceProtocolConfig protocol_config{identity::kProtocolName, 1024, 1024, 1024};
    auto client = factory.Create(protocol_config, IClientFactory::ClientConfig{1, 1, false, true, false});
    auto ready = std::make_shared<std::promise<void>>();
    client->Start(
        [ready](auto state) {
            if (state == IClientConnection::State::kReady)
            {
                ready->set_value();
            }
        },
        IClientConnection::NotifyCallback{});
    if (ready->get_future().wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        return nullptr;
    }
    return client;
}

std::optional<std::vector<std::uint32_t>> Ask(IClientConnection& client, const std::uint8_t command)
{
    const std::array<std::uint8_t, 1> request{command};
    std::array<std::uint8_t, 32> buffer{};
    const auto reply = client.SendWaitReply(request, buffer);
    if (!reply.has_value())
    {
        return std::nullopt;
    }
    return identity::Decode(reply.value());
}

std::optional<std::vector<std::uint32_t>> WhoAmI(ClientFactory& factory)
{
    auto client = Connect(factory);
    return client ? Ask(*client, identity::kWhoAmI) : std::nullopt;
}

int RunAllowed()
{
    ClientFactory factory{};
    const auto me = WhoAmI(factory);
    Check(me.has_value() && (me->at(0) == identity::kAllowedConsumer), "consumer C is principal 7");
    return failures;
}

int RunDenied()
{
    // Claims to be the allowed consumer 7, which only counts for servers inside this process
    ClientFactory factory{score::cpp::pmr::get_default_resource(), identity::kAllowedConsumer};
    const auto me = WhoAmI(factory);
    Check(!me.has_value(), "consumer B (routed as 8) is rejected despite claiming 7");
    return failures;
}

int RunTest()
{
    ClientFactory factory{};
    const auto first = WhoAmI(factory);
    Check(first.has_value() && (first->at(0) == identity::kAllowedConsumer), "consumer A is principal 7");

    // A client that claims another principal cannot influence the principal the provider derives
    ClientFactory spoofing{score::cpp::pmr::get_default_resource(), identity::kDeniedConsumer};
    const auto spoofed = WhoAmI(spoofing);
    Check(spoofed.has_value() && (spoofed->at(0) == identity::kAllowedConsumer), "claimed principal 8 is ignored");

    // Reconnecting in the same process keeps the node
    const auto second = WhoAmI(factory);
    Check(first.has_value() && second.has_value() && (first->at(1) == second->at(1)), "reconnect keeps the node");
    Check(first.has_value() && spoofed.has_value() && (first->at(1) == spoofed->at(1)), "same process is one node");

    // The children C (allowed) and B (denied) connect on their own; wait until the provider has seen them
    std::optional<std::vector<std::uint32_t>> stats;
    auto observer = Connect(factory);
    Check(observer != nullptr, "observer connected");
    for (int i = 0; (observer != nullptr) && (i < 100); ++i)
    {
        stats = Ask(*observer, identity::kStats);
        if (stats.has_value() && (stats->at(0) >= 1U) && (stats->at(1) >= 2U))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    Check(stats.has_value(), "stats available");
    if (stats.has_value())
    {
        std::cout << "stats: denied=" << stats->at(0) << " allowed_nodes=" << stats->at(1)
                  << " denied_principals=" << stats->at(2) << " denied_principal=" << stats->at(3) << std::endl;
        Check(stats->at(0) >= 1U, "the consumer routed as 8 was denied");
        Check(stats->at(1) == 2U, "process A and process C are two nodes (reconnects of A did not add nodes)");
        Check(stats->at(2) == 1U && stats->at(3) == identity::kDeniedConsumer, "only principal 8 was denied");
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string role = (argc > 1) ? argv[1] : "";
    int result{1};
    if (role == "--role=allowed")
    {
        result = RunAllowed();
    }
    else if (role == "--role=denied")
    {
        result = RunDenied();
    }
    else if (role == "--role=test")
    {
        result = RunTest();
    }
    std::cout << "identity " << role << (result == 0 ? " OK" : " FAILED") << std::endl;
    return result == 0 ? 0 : 1;
}
