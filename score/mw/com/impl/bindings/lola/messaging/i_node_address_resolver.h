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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_I_NODE_ADDRESS_RESOLVER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_I_NODE_ADDRESS_RESOLVER_H

#include "score/mw/com/impl/bindings/lola/messaging/client_quality_type.h"

#include "score/message_passing/identity_types.h"

#include <optional>
#include <string>

namespace score::mw::com::impl::lola
{

/// \brief Maps the identity of a peer (NodeId) to the address under which its message passing server can be reached.
/// \details The NodeId is opaque. How it translates to an address is a platform decision: On Linux/QNX the address
/// is a name derived from the pid, on Fuchsia a lookup at the broker/discovery service is needed. A NodeId which the
/// platform does not (or no longer) know cannot be resolved; callers must treat this as "peer is gone".
class INodeAddressResolver
{
  public:
    INodeAddressResolver() = default;
    virtual ~INodeAddressResolver() = default;

    INodeAddressResolver(const INodeAddressResolver&) = delete;
    INodeAddressResolver(INodeAddressResolver&&) = delete;
    INodeAddressResolver& operator=(const INodeAddressResolver&) = delete;
    INodeAddressResolver& operator=(INodeAddressResolver&&) = delete;

    /// \return service identifier of the message passing server of the given node or std::nullopt, if the node
    ///         cannot be resolved.
    virtual std::optional<std::string> Resolve(const ClientQualityType asil_level,
                                               const score::message_passing::NodeId node_id) const noexcept = 0;
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_I_NODE_ADDRESS_RESOLVER_H
