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
load("//quality/unit_testing:unit_testing.bzl", "cc_unit_test")
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

# Lease broker (design/fuchsia/README.md section 6.5). The core and the header are platform neutral and unit tested on
# the host; the service is the Fuchsia shell (VMOs, eventpairs, process handles).

cc_library(
    name = "incarnation_header",
    hdrs = ["incarnation_header.h"],
    features = COMPILER_WARNING_FEATURES,
    visibility = ["//quality/integration_testing/test:__subpackages__"],
)

cc_library(
    name = "broker_core",
    srcs = ["broker_core.cpp"],
    hdrs = ["broker_core.h"],
    features = COMPILER_WARNING_FEATURES,
    visibility = ["//quality/integration_testing/test:__subpackages__"],
)

cc_test(
    name = "broker_core_test",
    srcs = ["broker_core_test.cpp"],
    features = COMPILER_WARNING_FEATURES,
    deps = [
        ":broker_core",
        ":incarnation_header",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "broker_service",
    srcs = ["broker_service.cpp"],
    hdrs = ["broker_service.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        ":broker_core",
        ":incarnation_header",
        ":score_mw_com_fidl_cpp",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/mw/log",
    ],
)

# Routes LoLa's shared-memory creation/opening through VMOs and FIDL Attach (instead of shm_open).
cc_library(
    name = "vmo_shm_runtime",
    srcs = ["vmo_shm_runtime.cpp"],
    hdrs = ["vmo_shm_runtime.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        ":fidl_vmo_handle_client",
        ":fidl_vmo_handle_server",
        ":score_mw_com_fidl_cpp",
        ":vmo_role",
        "//score/memory/shared:shared_memory_factory",
        "//score/memory/shared/fuchsia:i_vmo_handle_provider",
        "//score/memory/shared/fuchsia:vmo_shared_memory_factory",
        "//score/memory/shared/lock_provider",
        "//score/memory/shared/lock_provider:i_lock_provider",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/zx",
    ],
)

# Drives the real lola::SkeletonMemoryManager (skeleton) and the proxy-side Open of the regions with a faked runtime.
cc_library(
    name = "lola_shm_harness",
    srcs = ["lola_shm_harness.cpp"],
    hdrs = ["lola_shm_harness.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        "//score/memory/shared:managed_memory_resource",
        "//score/memory/shared:shared_memory_factory",
        "//score/mw/com/impl:runtime",
        "//score/mw/com/impl:runtime_interfaces",
        "//score/mw/com/impl:skeleton_event_binding",
        "//score/mw/com/impl/bindings/lola:element_fq_id",
        "//score/mw/com/impl/bindings/lola:event_control",
        "//score/mw/com/impl/bindings/lola:event_data_storage",
        "//score/mw/com/impl/bindings/lola:i_runtime",
        "//score/mw/com/impl/bindings/lola:service_data_control",
        "//score/mw/com/impl/bindings/lola:service_data_storage",
        "//score/mw/com/impl/bindings/lola:shm_path_builder",
        "//score/mw/com/impl/bindings/lola:skeleton",
        "//score/mw/com/impl/configuration",
    ],
)

# LoLa service discovery on Fuchsia (design/fuchsia/README.md section 6.3). The client core is platform neutral and
# unit tested on the host against a fake transport; only the FIDL transport is Fuchsia specific.

cc_library(
    name = "discovery_transport",
    hdrs = ["discovery_transport.h"],
    features = COMPILER_WARNING_FEATURES,
    visibility = ["//quality/integration_testing/test:__subpackages__"],
)

cc_library(
    name = "fuchsia_service_discovery_client",
    srcs = ["fuchsia_service_discovery_client.cpp"],
    hdrs = ["fuchsia_service_discovery_client.h"],
    features = COMPILER_WARNING_FEATURES,
    visibility = [
        "//score/mw/com/impl/bindings/lola:__pkg__",
        "//quality/integration_testing/test:__subpackages__",
    ],
    deps = [
        ":discovery_transport",
        "//score/mw/com/impl:enriched_instance_identifier",
        "//score/mw/com/impl:error",
        "//score/mw/com/impl:find_service_handle",
        "//score/mw/com/impl:handle_type",
        "//score/mw/com/impl:i_service_discovery_client",
        "//score/mw/com/impl:instance_identifier",
        "//score/mw/com/impl/configuration",
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/mw/log",
        "@score_baselibs//score/result",
    ],
)

cc_library(
    name = "fidl_discovery_transport",
    srcs = ["fidl_discovery_transport.cpp"],
    hdrs = ["fidl_discovery_transport.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = [
        "//score/mw/com/impl/bindings/lola:__pkg__",
        "//quality/integration_testing/test:__subpackages__",
    ],
    deps = [
        ":discovery_transport",
        ":score_mw_com_fidl_cpp",
        "@fuchsia_sdk//pkg/async-cpp",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/language/futurecpp",
        "@score_baselibs//score/mw/log",
    ],
)

cc_unit_test(
    name = "fuchsia_service_discovery_client_test",
    testonly = True,
    srcs = [
        "fake_discovery_transport.h",
        "fuchsia_service_discovery_client_test.cpp",
    ],
    features = COMPILER_WARNING_FEATURES,
    deps = [
        ":discovery_transport",
        ":fuchsia_service_discovery_client",
        "//score/mw/com/impl:error",
        "//score/mw/com/impl:handle_type",
        "//score/mw/com/impl:instance_specifier",
        "//score/mw/com/impl/configuration",
        "//score/mw/com/impl/configuration/test:configuration_store",
    ],
)

# Test support: builds LoLa instance identifiers without a configuration file for the FEMU discovery tests.
cc_library(
    name = "discovery_test_support",
    srcs = ["discovery_test_support.cpp"],
    hdrs = ["discovery_test_support.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    visibility = ["//quality/integration_testing/test:__subpackages__"],
    deps = [
        "//score/mw/com/impl:enriched_instance_identifier",
        "//score/mw/com/impl:handle_type",
        "//score/mw/com/impl:instance_identifier",
        "//score/mw/com/impl:instance_specifier",
        "//score/mw/com/impl/configuration",
    ],
)
