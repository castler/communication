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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_NAME_BASED_NODE_ADDRESS_RESOLVER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_NAME_BASED_NODE_ADDRESS_RESOLVER_H

#include "score/mw/com/impl/bindings/lola/messaging/i_node_address_resolver.h"

namespace score::mw::com::impl::lola
{

/// \brief Resolver for Linux/QNX: the server of a node is named after the pid the NodeId was created from.
/// \details Every NodeId resolves; whether a server is actually listening is only found out when connecting.
class NameBasedNodeAddressResolver final : public INodeAddressResolver
{
  public:
    std::optional<std::string> Resolve(const ClientQualityType asil_level,
                                       const score::message_passing::NodeId node_id) const noexcept override;

    static std::string CreateName(const ClientQualityType asil_level,
                                  const score::message_passing::NodeId node_id) noexcept;
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_NAME_BASED_NODE_ADDRESS_RESOLVER_H
