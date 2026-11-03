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
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <zircon/syscalls.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Profile 1 of the slot refcount (decision record y07): the consumer-writable control regions are only handed to
// consumers whose declared integrity is >= the integrity of the service. Skeleton (FidlVmoHandleServer) and consumers
// (FidlVmoHandleClient) run in this process over real channels.

namespace
{
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleClient;
using score::mw::com::impl::lola::fuchsia::FidlVmoHandleServer;
using score::mw::com::impl::lola::fuchsia::Integrity;
using score::mw::com::impl::lola::fuchsia::kControlRights;

constexpr std::uint64_t kQmApp = 11U;
constexpr std::uint64_t kAsilBApp = 12U;
constexpr std::uint64_t kUndeclaredApp = 13U;
constexpr std::uint64_t kRegion = 4096U;
constexpr const char* kData = "/lola-data-0000000000004660-00001";
constexpr const char* kControl = "/lola-ctl-0000000000004660-00001";
constexpr const char* kControlAsilB = "/lola-ctl-0000000000004660-00001-b";

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

struct Skeleton
{
    Skeleton(async_dispatcher_t* dispatcher, const bool asil_b) : server{dispatcher}
    {
        Publish(kData);
        Publish(kControl);
        if (asil_b)
        {
            Publish(kControlAsilB);
        }
        server.SetEpoch(1U);
    }

    void Publish(const char* key)
    {
        zx::vmo vmo;
        zx::vmo copy;
        CHECK(zx::vmo::create(kRegion, 0U, &vmo) == ZX_OK);
        CHECK(vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &copy) == ZX_OK);
        CHECK(server.Publish(key, std::move(copy)).has_value());
        originals.push_back(std::move(vmo));
    }

    std::unique_ptr<FidlVmoHandleClient> Connect(const std::uint64_t application_id)
    {
        auto endpoints = fidl::CreateEndpoints<score_mw_com::Endpoint>();
        server.CreateHandler(application_id)(std::move(endpoints->server));
        return std::make_unique<FidlVmoHandleClient>(std::move(endpoints->client));
    }

    FidlVmoHandleServer server;
    std::vector<zx::vmo> originals;  // data, control[, asil-b control]
};

template <typename Object>
zx_rights_t RightsOf(const Object& object)
{
    zx_info_handle_basic_t info{};
    object.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    return info.rights;
}

void ExpectRefused(FidlVmoHandleClient& client, const char* who)
{
    auto asil_b = client.Acquire(kControlAsilB);
    CHECK(!asil_b.has_value());
    CHECK(client.attach_error().has_value());
    CHECK(client.attach_error() == score_mw_com::AttachError::kInsufficientIntegrity);
    // Fail closed: not even the data or the QM control region is delivered.
    CHECK(!client.Acquire(kData).has_value());
    CHECK(!client.Acquire(kControl).has_value());
    std::printf("%s: refused (INSUFFICIENT_INTEGRITY), no handles\n", who);
}

}  // namespace

int main()
{
    async::Loop loop{&kAsyncLoopConfigNeverAttachToThread};
    CHECK(loop.StartThread("attach-server") == ZX_OK);

    {
        // ASIL-B service: the control regions are consumer-writable.
        Skeleton skeleton{loop.dispatcher(), true};
        skeleton.server.SetConsumerIntegrity(kQmApp, Integrity::kQm);
        skeleton.server.SetConsumerIntegrity(kAsilBApp, Integrity::kAsilB);

        auto qm = skeleton.Connect(kQmApp);
        ExpectRefused(*qm, "QM consumer");

        auto undeclared = skeleton.Connect(kUndeclaredApp);
        ExpectRefused(*undeclared, "undeclared consumer");
        CHECK(skeleton.server.AttachCount() == 0U);

        auto asil_b = skeleton.Connect(kAsilBApp);
        auto data = asil_b->Acquire(kData);
        auto control = asil_b->Acquire(kControl);
        auto control_b = asil_b->Acquire(kControlAsilB);
        CHECK(data.has_value() && control.has_value() && control_b.has_value());
        CHECK(!asil_b->attach_error().has_value());
        CHECK(skeleton.server.AttachCount(kAsilBApp) == 1U);
        if (control_b.has_value())
        {
            CHECK(RightsOf(*control_b) == kControlRights);
            // The handle is really writable: a write by the consumer is seen through the skeleton's handle.
            const std::uint32_t value = 0xA51U;
            CHECK(control_b->write(&value, 0U, sizeof(value)) == ZX_OK);
            std::uint32_t read_back = 0U;
            CHECK(skeleton.originals[2].read(&read_back, 0U, sizeof(read_back)) == ZX_OK);
            CHECK(read_back == value);
        }
        std::printf("ASIL-B consumer: attached with data + QM control + ASIL-B control\n");
    }

    {
        // QM-only service: QM and ASIL-B consumers may both attach, an undeclared consumer counts as QM.
        Skeleton skeleton{loop.dispatcher(), false};
        skeleton.server.SetConsumerIntegrity(kAsilBApp, Integrity::kAsilB);
        auto qm = skeleton.Connect(kUndeclaredApp);
        CHECK(qm->Acquire(kData).has_value());
        CHECK(qm->Acquire(kControl).has_value());
        CHECK(!qm->Acquire(kControlAsilB).has_value());
        CHECK(!qm->attach_error().has_value());
        auto higher = skeleton.Connect(kAsilBApp);
        CHECK(higher->Acquire(kControl).has_value());
        CHECK(skeleton.server.AttachCount() == 2U);
        std::printf("QM-only service: QM and ASIL-B consumers attached\n");
    }

    loop.Quit();
    loop.JoinThreads();
    std::printf("LOLA_INTEGRITY_TEST %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
