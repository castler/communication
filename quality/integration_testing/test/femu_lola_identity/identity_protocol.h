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
#ifndef QUALITY_INTEGRATION_TESTING_TEST_FEMU_LOLA_IDENTITY_IDENTITY_PROTOCOL_H
#define QUALITY_INTEGRATION_TESTING_TEST_FEMU_LOLA_IDENTITY_IDENTITY_PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace score::mw::com::identity_test
{

constexpr const char* kProtocolName = "identity_probe";
constexpr std::uint32_t kAllowedConsumer = 7U;
constexpr std::uint32_t kDeniedConsumer = 8U;

constexpr std::uint8_t kWhoAmI = 1U;  // reply: principal, node as seen by the server
constexpr std::uint8_t kStats = 2U;   // reply: denied connections, allowed nodes, denied principals, a denied principal

inline std::vector<std::uint8_t> Encode(const std::vector<std::uint32_t>& values)
{
    std::vector<std::uint8_t> bytes(values.size() * sizeof(std::uint32_t));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

template <typename Span>
inline std::optional<std::vector<std::uint32_t>> Decode(const Span& bytes)
{
    if ((bytes.size() % sizeof(std::uint32_t)) != 0U)
    {
        return std::nullopt;
    }
    std::vector<std::uint32_t> values(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

}  // namespace score::mw::com::identity_test

#endif  // QUALITY_INTEGRATION_TESTING_TEST_FEMU_LOLA_IDENTITY_IDENTITY_PROTOCOL_H
