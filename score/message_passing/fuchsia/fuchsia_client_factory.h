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
#ifndef SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_CLIENT_FACTORY_H
#define SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_CLIENT_FACTORY_H

#include "score/message_passing/i_client_factory.h"

namespace score::message_passing
{

class FuchsiaEngine;

class FuchsiaClientFactory final : public IClientFactory
{
  public:
    explicit FuchsiaClientFactory(
        score::cpp::pmr::memory_resource* const resource = score::cpp::pmr::get_default_resource()) noexcept;
    explicit FuchsiaClientFactory(const std::shared_ptr<FuchsiaEngine> engine) noexcept;
    ~FuchsiaClientFactory() noexcept;

    FuchsiaClientFactory(const FuchsiaClientFactory&) = delete;
    FuchsiaClientFactory& operator=(const FuchsiaClientFactory&) = delete;
    FuchsiaClientFactory(FuchsiaClientFactory&&) = delete;
    FuchsiaClientFactory& operator=(FuchsiaClientFactory&&) = delete;

    score::cpp::pmr::unique_ptr<IClientConnection> Create(const ServiceProtocolConfig& protocol_config,
                                                          const ClientConfig& client_config) noexcept override;

    std::shared_ptr<FuchsiaEngine> GetEngine() const noexcept
    {
        return engine_;
    }

  private:
    const std::shared_ptr<FuchsiaEngine> engine_;
};

}  // namespace score::message_passing

#endif  // SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_CLIENT_FACTORY_H
