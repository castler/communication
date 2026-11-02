/*******************************************************************************
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
 *******************************************************************************/

// Build-only check: links the LoLa runtime, skeleton and proxy for a generated service into a Fuchsia binary.
#include "score/mw/com/runtime.h"
#include "score/string_manipulation/arguments/arguments.h"
#include "score/mw/com/types.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
template <typename T>
class TestInterface : public T::Base
{
  public:
    using T::Base::Base;

    typename T::template Field<std::int32_t, score::mw::com::WithNotifier> test_field{*this, "test_field"};
};

using TestProxy = score::mw::com::AsProxy<TestInterface>;
using TestSkeleton = score::mw::com::AsSkeleton<TestInterface>;
}  // namespace

int main(int argc, const char* argv[])
{
    auto args = score::string_manipulation::GetArguments(argc, argv);
    score::mw::com::runtime::InitializeRuntime(score::cpp::span<score::safecpp::zstring_view>{args});

    auto specifier = score::mw::com::InstanceSpecifier::Create(std::string{"test/femu_lola_build"});
    if (!specifier.has_value())
    {
        return 1;
    }

    auto skeleton = TestSkeleton::Create(specifier.value());
    if (!skeleton.has_value())
    {
        std::puts("skeleton creation failed");
        return 2;
    }
    const auto offered = skeleton.value().OfferService();
    const auto found = TestProxy::FindService(specifier.value());
    std::printf("offer=%d find=%d\n", offered.has_value() ? 1 : 0, found.has_value() ? 1 : 0);
    return 0;
}
