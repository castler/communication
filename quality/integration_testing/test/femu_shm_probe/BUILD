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
    "fuchsia_cc_test",
    "fuchsia_unittest_package",
)
load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

# Probes which POSIX shared memory primitives of the Fuchsia libc work at runtime.
fuchsia_cc_test(
    name = "shm_probe_test",
    srcs = ["shm_probe.cc"],
    tags = ["manual"],
    deps = [
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_unittest_package(
    name = "test_pkg",
    package_name = "shm_probe_test",
    tags = ["manual"],
    unit_tests = [":shm_probe_test"],
)

femu_integration_test(
    name = "shm_probe_itf_test",
    srcs = ["test_shm_probe.py"],
    packages = [":test_pkg"],
)
