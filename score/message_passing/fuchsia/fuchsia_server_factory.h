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
#ifndef SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_SERVER_FACTORY_H
#define SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_SERVER_FACTORY_H

#include "score/message_passing/i_server_factory.h"

namespace score::message_passing
{

class FuchsiaEngine;

class FuchsiaServerFactory final : public IServerFactory
{
  public:
    explicit FuchsiaServerFactory(
        score::cpp::pmr::memory_resource* const resource = score::cpp::pmr::get_default_resource()) noexcept;
    explicit FuchsiaServerFactory(const std::shared_ptr<FuchsiaEngine> engine) noexcept;
    ~FuchsiaServerFactory() noexcept;

    FuchsiaServerFactory(const FuchsiaServerFactory&) = delete;
    FuchsiaServerFactory& operator=(const FuchsiaServerFactory&) = delete;
    FuchsiaServerFactory(FuchsiaServerFactory&&) = delete;
    FuchsiaServerFactory& operator=(FuchsiaServerFactory&&) = delete;

    score::cpp::pmr::unique_ptr<IServer> Create(const ServiceProtocolConfig& protocol_config,
                                                const ServerConfig& server_config) noexcept override;

    std::shared_ptr<FuchsiaEngine> GetEngine() const noexcept
    {
        return engine_;
    }

  private:
    const std::shared_ptr<FuchsiaEngine> engine_;
};

}  // namespace score::message_passing

#endif  // SCORE_LIB_MESSAGE_PASSING_FUCHSIA_FUCHSIA_SERVER_FACTORY_H
