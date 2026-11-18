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
#include "score/message_passing/fuchsia/connection_registry.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <iostream>

using score::message_passing::detail::FuchsiaConnectionRegistry;

// Failure injection test: a failing start of the Connector service must make Listen fail without leaving a listener
// or leaked descriptors behind. Plain main() as the other Fuchsia tests, googletest does not build for Fuchsia here.

namespace
{

constexpr auto kIdentifier = "registry_start_failure";

int failures{0};

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cout << "FAILED: " << what << std::endl;
        ++failures;
    }
}

std::array<std::int32_t, 2> OpenPipe()
{
    std::array<std::int32_t, 2> fds{-1, -1};
    Check(::pipe(fds.data()) == 0, "pipe");
    return fds;
}

void ClosePipe(const std::array<std::int32_t, 2>& fds)
{
    ::close(fds[0]);
    ::close(fds[1]);
}

}  // namespace

int main()
{
    bool allow_start{false};
    FuchsiaConnectionRegistry registry{[&allow_start]() { return allow_start; }};

    // The lowest free descriptors are reused, so they reveal whether Listen leaked its pipe ends
    const auto probe = OpenPipe();
    ClosePipe(probe);

    const auto listen = registry.Listen(kIdentifier);
    Check(!listen.has_value(), "Listen fails if the service cannot be started");

    const auto after = OpenPipe();
    Check(after == probe, "Listen closes both pipe ends on failure");
    ClosePipe(after);

    Check(!registry.Enqueue(kIdentifier, -1, 0U, 0U), "no listener remains after a failed Listen");
    Check(!registry.Accept(kIdentifier).has_value(), "nothing to accept after a failed Listen");

    // A retry is not rejected as a duplicate and succeeds once the service can be started
    allow_start = true;
    const auto retry = registry.Listen(kIdentifier);
    Check(retry.has_value(), "Listen succeeds after the service can be started");
    if (retry.has_value())
    {
        registry.StopListening(kIdentifier);
        ::close(retry.value());
    }

    // Identity of connections within one process: the node is per local principal, the principal is the given one
    const auto local_listen = registry.Listen("registry_local_identity");
    Check(local_listen.has_value(), "Listen for local identities");
    const auto connect_as = [&registry](const std::uint32_t principal) {
        const auto client = registry.Connect("registry_local_identity", principal);
        Check(client.has_value(), "local Connect");
        if (client.has_value())
        {
            ::close(client.value());
        }
        const auto accepted = registry.Accept("registry_local_identity");
        Check(accepted.has_value(), "local Accept");
        if (accepted.has_value())
        {
            ::close(accepted.value().fd);
            return accepted.value();
        }
        return FuchsiaConnectionRegistry::AcceptedConnection{-1, 0U, 0U, 0U};
    };
    const auto first_a = connect_as(11U);
    const auto second_a = connect_as(11U);
    const auto first_b = connect_as(22U);
    Check(first_a.principal_id == 11U && first_b.principal_id == 22U, "local principal is the given one");
    Check(first_a.node_id == second_a.node_id, "reconnect of the same local peer keeps the node");
    Check(first_a.node_id != first_b.node_id, "another local peer is another node");
    Check(first_a.connection_id != second_a.connection_id, "every connection has its own id");
    if (local_listen.has_value())
    {
        registry.StopListening("registry_local_identity");
        ::close(local_listen.value());
    }

    // The endpoint routed to a consumer is named after its principal; serving it twice is harmless
    Check(FuchsiaConnectionRegistry::RoutedConnectorName(7U) == "score.messagepassing.Connector.7", "endpoint name");
    registry.ServeRoutedPrincipal(7U);
    registry.ServeRoutedPrincipal(7U);
    Check(registry.NewPeerId() != registry.NewPeerId(), "peer ids are unique");

    if (failures == 0)
    {
        std::cout << "registry start failure OK" << std::endl;
    }
    return failures == 0 ? 0 : 1;
}
