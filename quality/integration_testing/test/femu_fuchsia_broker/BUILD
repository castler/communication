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

# Lease broker (design/fuchsia/README.md section 6.5): the skeleton is killed (zx_process_exit, then a trap), the proxy
# detects it, a restarted skeleton gets a new epoch and new regions, the proxy reattaches, the broker releases the old
# regions.

fuchsia_fidl_library(
    name = "broker_probe_fidl",
    srcs = ["broker_probe.fidl"],
    cc_bindings = ["cpp"],
    library = "score.mw.com.test.broker",
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    deps = ["@fuchsia_sdk//fidl/zx"],
)

cc_library(
    name = "test_topology",
    hdrs = ["test_topology.h"],
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
)

fuchsia_cc_binary(
    name = "broker_main",
    srcs = ["broker_main.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:broker_service",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "skeleton_main",
    srcs = ["skeleton_main.cc"],
    tags = ["manual"],
    deps = [
        ":broker_probe_fidl_cpp",
        ":test_topology",
        "//score/memory/shared:shared_memory_factory",
        "//score/memory/shared/fuchsia:vmo_shared_memory_factory",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_vmo_handle_server",
        "//score/mw/com/impl/bindings/lola/fuchsia:incarnation_header",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "proxy_main",
    srcs = ["proxy_main.cc"],
    tags = ["manual"],
    deps = [
        ":broker_probe_fidl_cpp",
        ":test_topology",
        "//score/memory/shared/fuchsia:vmo_shared_memory_factory",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_vmo_handle_client",
        "//score/mw/com/impl/bindings/lola/fuchsia:incarnation_header",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "broker_test",
    srcs = ["broker_test.cc"],
    tags = ["manual"],
    deps = [
        ":broker_probe_fidl_cpp",
        ":test_topology",
        "//score/mw/com/impl/bindings/lola/fuchsia:score_mw_com_fidl_cpp",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "broker_manifest",
    src = "meta/broker.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "broker_component",
    manifest = ":broker_manifest",
    tags = ["manual"],
    deps = [":broker_main"],
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
    deps = [":skeleton_main"],
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
    deps = [":proxy_main"],
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
    name = "broker_test_manifest",
    src = "meta/broker_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "broker_test_component",
    manifest = ":broker_test_manifest",
    tags = ["manual"],
    deps = [":broker_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "fuchsia_broker_test",
    components = [
        ":broker_component",
        ":discovery_component",
        ":proxy_component",
        ":skeleton_component",
    ],
    tags = ["manual"],
    test_components = [":broker_test_component"],
)

femu_integration_test(
    name = "fuchsia_broker_itf_test",
    srcs = ["test_fuchsia_broker.py"],
    packages = [":test_pkg"],
)
