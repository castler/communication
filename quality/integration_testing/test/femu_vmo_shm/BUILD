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

# Skeleton process creates a VMO backed shared memory resource and a second process of the same binary reads it
# through a handle with reduced rights.
fuchsia_cc_binary(
    name = "vmo_shm_test",
    srcs = ["vmo_shm_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/memory/shared:shared_memory_factory",
        "//score/memory/shared:vector",
        "//score/memory/shared/fuchsia:vmo_shared_memory_factory",
        "//score/memory/shared/fuchsia:vmo_shared_memory_resource",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_component_manifest(
    name = "vmo_shm_test_manifest",
    src = "meta/vmo_shm_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "vmo_shm_test_component",
    manifest = ":vmo_shm_test_manifest",
    tags = ["manual"],
    deps = [":vmo_shm_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "vmo_shm_test",
    tags = ["manual"],
    test_components = [":vmo_shm_test_component"],
)

femu_integration_test(
    name = "vmo_shm_itf_test",
    srcs = ["test_vmo_shm.py"],
    packages = [":test_pkg"],
)
