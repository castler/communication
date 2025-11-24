# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Pytest configuration file for providing fixtures and utilities to test llvm-cov tools."""

import pathlib
import subprocess
import sys
import typing as t

import pytest
import pytest_mock

from quality_tools.llvm_cov import llvm, meta

DEFAULT_META_INFO_DICT: t.Dict[str, t.Any] = {
    "llvm_bin_dir": str(pathlib.Path("llvm_bin_dir").absolute()),
    "execroot": str(pathlib.Path("execroot").absolute()),
    "profdata": str(pathlib.Path("profdata").absolute()),
    "object_files": [
        str(pathlib.Path("execroot", "file1").absolute()),
        str(pathlib.Path("execroot", "file2").absolute()),
    ],
    "matched_sources": [
        str(pathlib.Path("execroot", "file3").absolute()),
    ],
    "user_config": {
        "html_flat_view": True,
        "show_instantiations": True,
        "show_expansions": True,
        "show_mcdc": True,
        "export_json": True,
    },
}


def default_meta_info() -> meta.Info:
    """Create a default meta.Info instance for testing."""
    return meta.Info(
        llvm_bin_dir=pathlib.Path(DEFAULT_META_INFO_DICT["llvm_bin_dir"]),
        execroot=pathlib.Path(DEFAULT_META_INFO_DICT["execroot"]),
        profdata=pathlib.Path(DEFAULT_META_INFO_DICT["profdata"]),
        object_files=[*map(pathlib.Path, DEFAULT_META_INFO_DICT["object_files"])],
        matched_sources=[*map(pathlib.Path, DEFAULT_META_INFO_DICT["matched_sources"])],
        user_config=llvm.UserConfiguration(**DEFAULT_META_INFO_DICT["user_config"]),
    )


@pytest.fixture
def sys_argv_mock(
    mocker: pytest_mock.MockerFixture,
    request: pytest.FixtureRequest,
) -> t.List[str]:
    """Fixture that patches sys.argv with arguments."""
    args: t.List[str] = request.param
    mocker.patch.object(sys, "argv", ["does_not_matter", *args])
    return args


@pytest.fixture
def subprocess_mock(
    mocker: pytest_mock.MockerFixture,
    request: pytest.FixtureRequest,
):
    """Fixture that mocks the subprocess.run function."""
    set_failure = getattr(request, "param", False)
    mock = mocker.patch.object(subprocess, "run")
    if set_failure:
        mock.side_effect = lambda args, **_: (_ for _ in ()).throw(
            subprocess.CalledProcessError(
                cmd=args,
                returncode=1,
                output="Fixture was parametrized to set failure to True.",
            )
        )
    else:
        mock.side_effect = lambda args, **_: subprocess.CompletedProcess(
            args=args,
            returncode=0,
            stdout=" ".join(args),
        )


@pytest.fixture
def operator_mock(mocker: pytest_mock.MockerFixture) -> llvm.Operator:
    """Create an llvm.Operator instance with a mocked _execute method."""
    llvm_bin_dir = pathlib.Path("llvm_bin")
    execroot = pathlib.Path("execroot")
    mock = mocker.patch.object(llvm.Operator, "_execute")
    mock.side_effect = " ".join
    return llvm.Operator(llvm_bin_dir, execroot)
