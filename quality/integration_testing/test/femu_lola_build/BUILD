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
    "fuchsia_package",
)

# Build-only: `bazel build --config=fuchsia //quality/integration_testing/test/femu_lola_build:pkg`.
fuchsia_cc_binary(
    name = "lola_build",
    srcs = ["main.cc"],
    tags = ["manual"],
    deps = [
        "//score/mw/com",
        "@score_baselibs//score/string_manipulation/arguments",
        "@fuchsia_sdk//pkg/fdio",
        "@fuchsia_sdk//pkg/syslog",
    ],
)

fuchsia_component_manifest(
    name = "manifest",
    src = "meta/lola_build.cml",
    includes = ["@fuchsia_sdk//pkg/syslog:client"],
    tags = ["manual"],
)

fuchsia_component(
    name = "component",
    manifest = ":manifest",
    tags = ["manual"],
    deps = [":lola_build"],
)

fuchsia_package(
    name = "pkg",
    package_name = "lola_build",
    components = [":component"],
    tags = ["manual"],
)
