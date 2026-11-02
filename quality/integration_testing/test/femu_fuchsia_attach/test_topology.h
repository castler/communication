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
#ifndef QUALITY_INTEGRATION_TESTING_TEST_FEMU_FUCHSIA_ATTACH_TEST_TOPOLOGY_H
#define QUALITY_INTEGRATION_TESTING_TEST_FEMU_FUCHSIA_ATTACH_TEST_TOPOLOGY_H

#include <cstdint>

namespace attach_test
{

constexpr std::uint64_t kServiceId = 0x1234U;
constexpr std::uint32_t kInstanceId = 1U;
constexpr std::uint64_t kSkeletonAppId = 1000U;
constexpr std::uint64_t kProxyAppId = 4002U;
constexpr std::uint64_t kRogueAppId = 4003U;
constexpr std::uint64_t kEpoch = 7U;

constexpr const char* kDataPath = "/lola-data-0000000000004660-00001";
constexpr const char* kControlPath = "/lola-ctl-0000000000004660-00001";
constexpr std::uint32_t kMagic = 0x4c4f4c41U;
constexpr std::uint64_t kCounter = 4711U;
constexpr std::uint64_t kRegionSize = 4096U;

constexpr int kOfferDelaySeconds = 4;

}  // namespace attach_test

#endif  // QUALITY_INTEGRATION_TESTING_TEST_FEMU_FUCHSIA_ATTACH_TEST_TOPOLOGY_H
