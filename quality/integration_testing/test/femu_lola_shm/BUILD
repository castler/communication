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

# LoLa's own shared-memory code (SkeletonMemoryManager creating data / QM control / ASIL-B control, proxy-side Open) on
# the Fuchsia VMO factory, with the handles travelling through the FIDL Attach layer between two processes, plus
# InterprocessMutex / InterprocessConditionalVariable in a shared VMO mapping across two processes.
fuchsia_cc_binary(
    name = "lola_shm_test",
    srcs = ["lola_shm_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/memory/shared:lock_file",
        "//score/memory/shared/flock:exclusive_flock_mutex",
        "//score/mw/com/impl/bindings/lola/fuchsia:lola_shm_harness",
        "//score/mw/com/impl/bindings/lola/fuchsia:score_mw_com_fidl_cpp",
        "//score/mw/com/impl/bindings/lola/fuchsia:vmo_shm_runtime",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/os/utils/interprocess:interprocess_notification",
    ],
)

fuchsia_component_manifest(
    name = "lola_shm_test_manifest",
    src = "meta/lola_shm_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "lola_shm_test_component",
    manifest = ":lola_shm_test_manifest",
    tags = ["manual"],
    deps = [":lola_shm_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lola_shm_test",
    tags = ["manual"],
    test_components = [":lola_shm_test_component"],
)

femu_integration_test(
    name = "lola_shm_itf_test",
    srcs = ["test_lola_shm.py"],
    packages = [":test_pkg"],
)
