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
#include "score/mw/com/impl/bindings/lola/fuchsia/broker_service.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <cstdio>

// Broker component: serves score.mw.com.Broker to the skeleton and to the proxies.
int main()
{
    async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
    component::OutgoingDirectory outgoing{loop.dispatcher()};
    score::mw::com::impl::lola::fuchsia::BrokerService broker{loop.dispatcher()};
    if (outgoing.AddUnmanagedProtocol<score_mw_com::Broker>(broker.CreateHandler()).is_error() ||
        outgoing.ServeFromStartupInfo().is_error())
    {
        return 1;
    }
    std::printf("broker: running\n");
    loop.Run();
    return 0;
}
