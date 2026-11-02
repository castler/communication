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


def test_fuchsia_attach(target):
    result = target.run_test_component("fuchsia_attach_test#meta/attach_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "FUCHSIA_ATTACH_TEST PASS" in result.output, result.output
    assert "FAIL" not in result.output, result.output


def test_discovery_service(target):
    result = target.run_test_component("fuchsia_attach_test#meta/discovery_service_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "DISCOVERY_SERVICE_TEST PASS" in result.output, result.output


def test_fidl_rights_on_receive(target):
    result = target.run_test_component("fuchsia_attach_test#meta/rights_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "RIGHTS_REDUCTION_TEST PASS" in result.output, result.output


def test_event_notification(target):
    result = target.run_test_component("fuchsia_attach_test#meta/events_test.cm")
    print(result.output)
    assert result.returncode == 0, result.output
    assert "EVENTS_TEST PASS" in result.output, result.output
