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
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_role.h"

#include <string_view>

namespace score::mw::com::impl::lola::fuchsia
{

auto DefaultKeyClassifier(const std::string& key) noexcept -> std::optional<VmoRole>
{
    if (key.find("lola-data-") != std::string::npos)
    {
        return VmoRole::kData;
    }
    if (key.find("lola-ctl-") != std::string::npos)
    {
        // ShmPathBuilder appends "-b" to the QM control path for the ASIL-B control region.
        constexpr std::string_view kAsilBSuffix{"-b"};
        if ((key.size() >= kAsilBSuffix.size()) &&
            (key.compare(key.size() - kAsilBSuffix.size(), kAsilBSuffix.size(), kAsilBSuffix) == 0))
        {
            return VmoRole::kControlAsilB;
        }
        return VmoRole::kControl;
    }
    return std::nullopt;
}

}  // namespace score::mw::com::impl::lola::fuchsia
