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


def test_lola_discovery(target):
    result = target.run_test_component("lola_discovery_test#meta/driver_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "LOLA_DISCOVERY_TEST PASS" in result.output, result.output
    assert "FAIL" not in result.output, result.output


def test_lola_discovery_transport_restart(target):
    result = target.run_test_component("lola_discovery_test#meta/transport_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "LOLA_DISCOVERY_TRANSPORT_TEST PASS" in result.output, result.output
    assert "FAIL" not in result.output, result.output
