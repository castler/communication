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


def test_fuchsia_broker_hardening(target, capsys):
    result = target.run_test_component("fuchsia_broker_test#meta/broker_hardening_test.cm")
    # pytest hides the stdout of a passing test: print the evidence of the run past the capture.
    with capsys.disabled():
        print("\n" + result.output)
    assert result.returncode == 0, result.output
    assert "HARDENING_TEST PASS" in result.output, result.output
    assert "FAIL" not in result.output, result.output
