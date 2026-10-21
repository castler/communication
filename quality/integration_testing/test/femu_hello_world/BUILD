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
    "fuchsia_cc_test",
    "fuchsia_component",
    "fuchsia_component_manifest",
    "fuchsia_package",
    "fuchsia_unittest_package",
)
load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

fuchsia_cc_binary(
    name = "hello_world",
    srcs = ["hello_world.cc"],
    tags = ["manual"],
    deps = [
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "manifest",
    src = "meta/hello_world.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "component",
    manifest = ":manifest",
    tags = ["manual"],
    deps = [":hello_world"],
)

fuchsia_package(
    name = "pkg",
    package_name = "hello_world",
    components = [":component"],
    tags = ["manual"],
)

# The same program packaged as a test so that its stdout is captured by `ffx test run`.
fuchsia_cc_test(
    name = "hello_world_test",
    srcs = ["hello_world.cc"],
    tags = ["manual"],
    deps = [
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_unittest_package(
    name = "test_pkg",
    package_name = "hello_world_test",
    tags = ["manual"],
    unit_tests = [":hello_world_test"],
)

femu_integration_test(
    name = "hello_world_itf_test",
    srcs = ["test_hello_world.py"],
    packages = [":test_pkg"],
)
