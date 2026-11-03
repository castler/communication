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
    "fuchsia_package_resource",
    "fuchsia_test_component",
    "fuchsia_test_package",
)
load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

# First end-to-end mw::com on Fuchsia: a skeleton component offers a WheelSpeed event (100 Hz) through the public
# mw::com API, a proxy component finds it (StartFindService), subscribes, receives (SetReceiveHandler/GetNewSamples)
# and verifies the samples; the skeleton is restarted and the proxy has to re-find and re-subscribe.
# Design: design/fuchsia/README.md; status: see the demo report.

fuchsia_fidl_library(
    name = "wheelspeed_fidl",
    srcs = ["wheelspeed.fidl"],
    cc_bindings = ["cpp"],
    library = "score.mw.com.wheelspeed",
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
)

cc_library(
    name = "wheel_speed_interface",
    hdrs = ["wheel_speed_interface.h"],
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    deps = ["//score/mw/com"],
)

fuchsia_cc_binary(
    name = "wheelspeed_skeleton",
    srcs = ["skeleton_main.cc"],
    tags = ["manual"],
    deps = [
        ":wheel_speed_interface",
        "//score/mw/com",
        "//score/mw/com/impl/bindings/lola/fuchsia:score_mw_com_fidl_cpp",
        "//score/mw/com/impl/bindings/lola/fuchsia:vmo_shm_runtime",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_cc_binary(
    name = "wheelspeed_proxy",
    srcs = ["proxy_main.cc"],
    tags = ["manual"],
    deps = [
        ":wheel_speed_interface",
        ":wheelspeed_fidl_cpp",
        "//score/mw/com",
        "//score/mw/com/impl/bindings/lola/fuchsia:score_mw_com_fidl_cpp",
        "//score/mw/com/impl/bindings/lola/fuchsia:vmo_shm_runtime",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_cc_binary(
    name = "wheelspeed_driver",
    srcs = ["driver_main.cc"],
    tags = ["manual"],
    deps = [
        ":wheelspeed_fidl_cpp",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_package_resource(
    name = "skeleton_config",
    src = "skeleton_config.json",
    dest = "data/skeleton_config.json",
    tags = ["manual"],
)

fuchsia_package_resource(
    name = "proxy_config",
    src = "proxy_config.json",
    dest = "data/proxy_config.json",
    tags = ["manual"],
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
    deps = [":wheelspeed_skeleton"],
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
    deps = [":wheelspeed_proxy"],
)

fuchsia_component_manifest(
    name = "driver_manifest",
    src = "meta/wheelspeed_driver.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "driver_component",
    manifest = ":driver_manifest",
    tags = ["manual"],
    deps = [":wheelspeed_driver"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lola_wheelspeed",
    components = [
        ":discovery_component",
        ":proxy_component",
        ":skeleton_component",
    ],
    resources = [
        ":proxy_config",
        ":skeleton_config",
    ],
    tags = ["manual"],
    test_components = [":driver_component"],
)

femu_integration_test(
    name = "lola_wheelspeed_itf_test",
    srcs = ["test_lola_wheelspeed.py"],
    packages = [":test_pkg"],
)
