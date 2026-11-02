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
#include "score/mw/com/impl/bindings/lola/fuchsia/discovery_test_support.h"

#include "score/mw/com/impl/configuration/service_identifier_type.h"

#include <utility>

namespace score::mw::com::impl::lola::fuchsia
{

DiscoveryTestInstance::DiscoveryTestInstance(const std::string& instance_specifier,
                                             const std::uint16_t service_id,
                                             const std::optional<std::uint16_t> instance_id,
                                             const QualityType quality)
    : service_identifier_{make_ServiceIdentifierType("lola_discovery_test")},
      instance_specifier_{InstanceSpecifier::Create(std::string{instance_specifier}).value()},
      lola_type_deployment_{LolaServiceId{service_id}},
      lola_instance_deployment_{instance_id.has_value()
                                    ? std::optional<LolaServiceInstanceId>{LolaServiceInstanceId{instance_id.value()}}
                                    : std::nullopt},
      type_deployment_{std::make_unique<ServiceTypeDeployment>(lola_type_deployment_)},
      instance_deployment_{std::make_unique<ServiceInstanceDeployment>(
          service_identifier_, lola_instance_deployment_, quality, instance_specifier_)}
{
}

InstanceIdentifier DiscoveryTestInstance::GetInstanceIdentifier() const
{
    return make_InstanceIdentifier(*instance_deployment_, *type_deployment_);
}

EnrichedInstanceIdentifier DiscoveryTestInstance::GetEnrichedInstanceIdentifier() const
{
    return EnrichedInstanceIdentifier{GetInstanceIdentifier()};
}

HandleType DiscoveryTestInstance::GetHandle(const std::uint16_t instance_id) const
{
    return make_HandleType(GetInstanceIdentifier(), ServiceInstanceId{LolaServiceInstanceId{instance_id}});
}

std::uint16_t InstanceIdOf(const HandleType& handle)
{
    return GetServiceInstanceIdBinding<LolaServiceInstanceId>(handle.GetInstanceId()).GetId();
}

}  // namespace score::mw::com::impl::lola::fuchsia
