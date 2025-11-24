# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for the path_finder module."""

import os
import pathlib
import tempfile
from unittest import mock

import pytest

from quality_tools.llvm_cov import exceptions, path_finder


def test_execroot():
    """Tests for path_finder.execroot.

    Covered use cases:
    - Bad os.environ, `TEST_WORKSPACE` key not found (exception)
    - Happy path
    """

    # Must contain external.
    llvm_bin_dir = "/abs/path/to/external/does/not/matter"
    # Must be relative.
    test_workspace = "relative/path/to/workspace"
    # Absolute path to `llvm_bin_dir/execroot/test_workspace`.
    expected_path = "/abs/path/to/execroot/relative/path/to/workspace"

    with mock.patch.dict(os.environ, {}, clear=True):
        with pytest.raises(KeyError):
            path_finder.execroot(pathlib.Path(llvm_bin_dir))

    with mock.patch.dict(os.environ, {"TEST_WORKSPACE": test_workspace}, clear=True):
        actual = path_finder.execroot(llvm_bin_dir)

    assert actual.as_posix() == pathlib.Path(expected_path).as_posix()


def test_llvm_bin_dir():
    """Tests for path_finder.llvm_bin_dir.

    Covered use cases:
    - Path not found (exception)
    - Happy path, a unique path was found
    - Multiple paths are found (exception)
    """

    with tempfile.TemporaryDirectory() as temp_dir:
        # Change CWD because `path_finder.llvm_bin_dir` relies on `Path.cwd()``.
        old_cwd = os.getcwd()
        os.chdir(temp_dir)

        with pytest.raises(exceptions.LlvmBinRootPathNotFound):
            path_finder.llvm_bin_dir()

        # Arrange a llvm-cov file.
        llvm_cov = pathlib.Path("external/host/bin/llvm-cov")
        llvm_cov.parent.mkdir(parents=True)
        llvm_cov.touch()

        expected_final_path = str(pathlib.Path("host/bin"))

        actual = str(path_finder.llvm_bin_dir())

        assert actual.endswith(expected_final_path)

        # Arrange a duplicate llvm-cov file.
        llvm_cov = pathlib.Path("external/duplicate/bin/llvm-cov")
        llvm_cov.parent.mkdir(parents=True)
        llvm_cov.touch()

        with pytest.raises(exceptions.MultipleLlvmBinRootPathsFound):
            path_finder.llvm_bin_dir()

        os.chdir(old_cwd)
