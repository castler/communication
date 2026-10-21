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
"""ITF plugin that boots the Fuchsia emulator (FEMU) and runs test components on it.

Provides the `target` fixture (a `FemuTarget`). Everything (ffx, SDK, product bundle,
packages) is taken from runfiles, no network access is needed.
"""

import dataclasses
import pathlib

import pytest

from .femu_process import REPO_NAME, Femu


def pytest_addoption(parser):
    parser.addoption("--femu-ffx", action="store", required=True)
    parser.addoption("--femu-product-bundle", action="store", required=True,
                     help="Path to product_bundle.json")
    parser.addoption("--femu-package", action="append", default=[],
                     help="Package archive (.far) to publish, repeatable")


@dataclasses.dataclass
class RunResult:
    returncode: int
    output: str


class FemuTarget:
    def __init__(self, femu):
        self._femu = femu

    def run_test_component(self, url):
        """Runs `<package>#<manifest>` below the test repository via `ffx test run`."""
        result = self._femu.ffx(
            "test", "run", f"fuchsia-pkg://{REPO_NAME}/{url}", check=False, capture=True
        )
        return RunResult(result.returncode, result.stdout)


@pytest.fixture(scope="session")
def femu(request):
    opt = request.config.getoption
    # Runfiles are symlinks, ffx locates the SDK relative to its real path.
    ffx = pathlib.Path(opt("femu_ffx")).resolve()
    bundle = pathlib.Path(opt("femu_product_bundle")).resolve()
    emulator = Femu(ffx, bundle, keep_running=False)
    try:
        emulator.start()
        emulator.deploy([pathlib.Path(p).resolve() for p in opt("femu_package")])
        yield emulator
    finally:
        emulator.stop()


@pytest.fixture(scope="session")
def target(femu):
    return FemuTarget(femu)
