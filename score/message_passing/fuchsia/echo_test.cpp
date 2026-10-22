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
#include "score/message_passing/client_factory.h"
#include "score/message_passing/server_factory.h"

#include "score/message_passing/i_server_connection.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>

using namespace score::message_passing;

// Echoes a message through a server and a client sharing one engine, on the Fuchsia message_passing backend.

int main()
{
    ServerFactory server_factory{};
    ClientFactory client_factory{server_factory.GetEngine()};
    ServiceProtocolConfig protocol_config{"fuchsia_probe", 1024, 1024, 1024};

    auto server = server_factory.Create(protocol_config, IServerFactory::ServerConfig{});
    auto reply_cb = [](IServerConnection& c, score::cpp::span<const std::uint8_t> m) -> score::cpp::blank {
        c.Reply(m);
        return {};
    };
    auto listen = server->StartListening(
        [](IServerConnection&) -> void* { return nullptr; }, [](IServerConnection&) {}, reply_cb, reply_cb);
    std::cout << "listen: " << listen.has_value() << std::endl;
    if (!listen.has_value())
    {
        std::cout << "listen error: " << listen.error().ToString() << std::endl;
        return 1;
    }

    auto client = client_factory.Create(protocol_config, IClientFactory::ClientConfig{1, 1, false, true, false});
    std::promise<void> ready;
    client->Start(
        [&](auto state) {
            if (state == IClientConnection::State::kReady)
            {
                ready.set_value();
            }
        },
        IClientConnection::NotifyCallback{});
    if (ready.get_future().wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        std::cout << "client not ready" << std::endl;
        return 2;
    }
    std::array<std::uint8_t, 4> msg{1, 2, 3, 4};
    std::array<std::uint8_t, 16> buf{};
    auto r = client->SendWaitReply(msg, buf);
    if (!r.has_value() || r.value().size() != msg.size())
    {
        std::cout << "reply failed" << std::endl;
        return 3;
    }
    std::cout << "echo OK" << std::endl;
    client.reset();
    server->StopListening();
    server.reset();
    return 0;
}
