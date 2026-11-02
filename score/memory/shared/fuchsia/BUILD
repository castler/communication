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
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")

# VMO backed shared memory for Fuchsia, only built with --config=fuchsia.

cc_library(
    name = "i_vmo_handle_provider",
    hdrs = ["i_vmo_handle_provider.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//visibility:public"],
    deps = [
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/os:errno",
    ],
)

cc_library(
    name = "vmo_shared_memory_resource",
    srcs = ["vmo_shared_memory_resource.cpp"],
    hdrs = ["vmo_shared_memory_resource.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//visibility:public"],
    deps = [
        "//score/memory/shared:i_shared_memory_resource",
        "//score/memory/shared:memory_resource_proxy",
        "//score/memory/shared:memory_resource_registry",
        "//score/memory/shared:pointer_arithmetic_util",
        "//score/memory/shared:shared_memory_resource",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/mw/log",
        "@score_baselibs//score/os:errno",
    ],
)

cc_library(
    name = "vmo_shared_memory_factory",
    srcs = ["vmo_shared_memory_factory.cpp"],
    hdrs = ["vmo_shared_memory_factory.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//visibility:public"],
    deps = [
        ":i_vmo_handle_provider",
        ":vmo_shared_memory_resource",
        "//score/memory/shared:i_shared_memory_factory",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/mw/log",
    ],
)
