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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_TEST_SUPPORT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_TEST_SUPPORT_H

#include "score/mw/com/impl/configuration/lola_service_instance_deployment.h"
#include "score/mw/com/impl/configuration/lola_service_type_deployment.h"
#include "score/mw/com/impl/configuration/service_identifier_type.h"
#include "score/mw/com/impl/configuration/service_instance_deployment.h"
#include "score/mw/com/impl/configuration/service_type_deployment.h"
#include "score/mw/com/impl/instance_specifier.h"
#include "score/mw/com/impl/enriched_instance_identifier.h"
#include "score/mw/com/impl/handle_type.h"
#include "score/mw/com/impl/instance_identifier.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Test support: a LoLa instance identifier for (service id, optional instance id, quality) without a
///        configuration file, so that integration test components can drive the IServiceDiscoveryClient directly.
class DiscoveryTestInstance
{
  public:
    DiscoveryTestInstance(const std::string& instance_specifier,
                          std::uint16_t service_id,
                          std::optional<std::uint16_t> instance_id,
                          QualityType quality);

    InstanceIdentifier GetInstanceIdentifier() const;
    EnrichedInstanceIdentifier GetEnrichedInstanceIdentifier() const;
    /// Handle of the given instance as the client reports it to a find handler.
    HandleType GetHandle(std::uint16_t instance_id) const;

  private:
    // Heap allocated: InstanceIdentifiers point to the deployments, their addresses must not change.
    ServiceIdentifierType service_identifier_;
    InstanceSpecifier instance_specifier_;
    LolaServiceTypeDeployment lola_type_deployment_;
    LolaServiceInstanceDeployment lola_instance_deployment_;
    std::unique_ptr<ServiceTypeDeployment> type_deployment_;
    std::unique_ptr<ServiceInstanceDeployment> instance_deployment_;
};

/// Instance id of a handle as delivered to a find handler.
std::uint16_t InstanceIdOf(const HandleType& handle);

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_DISCOVERY_TEST_SUPPORT_H
