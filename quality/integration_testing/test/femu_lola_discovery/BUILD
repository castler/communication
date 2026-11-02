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
    "fuchsia_component",
    "fuchsia_component_manifest",
    "fuchsia_fidl_library",
    "fuchsia_test_component",
    "fuchsia_test_package",
)
load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

# The real LoLa discovery client (FuchsiaServiceDiscoveryClient over FidlDiscoveryTransport) against the discovery
# component: a skeleton component offers, a proxy component finds. Scenarios: start-find-before-offer, stop-offer,
# offer restart (skeleton crash), find-once, start-find-after-offer, stop-find, foreign service id. The transport test
# restarts an in-process discovery service under live clients. Design: design/fuchsia/README.md section 6.3.

fuchsia_fidl_library(
    name = "agent_fidl",
    srcs = ["agent.fidl"],
    cc_bindings = ["cpp"],
    library = "score.mw.com.test.lola",
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    deps = ["@fuchsia_sdk//fidl/zx"],
)

fuchsia_cc_binary(
    name = "agent_main",
    srcs = ["agent_main.cc"],
    tags = ["manual"],
    deps = [
        ":agent_fidl_cpp",
        "//score/mw/com/impl/bindings/lola/fuchsia:discovery_test_support",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_discovery_transport",
        "//score/mw/com/impl/bindings/lola/fuchsia:fuchsia_service_discovery_client",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "driver_test",
    srcs = ["driver_test.cc"],
    tags = ["manual"],
    deps = [
        ":agent_fidl_cpp",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "transport_test",
    srcs = ["transport_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:discovery_service",
        "//score/mw/com/impl/bindings/lola/fuchsia:discovery_test_support",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_discovery_transport",
        "//score/mw/com/impl/bindings/lola/fuchsia:fuchsia_service_discovery_client",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "discovery_manifest",
    src = "meta/discovery.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "discovery_component",
    manifest = ":discovery_manifest",
    tags = ["manual"],
    deps = ["//quality/integration_testing/test/femu_fuchsia_attach:discovery_main"],
)

fuchsia_component_manifest(
    name = "skeleton_manifest",
    src = "meta/skeleton.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "skeleton_component",
    manifest = ":skeleton_manifest",
    tags = ["manual"],
    deps = [":agent_main"],
)

fuchsia_component_manifest(
    name = "proxy_manifest",
    src = "meta/proxy.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "proxy_component",
    manifest = ":proxy_manifest",
    tags = ["manual"],
    deps = [":agent_main"],
)

fuchsia_component_manifest(
    name = "driver_test_manifest",
    src = "meta/driver_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "driver_test_component",
    manifest = ":driver_test_manifest",
    tags = ["manual"],
    deps = [":driver_test"],
)

fuchsia_component_manifest(
    name = "transport_test_manifest",
    src = "meta/transport_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "transport_test_component",
    manifest = ":transport_test_manifest",
    tags = ["manual"],
    deps = [":transport_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lola_discovery_test",
    components = [
        ":discovery_component",
        ":proxy_component",
        ":skeleton_component",
    ],
    tags = ["manual"],
    test_components = [
        ":driver_test_component",
        ":transport_test_component",
    ],
)

femu_integration_test(
    name = "lola_discovery_itf_test",
    srcs = ["test_lola_discovery.py"],
    packages = [":test_pkg"],
)
