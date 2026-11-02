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

# Skeleton creates data and control regions and offers the instance at the discovery component; the proxy finds it
# (Find, StartFind + Watch), attaches over its routed Endpoint and reads; a third consumer without a routed Endpoint is
# refused. Design: design/fuchsia/README.md section 6.3.

fuchsia_fidl_library(
    name = "probe_fidl",
    srcs = ["probe.fidl"],
    cc_bindings = ["cpp"],
    library = "score.mw.com.test",
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
    name = "discovery_main",
    srcs = ["discovery_main.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:discovery_service",
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
        ":test_topology",
        "//score/memory/shared:shared_memory_factory",
        "//score/memory/shared/fuchsia:vmo_shared_memory_factory",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_vmo_handle_server",
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
    name = "consumer_main",
    srcs = ["consumer_main.cc"],
    tags = ["manual"],
    deps = [
        ":probe_fidl_cpp",
        ":test_topology",
        "//score/memory/shared/fuchsia:vmo_shared_memory_factory",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_vmo_handle_client",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "attach_test",
    srcs = ["attach_test.cc"],
    tags = ["manual"],
    deps = [
        ":probe_fidl_cpp",
        ":test_topology",
        "//score/mw/com/impl/bindings/lola/fuchsia:vmo_role",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "rights_test",
    srcs = ["rights_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:score_mw_com_fidl_cpp",
        "//score/mw/com/impl/bindings/lola/fuchsia:vmo_role",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "rights_test_manifest",
    src = "meta/rights_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "rights_test_component",
    manifest = ":rights_test_manifest",
    tags = ["manual"],
    deps = [":rights_test"],
)

fuchsia_cc_binary(
    name = "discovery_service_test",
    srcs = ["discovery_service_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:discovery_service",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "discovery_service_test_manifest",
    src = "meta/discovery_service_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "discovery_service_test_component",
    manifest = ":discovery_service_test_manifest",
    tags = ["manual"],
    deps = [":discovery_service_test"],
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
    deps = [":discovery_main"],
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
    deps = [":consumer_main"],
)

fuchsia_component_manifest(
    name = "rogue_manifest",
    src = "meta/rogue.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "rogue_component",
    manifest = ":rogue_manifest",
    tags = ["manual"],
    deps = [":consumer_main"],
)

fuchsia_component_manifest(
    name = "attach_test_manifest",
    src = "meta/attach_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "attach_test_component",
    manifest = ":attach_test_manifest",
    tags = ["manual"],
    deps = [":attach_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "fuchsia_attach_test",
    components = [
        ":discovery_component",
        ":proxy_component",
        ":rogue_component",
        ":skeleton_component",
    ],
    tags = ["manual"],
    test_components = [
        ":attach_test_component",
        ":discovery_service_test_component",
        ":rights_test_component",
    ],
)

femu_integration_test(
    name = "fuchsia_attach_itf_test",
    srcs = ["test_fuchsia_attach.py"],
    packages = [":test_pkg"],
)
