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

# LockFile/FlockMutex (LoLa existence and usage markers) without flock(2): the parent process hosts a lock registry,
# child processes are its clients. A second acquire fails while the first process holds, and succeeds after that
# process died without releasing.
fuchsia_cc_binary(
    name = "lock_test",
    srcs = ["lock_test.cc"],
    tags = ["manual"],
    deps = [
        "//score/memory/shared:lock_file",
        "//score/memory/shared/flock:exclusive_flock_mutex",
        "//score/memory/shared/flock:shared_flock_mutex",
        "//score/memory/shared/fuchsia:lock_registry_channel",
        "//score/memory/shared/lock_provider",
        "//score/memory/shared/lock_provider:in_process_lock_registry",
        "//score/memory/shared/lock_provider:registry_lock_provider",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_component_manifest(
    name = "lock_test_manifest",
    src = "meta/lock_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "lock_test_component",
    manifest = ":lock_test_manifest",
    tags = ["manual"],
    deps = [":lock_test"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lock_test",
    tags = ["manual"],
    test_components = [":lock_test_component"],
)

femu_integration_test(
    name = "lock_itf_test",
    srcs = ["test_lock.py"],
    packages = [":test_pkg"],
)
