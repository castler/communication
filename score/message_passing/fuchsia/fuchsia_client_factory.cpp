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
#include "score/message_passing/fuchsia/fuchsia_client_factory.h"

#include "score/message_passing/client_connection.h"
#include "score/message_passing/fuchsia/fuchsia_engine.h"

namespace score::message_passing
{

FuchsiaClientFactory::FuchsiaClientFactory(score::cpp::pmr::memory_resource* const resource,
                                           const std::uint32_t consumer_id) noexcept
    : FuchsiaClientFactory{
          score::cpp::pmr::make_shared<FuchsiaEngine>(resource, resource, GetCerrLogger(), consumer_id)}
{
}

FuchsiaClientFactory::FuchsiaClientFactory(const std::shared_ptr<FuchsiaEngine> engine) noexcept : engine_{engine} {}

FuchsiaClientFactory::~FuchsiaClientFactory() noexcept = default;

score::cpp::pmr::unique_ptr<IClientConnection> FuchsiaClientFactory::Create(
    const ServiceProtocolConfig& protocol_config,
    const ClientConfig& client_config) noexcept
{
    return score::cpp::pmr::make_unique<detail::ClientConnection>(
        engine_->GetMemoryResource(), engine_, protocol_config, client_config);
}

}  // namespace score::message_passing
