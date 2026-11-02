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

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/vmo.h>

#include <cstdio>
#include <string>
#include <vector>

// Does FIDL (fidlc declared `zx.Handle:<VMO, rights>` + the C++ bindings) reduce or reject handle rights on receive?
// A deliberately misbehaving in-process Endpoint server sends handles with more or fewer rights than declared; the
// client prints what it observes. Checked below are the results that the design relies on.

namespace
{
using score::mw::com::impl::lola::fuchsia::kControlRights;
using score::mw::com::impl::lola::fuchsia::kDataRights;
using score::mw::com::impl::lola::fuchsia::kLivenessRights;

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

struct Scenario
{
    const char* name;
    // ZX_RIGHT_SAME_RIGHTS keeps whatever the freshly created handle has.
    zx_rights_t data;
    zx_rights_t control;
    zx_rights_t liveness;
};

class RogueServer final : public fidl::Server<score_mw_com::Endpoint>
{
  public:
    explicit RogueServer(const Scenario& scenario) : scenario_{scenario} {}

    void Attach(AttachRequest&, AttachCompleter::Sync& completer) override
    {
        zx::vmo data;
        zx::vmo control;
        zx::vmo::create(4096U, 0U, &data);
        zx::vmo::create(4096U, 0U, &control);
        zx::vmo data_out;
        zx::vmo control_out;
        zx::eventpair local;
        zx::eventpair remote;
        zx::eventpair::create(0U, &local, &remote);
        zx::eventpair remote_out;
        data.duplicate(scenario_.data, &data_out);
        control.duplicate(scenario_.control, &control_out);
        remote.duplicate(scenario_.liveness, &remote_out);
        kept_.push_back(std::move(local));
        completer.Reply(fit::ok(score_mw_com::EndpointAttachResponse{
            std::move(data_out), std::move(control_out), std::vector<std::uint8_t>{}, 1U, 2U, std::move(remote_out)}));
    }

  private:
    Scenario scenario_;
    std::vector<zx::eventpair> kept_;
};

template <typename Object>
zx_rights_t RightsOf(const Object& object)
{
    zx_info_handle_basic_t info{};
    object.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    return info.rights;
}

struct Outcome
{
    bool ok;
    std::string description;
    zx_rights_t data;
    zx_rights_t control;
    zx_rights_t liveness;
};

Outcome Run(async::Loop& loop, const Scenario& scenario)
{
    auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
    auto server = std::make_unique<RogueServer>(scenario);
    fidl::BindServer(loop.dispatcher(), std::move(endpoints->server), std::move(server));
    fidl::SyncClient<score_mw_com::Endpoint> client{std::move(endpoints->client)};
    auto events = fidl::CreateEndpoints<score_mw_com::EventChannel>();
    auto result = client->Attach(score_mw_com::EndpointAttachRequest{std::move(events->server)});
    if (result.is_error())
    {
        return {false, result.error_value().FormatDescription(), 0U, 0U, 0U};
    }
    auto& response = result.value();
    return {true, "accepted", RightsOf(response.data_vmo()), RightsOf(response.control_vmo()), RightsOf(response.liveness())};
}
}  // namespace

int main()
{
    async::Loop loop{&kAsyncLoopConfigNoAttachToCurrentThread};
    loop.StartThread("rogue");

    constexpr zx_rights_t kT = ZX_RIGHT_TRANSFER;
    constexpr zx_rights_t kAll = ZX_RIGHT_SAME_RIGHTS;

    std::printf("declared: data=0x%x control=0x%x liveness=0x%x\n", kDataRights, kControlRights, kLivenessRights);

    const auto exact = Run(loop, {"exact", kDataRights | kT, kControlRights | kT, kLivenessRights | kT});
    std::printf("exact+TRANSFER : %s data=0x%x control=0x%x liveness=0x%x\n", exact.description.c_str(), exact.data, exact.control, exact.liveness);
    CHECK(exact.ok);
    CHECK(exact.data == kDataRights);
    CHECK(exact.control == kControlRights);
    CHECK(exact.liveness == kLivenessRights);

    const auto extra = Run(loop, {"extra", kAll, kAll, kAll});
    std::printf("extra rights   : %s data=0x%x control=0x%x liveness=0x%x\n", extra.description.c_str(), extra.data, extra.control, extra.liveness);

    const auto no_write = Run(loop, {"no_write", kDataRights | kT, (kControlRights & ~ZX_RIGHT_WRITE) | kT, kLivenessRights | kT});
    std::printf("control no WRITE: %s data=0x%x control=0x%x\n", no_write.description.c_str(), no_write.data, no_write.control);

    const auto no_map = Run(loop, {"no_map", (kDataRights & ~ZX_RIGHT_MAP) | kT, kControlRights | kT, kLivenessRights | kT});
    std::printf("data no MAP    : %s data=0x%x\n", no_map.description.c_str(), no_map.data);

    const auto data_writable = Run(loop, {"data_write", kDataRights | ZX_RIGHT_WRITE | kT, kControlRights | kT, kLivenessRights | kT});
    std::printf("data + WRITE   : %s data=0x%x\n", data_writable.description.c_str(), data_writable.data);

    loop.Quit();
    loop.JoinThreads();
    std::printf("RIGHTS_REDUCTION_TEST %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
