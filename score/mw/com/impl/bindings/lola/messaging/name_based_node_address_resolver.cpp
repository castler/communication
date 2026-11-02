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
#include "score/mw/com/impl/bindings/lola/messaging/name_based_node_address_resolver.h"

#include <sstream>

namespace score::mw::com::impl::lola
{

namespace
{
constexpr auto mq_name_prefix("LoLa_2_");
constexpr auto mq_name_qm_postfix("_QM");
constexpr auto mq_name_asil_b_postfix("_ASIL_B");
}  // namespace

std::optional<std::string> NameBasedNodeAddressResolver::Resolve(
    const ClientQualityType asil_level,
    const score::message_passing::NodeId node_id) const noexcept
{
    return CreateName(asil_level, node_id);
}

std::string NameBasedNodeAddressResolver::CreateName(const ClientQualityType asil_level,
                                                     const score::message_passing::NodeId node_id) noexcept
{
    std::stringstream identifier;
    identifier << mq_name_prefix << score::message_passing::NodeIdToPid(node_id);
    if (asil_level != ClientQualityType::kASIL_B)
    {
        identifier << mq_name_qm_postfix;
    }
    else
    {
        identifier << mq_name_asil_b_postfix;
    }
    return identifier.str();
}

}  // namespace score::mw::com::impl::lola
