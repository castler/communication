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
#include "score/message_passing/i_server_connection.h"
#include "score/message_passing/server_factory.h"
#include "score/message_passing/server_types.h"
#include "score/message_passing/fuchsia/connection_registry.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

// Provider with the allowed-consumer check of LoLa (ClientIdentity::IsAllowed): consumer 7 is allowed, 8 is not.
// The Connector endpoints routed to the consumers are published like the LoLa Runtime does for the configured
// allowed consumers (here additionally for the denied consumer 8, to have it routed).
int main()
{
    using namespace score::message_passing;
    namespace identity = score::mw::com::identity_test;

    auto& registry = detail::FuchsiaConnectionRegistry::Instance();
    registry.ServeRoutedPrincipal(identity::kAllowedConsumer);
    registry.ServeRoutedPrincipal(identity::kDeniedConsumer);

    struct State
    {
        std::mutex mutex;
        std::set<std::uint32_t> allowed_nodes;
        std::set<std::uint32_t> denied_principals;
        std::uint32_t denied{0U};
        ClientIdentity::AllowedPrincipals allowed_list{
            std::set<ClientIdentity::PrincipalId>{ClientIdentity::PrincipalId{identity::kAllowedConsumer}}};
    };
    State state;
    State* const s = &state;  // a single pointer fits the small callback storage

    ServerFactory factory{};
    ServiceProtocolConfig protocol_config{identity::kProtocolName, 1024, 1024, 1024};
    auto server = factory.Create(protocol_config, IServerFactory::ServerConfig{});

    auto handle = [s](IServerConnection& connection, score::cpp::span<const std::uint8_t> request) -> score::cpp::blank {
        std::lock_guard<std::mutex> guard{s->mutex};
        const auto principal = static_cast<std::uint32_t>(connection.GetClientIdentity().GetPrincipalId().Raw());
        const auto node = static_cast<std::uint32_t>(connection.GetClientIdentity().GetNodeId().Raw());
        if (!request.empty() && (request[0] == identity::kWhoAmI))
        {
            connection.Reply(identity::Encode({principal, node}));
        }
        else
        {
            connection.Reply(identity::Encode({s->denied,
                                               static_cast<std::uint32_t>(s->allowed_nodes.size()),
                                               static_cast<std::uint32_t>(s->denied_principals.size()),
                                               s->denied_principals.empty() ? 0U : *s->denied_principals.begin()}));
        }
        return {};
    };
    const auto listening = server->StartListening(
        [s](IServerConnection& connection) -> score::cpp::expected<UserData, score::os::Error> {
            std::lock_guard<std::mutex> guard{s->mutex};
            const auto& client = connection.GetClientIdentity();
            if (!client.IsAllowed(s->allowed_list))
            {
                ++s->denied;
                s->denied_principals.insert(static_cast<std::uint32_t>(client.GetPrincipalId().Raw()));
                return score::cpp::make_unexpected(score::os::Error::createFromErrno(EACCES));
            }
            s->allowed_nodes.insert(static_cast<std::uint32_t>(client.GetNodeId().Raw()));
            return UserData{static_cast<void*>(nullptr)};
        },
        [](IServerConnection&) {},
        handle,
        handle);
    if (!listening.has_value())
    {
        std::cerr << "identity_server: listen failed" << std::endl;
        return 1;
    }
    std::cout << "identity_server: listening" << std::endl;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
