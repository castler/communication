#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_ASIL_SPECIFIC_CFG_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_ASIL_SPECIFIC_CFG_H

#include "score/message_passing/identity_types.h"

#include <unistd.h>

#include <cstdint>
#include <vector>

namespace score::mw::com::impl::lola
{

/// \brief Aggregation of ASIL level specific/dependent config properties.
struct AsilSpecificCfg
{
    // Suppress "AUTOSAR C++14 M11-0-1" rule findings. This rule states: "Member data in non-POD class types shall
    // be private.". We need these data elements to be organized into a coherent organized data structure.
    // coverity[autosar_cpp14_m11_0_1_violation]
    std::int32_t message_queue_rx_size_;
    // coverity[autosar_cpp14_m11_0_1_violation]
    std::vector<score::message_passing::PrincipalId> allowed_user_ids_;
};
}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_ASIL_SPECIFIC_CFG_H
