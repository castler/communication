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

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//quality/unit_testing:unit_testing.bzl", "cc_unit_test")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")

cc_library(
    name = "i_lock_provider",
    hdrs = ["i_lock_provider.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["FFI"],
    visibility = ["//visibility:public"],  # platform_only
    deps = [
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/os:errno",
        "@score_baselibs//score/os:fcntl",
    ],
)

cc_library(
    name = "i_lock_registry",
    hdrs = ["i_lock_registry.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["FFI"],
    visibility = ["//visibility:public"],  # platform_only
    deps = [
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/os:errno",
    ],
)

cc_library(
    name = "posix_lock_provider",
    srcs = ["posix_lock_provider.cpp"],
    hdrs = ["posix_lock_provider.h"],
    features = COMPILER_WARNING_FEATURES,
    implementation_deps = [
        "@score_baselibs//score/os:stat",
        "@score_baselibs//score/os:unistd",
    ],
    tags = ["FFI"],
    visibility = ["//visibility:public"],  # platform_only
    deps = [
        ":i_lock_provider",
        "@score_baselibs//score/os:fcntl",
    ],
)

cc_library(
    name = "in_process_lock_registry",
    srcs = ["in_process_lock_registry.cpp"],
    hdrs = ["in_process_lock_registry.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["FFI"],
    visibility = ["//visibility:public"],  # platform_only
    deps = [":i_lock_registry"],
)

cc_library(
    name = "registry_lock_provider",
    srcs = ["registry_lock_provider.cpp"],
    hdrs = ["registry_lock_provider.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["FFI"],
    visibility = ["//visibility:public"],  # platform_only
    deps = [
        ":i_lock_provider",
        ":i_lock_registry",
    ],
)

cc_library(
    name = "lock_provider",
    srcs = ["lock_provider.cpp"],
    hdrs = ["lock_provider.h"],
    features = COMPILER_WARNING_FEATURES,
    implementation_deps = [
        ":in_process_lock_registry",
        ":posix_lock_provider",
        ":registry_lock_provider",
    ],
    tags = ["FFI"],
    visibility = ["//visibility:public"],  # platform_only
    deps = [":i_lock_provider"],
)

cc_unit_test(
    name = "unit_test",
    srcs = [
        "lock_provider_test.cpp",
        "lock_seam_test.cpp",
    ],
    features = COMPILER_WARNING_FEATURES,
    visibility = ["//score/memory:__pkg__"],
    deps = [
        ":in_process_lock_registry",
        ":lock_provider",
        ":registry_lock_provider",
        "//score/memory/shared:lock_file",
        "//score/memory/shared/flock:exclusive_flock_mutex",
        "//score/memory/shared/flock:shared_flock_mutex",
        "@score_baselibs//score/mw/log:backend_stub_testutil",
    ],
)
