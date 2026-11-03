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
 ********************************************************************************/
#ifndef QUALITY_INTEGRATION_TESTING_TEST_FEMU_LOLA_WHEELSPEED_WHEEL_SPEED_INTERFACE_H
#define QUALITY_INTEGRATION_TESTING_TEST_FEMU_LOLA_WHEELSPEED_WHEEL_SPEED_INTERFACE_H

#include "score/mw/com/types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Hand-written stand-in for the output of the inc_ecu_model generator (not wired in this repository): a plain C++
// payload type and a service interface in the shape that the generator produces (AsProxy / AsSkeleton templates).
namespace wheelspeed
{

struct WheelSpeed
{
    std::uint32_t incarnation;  // identifies one run of the skeleton; the sequence restarts with every incarnation
    std::uint32_t sequence;
    std::int64_t publish_time_ns;    // zx_clock_get_monotonic of the skeleton, comparable across processes
    std::array<float, 4> wheel_rpm;  // front left, front right, rear left, rear right
    std::uint32_t checksum;
};

constexpr std::uint32_t ExpectedChecksum(const WheelSpeed& sample) noexcept
{
    return (sample.incarnation * 31U) ^ (sample.sequence * 2654435761U) ^ 0xA5A5A5A5U;
}

// Deterministic payload so that the proxy can verify the content, not just the sequence.
constexpr float ExpectedRpm(const std::uint32_t sequence, const std::size_t wheel) noexcept
{
    return static_cast<float>(sequence % 1000U) + static_cast<float>(wheel) * 0.25F;
}

template <typename T>
class WheelSpeedInterface : public T::Base
{
  public:
    using T::Base::Base;

    typename T::template Event<WheelSpeed> wheel_speed{*this, "wheel_speed"};
};

using WheelSpeedProxy = score::mw::com::AsProxy<WheelSpeedInterface>;
using WheelSpeedSkeleton = score::mw::com::AsSkeleton<WheelSpeedInterface>;

constexpr const char* kInstanceSpecifier = "wheelspeed/WheelSpeed";
constexpr std::uint64_t kServiceId = 3429U;
constexpr std::uint64_t kSkeletonAppId = 1000U;
constexpr std::uint64_t kProxyAppId = 4002U;

}  // namespace wheelspeed

#endif  // QUALITY_INTEGRATION_TESTING_TEST_FEMU_LOLA_WHEELSPEED_WHEEL_SPEED_INTERFACE_H
