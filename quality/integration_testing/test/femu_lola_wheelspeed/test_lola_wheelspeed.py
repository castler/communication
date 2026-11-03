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
import re


def _metric(output, label, key):
    match = re.search(rf"DRIVER {label}:.*?{key}=(-?\d+)", output)
    assert match, f"{key} of {label} missing in output"
    return int(match.group(1))


def test_wheelspeed_end_to_end(target, capfd):
    result = target.run_test_component("lola_wheelspeed#meta/wheelspeed_driver.cm", min_severity="INFO")
    with capfd.disabled():
        print("\n".join(line for line in result.output.splitlines() if "DRIVER" in line or "WHEELSPEED" in line))
    logs = "\n".join(target.dump_logs().splitlines()[-400:])
    print("---- target log (wheelspeed components) ----")
    print(logs)
    out = result.output
    assert result.returncode == 0, out + "\n---- target log ----\n" + logs
    assert "WHEELSPEED PASS" in out, out
    for label in ("first", "restart"):
        assert _metric(out, label, "samples") >= 100, out
        assert _metric(out, label, "order_errors") == 0, out
        assert _metric(out, label, "payload_errors") == 0, out
        assert _metric(out, label, "found") == 1, out
    # The restart is a new skeleton incarnation: the proxy had to re-find it and re-subscribe.
    assert _metric(out, "first", "incarnation") != _metric(out, "restart", "incarnation"), out
    assert _metric(out, "first", "receive_handler_calls") > 0, out
    assert "FAIL" not in out.replace("WHEELSPEED PASS", ""), out
