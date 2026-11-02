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


def test_shm_probe_runs_on_fuchsia(target):
    result = target.run_test_component("shm_probe_test#meta/shm_probe_test.cm")
    print(result.output)
    assert "PROBE done" in result.output, result.output
