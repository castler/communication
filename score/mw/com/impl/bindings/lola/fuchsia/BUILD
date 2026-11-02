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

load("@rules_fuchsia//fuchsia:defs.bzl", "fuchsia_fidl_library")
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")

# Attach and discovery of LoLa on Fuchsia, design/fuchsia/README.md section 6.3. Only built with --config=fuchsia.
#
# Lives below bindings/lola because Attach hands out the data/control regions of a LoLa service instance and the
# discovery maps onto IServiceDiscoveryClient; score/memory/shared/fuchsia stays free of mw::com concepts and only
# provides the IVmoHandleProvider seam that is implemented here.

fuchsia_fidl_library(
    name = "score_mw_com_fidl",
    srcs = ["score_mw_com.fidl"],
    cc_bindings = ["cpp"],
    library = "score.mw.com",
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = ["@fuchsia_sdk//fidl/zx"],
)

cc_library(
    name = "vmo_role",
    srcs = ["vmo_role.cpp"],
    hdrs = ["vmo_role.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = ["@fuchsia_sdk//pkg/zx"],
)

cc_library(
    name = "fidl_vmo_handle_server",
    srcs = ["fidl_vmo_handle_server.cpp"],
    hdrs = ["fidl_vmo_handle_server.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        ":score_mw_com_fidl_cpp",
        ":vmo_role",
        "//score/memory/shared/fuchsia:i_vmo_handle_provider",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/mw/log",
    ],
)

cc_library(
    name = "fidl_vmo_handle_client",
    srcs = ["fidl_vmo_handle_client.cpp"],
    hdrs = ["fidl_vmo_handle_client.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        ":score_mw_com_fidl_cpp",
        ":vmo_role",
        "//score/memory/shared/fuchsia:i_vmo_handle_provider",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/mw/log",
    ],
)

cc_library(
    name = "discovery_service",
    srcs = ["discovery_service.cpp"],
    hdrs = ["discovery_service.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        ":score_mw_com_fidl_cpp",
        "@fuchsia_sdk//pkg/async-cpp",
        "@score_baselibs//score/mw/log",
    ],
)
