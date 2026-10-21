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
load("@rules_python//python:defs.bzl", "py_library")
load("@score_itf//bazel:py_itf_plugin.bzl", "py_itf_plugin")

py_library(
    name = "femu_lib",
    srcs = [
        "__init__.py",
        "femu_process.py",
    ],
    imports = [".."],
)

py_itf_plugin(
    name = "femu_plugin",
    enabled_plugins = ["femu"],
    py_library = ":femu_lib",
    visibility = ["//quality/integration_testing:__subpackages__"],
)
