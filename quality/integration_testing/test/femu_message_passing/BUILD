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

load("//quality/integration_testing:integration_testing.bzl", "femu_integration_test")

femu_integration_test(
    name = "message_passing_itf_test",
    srcs = ["test_message_passing.py"],
    packages = [
        "//score/message_passing/fuchsia:cross_component_echo_test_pkg",
        "//score/message_passing/fuchsia:echo_test_pkg",
        "//score/message_passing/fuchsia:stalled_reader_test_pkg",
    ],
)
