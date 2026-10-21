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


def test_hello_world_runs_on_fuchsia(target):
    result = target.run_test_component("hello_world_test#meta/hello_world_test.cm")
    assert result.returncode == 0, result.output
    assert "Hello, World from Fuchsia!" in result.output
