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
"""Controls a Fuchsia emulator (FEMU) through ffx."""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import uuid

REPO_NAME = "femu.test"


def _log(message):
    print(f"[femu] {message}", file=sys.stderr, flush=True)


class Femu:
    def __init__(self, ffx, product_bundle, keep_running):
        self._ffx = ffx
        self._product_bundle = product_bundle
        self._keep_running = keep_running
        self._name = f"femu-{uuid.uuid4().hex[:8]}"
        self._tmp = pathlib.Path(tempfile.mkdtemp(prefix="femu_"))
        self._env = dict(os.environ)
        # Every run gets its own ffx daemon/config/emulator state.
        self._env["FFX_ISOLATE_DIR"] = str(self._tmp / "ffx")
        home = self._tmp / "home"
        home.mkdir(exist_ok=True)
        self._env["HOME"] = str(home)
        self._env["XDG_DATA_HOME"] = str(home / ".local/share")
        self._env["XDG_CONFIG_HOME"] = str(home / ".config")
        self._env["XDG_CACHE_HOME"] = str(home / ".cache")
        (self._tmp / "ffx").mkdir()

    def ffx(self, *args, check=True, capture=False):
        _log("ffx " + " ".join(str(a) for a in args))
        return subprocess.run(
            [str(self._ffx), *map(str, args)],
            env=self._env,
            check=check,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
        )

    def start(self):
        if not os.access("/dev/kvm", os.R_OK | os.W_OK):
            _log("WARNING: /dev/kvm is not accessible, the emulator will be very slow")
        self.ffx(
            "emu", "start", self._product_bundle.parent,
            "--headless", "--net", "user", "--name", self._name,
        )

    def deploy(self, package_archives):
        repo = self._tmp / "repo"
        self.ffx("repository", "create", repo)
        for far in package_archives:
            self.ffx("repository", "publish", "--package-archive", far, repo)
        self.ffx(
            "repository", "server", "start", "--background",
            "--repo-path", repo, "-r", REPO_NAME, "--address", "[::1]:0",
        )

    def run_test(self, url, expected_output):
        result = self.ffx("test", "run", url, check=False, capture=True)
        print(result.stdout)
        if result.returncode != 0:
            _log(f"test failed with exit code {result.returncode}")
            return result.returncode
        if expected_output and expected_output not in result.stdout:
            _log(f"expected output not found: {expected_output!r}")
            return 1
        return 0

    def stop(self):
        if not self._keep_running:
            self.ffx("repository", "server", "stop", "--all", check=False)
            self.ffx("emu", "stop", "--all", check=False)
            shutil.rmtree(self._tmp, ignore_errors=True)
