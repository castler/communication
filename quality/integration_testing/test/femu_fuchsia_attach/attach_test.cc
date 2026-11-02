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
#include "quality/integration_testing/test/femu_fuchsia_attach/test_topology.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_role.h"

#include <fidl/score.mw.com.test/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>

#include <cerrno>
#include <cstdio>
#include <optional>

// Driver (realm root). Children: discovery, skeleton, proxy (routed, application 4002) and rogue (application 4003, no
// Endpoint routed, not allowed to see the service). The driver asks both consumers what they observed.

namespace
{
using namespace attach_test;

int failures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

std::optional<score_mw_com_test::ProbeResult> Run(const char* path, const char* name)
{
    auto end = component::Connect<score_mw_com_test::Probe>(path);
    if (end.is_error())
    {
        std::printf("FAIL cannot connect %s\n", name);
        ++failures;
        return std::nullopt;
    }
    fidl::SyncClient<score_mw_com_test::Probe> client{std::move(*end)};
    auto result = client->Run();
    if (result.is_error())
    {
        std::printf("FAIL Probe.Run on %s: %s\n", name, result.error_value().FormatDescription().c_str());
        ++failures;
        return std::nullopt;
    }
    const auto& r = result->result();
    std::printf(
        "%s: find=%u first_watch=%u watch=%u attach_errno=%d data_rights=0x%x control_rights=0x%x app=%lu epoch=%lu "
        "discovery_epoch=%lu data_read_ok=%d data_write_denied=%d control_write_ok=%d\n",
        name,
        r.find_count(),
        r.first_watch_count(),
        r.watch_count(),
        r.attach_errno(),
        r.data_rights(),
        r.control_rights(),
        static_cast<unsigned long>(r.application_id()),
        static_cast<unsigned long>(r.epoch()),
        static_cast<unsigned long>(r.discovery_epoch()),
        r.data_read_ok(),
        r.data_write_denied(),
        r.control_write_ok());
    return r;
}
}  // namespace

int main()
{
    const auto proxy = Run("/svc/probe_proxy", "proxy");
    const auto rogue = Run("/svc/probe_rogue", "rogue");
    if (proxy.has_value())
    {
        // The offer comes after the first Watch (which returned at once, empty); the second Watch was a hanging get.
        CHECK(proxy->first_watch_count() == 0U);
        CHECK(proxy->watch_count() == 1U);
        CHECK(proxy->find_count() <= 1U);
        CHECK(proxy->attach_errno() == 0);
        CHECK(proxy->application_id() == kProxyAppId);
        CHECK(proxy->epoch() == kEpoch);
        CHECK(proxy->discovery_epoch() == kEpoch);
        CHECK(proxy->data_rights() == score::mw::com::impl::lola::fuchsia::kDataRights);
        CHECK(proxy->control_rights() == score::mw::com::impl::lola::fuchsia::kControlRights);
        CHECK(proxy->data_write_denied());
        CHECK(proxy->control_write_ok());
        CHECK(proxy->data_read_ok());
    }
    if (rogue.has_value())
    {
        // Sees nothing (discovery is filtered per caller) and the Endpoint is not routed to it.
        CHECK(rogue->first_watch_count() == 0U);
        CHECK(rogue->watch_count() == 0U);
        CHECK(rogue->find_count() == 0U);
        CHECK(rogue->attach_errno() == ECONNRESET || rogue->attach_errno() == ENOENT);
        CHECK(rogue->application_id() == 0U);
        CHECK(!rogue->data_read_ok());
    }
    std::printf("FUCHSIA_ATTACH_TEST %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
