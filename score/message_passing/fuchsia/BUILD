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
    "fuchsia_cc_test",
    "fuchsia_component",
    "fuchsia_component_manifest",
    "fuchsia_fidl_library",
    "fuchsia_test_component",
    "fuchsia_test_package",
    "fuchsia_unittest_package",
)
load("//score/mw:common_features.bzl", "COMPILER_WARNING_FEATURES")

# Fuchsia targets are only built with --config=fuchsia, see quality/integration_testing/environments/femu.

fuchsia_fidl_library(
    name = "connector_fidl",
    srcs = ["connector.fidl"],
    cc_bindings = ["cpp"],
    library = "score.messagepassing",
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//visibility:private"],
    deps = ["@fuchsia_sdk//fidl/zx"],
)

cc_library(
    name = "connection_registry",
    srcs = ["connection_registry.cpp"],
    hdrs = ["connection_registry.h"],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = ["//score/message_passing:__subpackages__"],
    deps = [
        ":connector_fidl_cpp",
        "//score/message_passing:common_headers",
        "@fuchsia_sdk//pkg/async-loop-cpp",
        "@fuchsia_sdk//pkg/async-loop-default",
        "@fuchsia_sdk//pkg/component_incoming_cpp",
        "@fuchsia_sdk//pkg/component_outgoing_cpp",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/zx",
        "@score_baselibs//score/os:errno",
    ],
)

cc_library(
    name = "message_passing_fuchsia",
    srcs = [
        "fuchsia_client_factory.cpp",
        "fuchsia_engine.cpp",
        "fuchsia_server.cpp",
        "fuchsia_server_factory.cpp",
    ],
    hdrs = [
        "fuchsia_client_factory.h",
        "fuchsia_engine.h",
        "fuchsia_server.h",
        "fuchsia_server_factory.h",
    ],
    features = COMPILER_WARNING_FEATURES,
    tags = ["manual"],
    target_compatible_with = ["@platforms//os:fuchsia"],
    visibility = [
        "//score/message_passing:__pkg__",
        "//score/mw/com/impl:__subpackages__",
    ],
    deps = [
        ":connection_registry",
        "//score/message_passing:message_passing_common",
        "@score_baselibs//score/os:sys_poll",
        "@score_baselibs//score/os:unistd",
    ],
)

# Server and client in separate components, connected through the Connector FIDL protocol.
fuchsia_cc_binary(
    name = "echo_server",
    srcs = ["echo_server.cpp"],
    tags = ["manual"],
    deps = [
        "//score/message_passing",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "echo_server_manifest",
    src = "meta/echo_server.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "echo_server_component",
    manifest = ":echo_server_manifest",
    tags = ["manual"],
    deps = [":echo_server"],
)

fuchsia_cc_binary(
    name = "echo_client_test",
    srcs = ["echo_client_test.cpp"],
    tags = ["manual"],
    deps = [
        "//score/message_passing",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "echo_client_test_manifest",
    src = "meta/echo_client_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "echo_client_test_component",
    manifest = ":echo_client_test_manifest",
    tags = ["manual"],
    deps = [":echo_client_test"],
)

fuchsia_test_package(
    name = "cross_component_echo_test_pkg",
    package_name = "message_passing_cross_component_echo_test",
    components = [":echo_server_component"],
    tags = ["manual"],
    test_components = [":echo_client_test_component"],
    visibility = ["//quality/integration_testing:__subpackages__"],
)

# Server and client within one process.
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

# Failure injection test of the connection registry.
fuchsia_cc_test(
    name = "connection_registry_test",
    srcs = ["connection_registry_test.cpp"],
    tags = ["manual"],
    deps = [
        ":connection_registry",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_unittest_package(
    name = "connection_registry_test_pkg",
    package_name = "message_passing_connection_registry_test",
    tags = ["manual"],
    unit_tests = [":connection_registry_test"],
    visibility = ["//quality/integration_testing:__subpackages__"],
)

# Slow subscriber: senders must not block on a peer that does not read, and framing must stay intact.
fuchsia_cc_test(
    name = "stalled_reader_test",
    srcs = ["stalled_reader_test.cpp"],
    tags = ["manual"],
    deps = [
        ":message_passing_fuchsia",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
        "@fuchsia_sdk//pkg/zx",
    ],
)

fuchsia_unittest_package(
    name = "stalled_reader_test_pkg",
    package_name = "message_passing_stalled_reader_test",
    tags = ["manual"],
    unit_tests = [":stalled_reader_test"],
    visibility = ["//quality/integration_testing:__subpackages__"],
)
