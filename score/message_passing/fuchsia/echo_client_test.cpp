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

#include <array>
#include <chrono>
#include <future>
#include <iostream>

// Connects to the echo server running in a different component of the same test realm.
int main()
{
    using namespace score::message_passing;

    ClientFactory factory{};
    ServiceProtocolConfig protocol_config{"cross_component_echo", 1024, 1024, 1024};
    auto client = factory.Create(protocol_config, IClientFactory::ClientConfig{1, 1, false, true, false});
    std::promise<void> ready;
    client->Start(
        [&](auto state) {
            if (state == IClientConnection::State::kReady)
            {
                ready.set_value();
            }
        },
        IClientConnection::NotifyCallback{});
    if (ready.get_future().wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    {
        std::cout << "client not ready" << std::endl;
        return 1;
    }
    const std::array<std::uint8_t, 4> message{1, 2, 3, 4};
    std::array<std::uint8_t, 16> buffer{};
    const auto reply = client->SendWaitReply(message, buffer);
    if (!reply.has_value() || reply.value().size() != message.size())
    {
        std::cout << "reply failed" << std::endl;
        return 2;
    }
    std::cout << "cross component echo OK" << std::endl;
    return 0;
}
