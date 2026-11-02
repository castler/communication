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


def test_vmo_shm_across_processes(target):
    result = target.run_test_component("vmo_shm_test#meta/vmo_shm_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "VMO_SHM_TEST child PASS" in result.output, result.output
    assert "VMO_SHM_TEST parent PASS" in result.output, result.output
    assert "FAIL" not in result.output, result.output
