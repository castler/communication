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

# Slot refcount profile 1: Attach hands the consumer-writable control regions only to consumers whose integrity is at
# least the one of the service (a QM consumer cannot attach to an ASIL-B control region, an ASIL-B consumer can).

fuchsia_cc_binary(
    name = "integrity_test",
    srcs = ["integrity_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_vmo_handle_client",
        "//score/mw/com/impl/bindings/lola/fuchsia:fidl_vmo_handle_server",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "integrity_test_manifest",
    src = "meta/integrity_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "integrity_test_component",
    manifest = ":integrity_test_manifest",
    tags = ["manual"],
    deps = [":integrity_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lola_integrity_test",
    tags = ["manual"],
    test_components = [":integrity_test_component"],
)

femu_integration_test(
    name = "lola_integrity_itf_test",
    srcs = ["test_lola_integrity.py"],
    packages = [":test_pkg"],
)
