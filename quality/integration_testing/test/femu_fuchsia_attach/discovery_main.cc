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

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Discovery component. Arguments (generated from the mw::com configuration, like the realm):
//   --skeleton=<application id>:<service id>[,<service id>...]   serves score.mw.com.registry.for_app_<id>
//   --consumer=<application id>:<service id>[,<service id>...]   serves score.mw.com.discovery.for_app_<id>
// The realm renames the per-caller capability to score.mw.com.ServiceRegistry / score.mw.com.ServiceDiscovery.

namespace
{
using score::mw::com::impl::lola::fuchsia::DiscoveryService;

bool ParseParticipant(const std::string& value, DiscoveryService::Participant& participant)
{
    const auto colon = value.find(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    participant.application_id = std::strtoull(value.substr(0, colon).c_str(), nullptr, 0);
    std::size_t begin = colon + 1U;
    while (begin < value.size())
    {
        auto end = value.find(',', begin);
        if (end == std::string::npos)
        {
            end = value.size();
        }
        participant.service_ids.insert(std::strtoull(value.substr(begin, end - begin).c_str(), nullptr, 0));
        begin = end + 1U;
    }
    return true;
}
}  // namespace

int main(int argc, char** argv)
{
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    DiscoveryService service{loop.dispatcher()};
    component::OutgoingDirectory outgoing{loop.dispatcher()};

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument{argv[i]};
        DiscoveryService::Participant participant{};
        const bool is_skeleton = argument.rfind("--skeleton=", 0) == 0;
        const bool is_consumer = argument.rfind("--consumer=", 0) == 0;
        if ((!is_skeleton && !is_consumer) || !ParseParticipant(argument.substr(argument.find('=') + 1U), participant))
        {
            std::printf("discovery: bad argument %s\n", argument.c_str());
            return 1;
        }
        const std::string id = std::to_string(participant.application_id);
        if (is_skeleton)
        {
            auto result = outgoing.AddUnmanagedProtocol<score_mw_com::ServiceRegistry>(
                service.RegistryHandler(participant), "score.mw.com.registry.for_app_" + id);
            if (result.is_error())
            {
                return 2;
            }
        }
        else
        {
            auto result = outgoing.AddUnmanagedProtocol<score_mw_com::ServiceDiscovery>(
                service.DiscoveryHandler(participant), "score.mw.com.discovery.for_app_" + id);
            if (result.is_error())
            {
                return 2;
            }
        }
    }
    if (outgoing.ServeFromStartupInfo().is_error())
    {
        return 3;
    }
    std::printf("discovery: serving\n");
    loop.Run();
    return 0;
}
