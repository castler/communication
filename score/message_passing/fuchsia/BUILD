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

load("@rules_fuchsia//fuchsia:defs.bzl", "fuchsia_cc_test", "fuchsia_unittest_package")

# Fuchsia targets are only built with --config=fuchsia, see quality/integration_testing/environments/femu.
fuchsia_cc_test(
    name = "echo_test",
    srcs = ["echo_test.cpp"],
    tags = ["manual"],
    deps = [
        "//score/message_passing",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_unittest_package(
    name = "echo_test_pkg",
    package_name = "message_passing_echo_test",
    tags = ["manual"],
    unit_tests = [":echo_test"],
    visibility = ["//quality/integration_testing:__subpackages__"],
)
