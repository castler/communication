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


def test_message_passing_echo_on_fuchsia(target):
    result = target.run_test_component("message_passing_echo_test#meta/echo_test.cm")
    assert "echo OK" in result.output, result.output
    assert result.returncode == 0, result.output
