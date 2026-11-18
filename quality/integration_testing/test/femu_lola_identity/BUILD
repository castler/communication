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
    "fuchsia_component",
    "fuchsia_component_manifest",
    "fuchsia_test_component",
    "fuchsia_test_package",
)
load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

# Authoritative identity of message_passing on Fuchsia: the provider derives the principal of a connection from the
# Connector endpoint the realm routed to the consumer, never from the consumer. Consumers A (this test) and C are
# routed as 7 (allowed, different processes -> different nodes), consumer B as 8 (denied by IsAllowed), also when
# it claims to be 7.

cc_library(
    name = "identity_protocol",
    hdrs = ["identity_protocol.h"],
    tags = ["manual"],
)

fuchsia_cc_binary(
    name = "identity_server",
    srcs = ["identity_server.cc"],
    tags = ["manual"],
    deps = [
        ":identity_protocol",
        "//score/message_passing",
        "//score/message_passing/fuchsia:connection_registry",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_cc_binary(
    name = "identity_client",
    srcs = ["identity_client.cc"],
    tags = ["manual"],
    deps = [
        ":identity_protocol",
        "//score/message_passing",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "identity_server_manifest",
    src = "meta/identity_server.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "identity_server_component",
    manifest = ":identity_server_manifest",
    tags = ["manual"],
    deps = [":identity_server"],
)

fuchsia_component_manifest(
    name = "identity_client_allowed_manifest",
    src = "meta/identity_client_allowed.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "identity_client_allowed_component",
    manifest = ":identity_client_allowed_manifest",
    tags = ["manual"],
    deps = [":identity_client"],
)

fuchsia_component_manifest(
    name = "identity_client_denied_manifest",
    src = "meta/identity_client_denied.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "identity_client_denied_component",
    manifest = ":identity_client_denied_manifest",
    tags = ["manual"],
    deps = [":identity_client"],
)

fuchsia_component_manifest(
    name = "identity_test_manifest",
    src = "meta/identity_test.cml",
    includes = [
        "@fuchsia_sdk//pkg/sys/testing:elf_test_runner",
        "@fuchsia_sdk//pkg/syslog:client",
    ],
    tags = ["manual"],
)

fuchsia_test_component(
    name = "identity_test_component",
    manifest = ":identity_test_manifest",
    tags = ["manual"],
    deps = [":identity_client"],
)

fuchsia_test_package(
    name = "test_pkg",
    package_name = "lola_identity_test",
    components = [
        ":identity_client_allowed_component",
        ":identity_client_denied_component",
        ":identity_server_component",
    ],
    tags = ["manual"],
    test_components = [":identity_test_component"],
)

femu_integration_test(
    name = "lola_identity_itf_test",
    srcs = ["test_lola_identity.py"],
    packages = [":test_pkg"],
)
