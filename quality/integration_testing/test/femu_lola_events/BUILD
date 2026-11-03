# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************

load(
    "@rules_fuchsia//fuchsia:defs.bzl",
    "fuchsia_cc_binary",
    "fuchsia_component_manifest",
    "fuchsia_test_component",
    "fuchsia_test_package",
)
load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

# LoLa event notification on Fuchsia: a skeleton process publishing through LoLa's event data control (composite,
# SkeletonMemoryManager regions) and notifying through the EventNotifyingServiceInstance decorator, three subscriber
# processes receiving through the Attach notification eventpair, the death of a subscriber and of the skeleton.
fuchsia_cc_binary(
    name = "lola_events_test",
    srcs = ["lola_events_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:event_notifying_service_instance",
        "//score/mw/com/impl/bindings/lola/fuchsia:fuchsia_event_notifier",
        "//score/mw/com/impl/bindings/lola/fuchsia:lola_shm_harness",
        "//score/mw/com/impl/bindings/lola/fuchsia:score_mw_com_fidl_cpp",
        "//score/mw/com/impl/bindings/lola/fuchsia:vmo_shm_runtime",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_component_manifest(
    name = "lola_events_test_manifest",
    src = "meta/lola_events_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "lola_events_test_component",
    manifest = ":lola_events_test_manifest",
    tags = ["manual"],
    deps = [":lola_events_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lola_events_test",
    tags = ["manual"],
    test_components = [":lola_events_test_component"],
)

femu_integration_test(
    name = "lola_events_itf_test",
    srcs = ["test_lola_events.py"],
    packages = [":test_pkg"],
)
