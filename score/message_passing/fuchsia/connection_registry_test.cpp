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

    Check(!registry.Enqueue(kIdentifier, -1), "no listener remains after a failed Listen");
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

    if (failures == 0)
    {
        std::cout << "registry start failure OK" << std::endl;
    }
    return failures == 0 ? 0 : 1;
}
