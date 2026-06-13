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

load("@rules_python//python:defs.bzl", "py_binary")

py_binary(
    name = "merger",
    srcs = ["merger.py"],
    visibility = ["//visibility:public"],
)

py_binary(
    name = "reporter",
    srcs = ["reporter.py"],
    data = [
        ":effective_coverage",
        ":justify",
    ],
    visibility = ["//visibility:public"],
)

py_binary(
    name = "justify",
    srcs = ["justify.py"],
    visibility = ["//visibility:public"],
    deps = [
        "@score_communication_pip//pyyaml",
    ],
)

py_binary(
    name = "effective_coverage",
    srcs = ["effective_coverage.py"],
    visibility = ["//visibility:public"],
)
