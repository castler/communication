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
#include "score/message_passing/fuchsia/fuchsia_server_factory.h"

#include "score/message_passing/fuchsia/fuchsia_engine.h"
#include "score/message_passing/fuchsia/fuchsia_server.h"

namespace score::message_passing
{

FuchsiaServerFactory::FuchsiaServerFactory(score::cpp::pmr::memory_resource* const resource) noexcept
    : FuchsiaServerFactory{score::cpp::pmr::make_shared<FuchsiaEngine>(resource, resource)}
{
}

FuchsiaServerFactory::FuchsiaServerFactory(const std::shared_ptr<FuchsiaEngine> engine) noexcept : engine_{engine} {}

FuchsiaServerFactory::~FuchsiaServerFactory() noexcept = default;

score::cpp::pmr::unique_ptr<IServer> FuchsiaServerFactory::Create(const ServiceProtocolConfig& protocol_config,
                                                                  const ServerConfig& server_config) noexcept
{
    return score::cpp::pmr::make_unique<detail::FuchsiaServer>(
        engine_->GetMemoryResource(), engine_, protocol_config, server_config);
}

}  // namespace score::message_passing
