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
#include "score/message_passing/i_server_connection.h"
#include "score/message_passing/server_factory.h"

#include <chrono>
#include <iostream>
#include <thread>

// Echoes every request and send, until the component is stopped. Clients reach it through the Connector protocol
// exposed in the outgoing directory by the Fuchsia message_passing backend.
int main()
{
    using namespace score::message_passing;

    ServerFactory factory{};
    ServiceProtocolConfig protocol_config{"cross_component_echo", 1024, 1024, 1024};
    auto server = factory.Create(protocol_config, IServerFactory::ServerConfig{});
    auto echo = [](IServerConnection& connection, score::cpp::span<const std::uint8_t> message) -> score::cpp::blank {
        connection.Reply(message);
        return {};
    };
    const auto listening = server->StartListening(
        [](IServerConnection&) -> void* {
            return nullptr;
        },
        [](IServerConnection&) {},
        echo,
        echo);
    if (!listening.has_value())
    {
        std::cerr << "echo_server: listen failed" << std::endl;
        return 1;
    }
    std::cout << "echo_server: listening" << std::endl;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
