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


def test_lola_events_across_processes(target):
    result = target.run_test_component("lola_events_test#meta/lola_events_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "SKEL subscribers attached: 3" in result.output, result.output
    assert "ORCH all subscribers received phase A" in result.output, result.output
    assert "ORCH subscriber 2 died" in result.output, result.output
    assert "dead subscriber pruned=1" in result.output, result.output
    assert "ORCH remaining subscribers received phase B" in result.output, result.output
    assert "ORCH skeleton killed" in result.output, result.output
    assert "SUB 0 provider lost: counter=10000" in result.output, result.output
    assert "SUB 1 provider lost: counter=10000" in result.output, result.output
    assert "ORCH subscribers drained the final counter" in result.output, result.output
    assert "LOLA_EVENTS PASS" in result.output or "LOLA_EVENTS orchestrator PASS" in result.output, result.output
    assert "FAIL" not in result.output, result.output
