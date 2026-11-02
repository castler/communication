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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_VMO_ROLE_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_VMO_ROLE_H

#include <zircon/rights.h>

#include <functional>
#include <optional>
#include <string>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Attach carries exactly one data and one control region per endpoint (README 6.3); the keys of the
///        IVmoHandleProvider interface are mapped to these roles.
enum class VmoRole
{
    kData,
    kControl,
};

using KeyClassifier = std::function<std::optional<VmoRole>(const std::string& key)>;

/// \brief Maps the LoLa shm names ("/lola-data-...", "/lola-ctl-...", see ShmPathBuilder) to their role. Other keys
///        (e.g. method channels) cannot be carried by Attach and yield nullopt.
std::optional<VmoRole> DefaultKeyClassifier(const std::string& key) noexcept;

/// \brief The control region is the one that proxies write.
inline bool IsConsumerWritable(const std::string& key) noexcept
{
    return DefaultKeyClassifier(key) == VmoRole::kControl;
}

/// \brief Rights of the handles as declared in score_mw_com.fidl; the sender duplicates with these (plus TRANSFER, which
///        the kernel needs to send a handle but which is not part of the declared rights, so the receiver never gets it).
constexpr zx_rights_t kDataRights = ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_GET_PROPERTY;
constexpr zx_rights_t kControlRights = ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_MAP | ZX_RIGHT_GET_PROPERTY;
constexpr zx_rights_t kLivenessRights = ZX_RIGHT_WAIT;

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_VMO_ROLE_H
