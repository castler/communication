# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for the llvm module."""

import pathlib
import subprocess
import typing as t
from contextlib import nullcontext

import pytest

from quality_tools.llvm_cov import llvm


def test_operator_instantiation():
    """Test operator.Operator instantiation."""

    llvm_bin_dir = pathlib.Path("llvm_bin")
    execroot = pathlib.Path("execroot")

    operator = llvm.Operator(llvm_bin_dir, execroot)

    assert operator.llvm_bin_dir == llvm_bin_dir
    assert operator.llvm_cov_bin == llvm_bin_dir / "llvm-cov"
    assert operator.llvm_profdata_bin == llvm_bin_dir / "llvm-profdata"
    assert operator.llvm_demangler_bin == llvm_bin_dir / "llvm-cxxfilt"
    assert operator.execroot == execroot


@pytest.mark.parametrize("command", [["happy", "path"]])
@pytest.mark.usefixtures("subprocess_mock")
def test_operator_execute(command: t.List[str]):
    """Test operator.Operator._execute method."""
    dummy_path = pathlib.Path()
    operator = llvm.Operator(dummy_path, dummy_path)
    expected_output = " ".join(command)

    output = operator._execute(command)  # pylint: disable=protected-access

    assert expected_output == output


@pytest.mark.parametrize("command", [["exception", "path"]])
@pytest.mark.parametrize("subprocess_mock", [True], indirect=["subprocess_mock"])
@pytest.mark.usefixtures("subprocess_mock")
def test_operator_execute_exception_path(command: t.List[str], caplog: pytest.LogCaptureFixture):
    """Test operator.Operator._execute method."""
    dummy_path = pathlib.Path()
    operator = llvm.Operator(dummy_path, dummy_path)

    with pytest.raises(subprocess.CalledProcessError):
        operator._execute(command)  # pylint: disable=protected-access

    assert "The command" in caplog.text
    assert f"{command}" in caplog.text


@pytest.mark.parametrize("create_output", [True, False])
def test_operator_profdata_merge(operator_mock: llvm.Operator, tmp_path: pathlib.Path, create_output: bool):
    """Test operator.Operator.profdata_merge method."""
    files = [
        pathlib.Path("file1"),
        pathlib.Path("file2"),
    ]
    output_file = tmp_path / "output.profdata"

    expected_strings_in_stdout = [
        str(operator_mock.llvm_profdata_bin),
        "merge",
        str(output_file),
        *(map(str, files)),
    ]

    if create_output:
        output_file.touch()

    with nullcontext() if create_output else pytest.raises(AssertionError):
        stdout = operator_mock.profdata_merge(files, output_file)
        for string in expected_strings_in_stdout:
            assert string in stdout


def test_operator_create_lcov_report(operator_mock: llvm.Operator):
    """Test operator.Operator.create_lcov_report method."""
    profdata = pathlib.Path("file1")
    object_files = {
        operator_mock.execroot / "file2",
        operator_mock.execroot / "file3",
    }
    report_dir = pathlib.Path("dir1")

    expected_strings_in_stdout = [
        str(operator_mock.llvm_cov_bin),
        "export",
        str(operator_mock.llvm_demangler_bin),
        f"--path-equivalence=/proc/self/cwd/,{operator_mock.execroot}",
        f"--compilation-dir={operator_mock.execroot}",
        f"--instr-profile={profdata}",
        "--object=file2",
        "--object=file3",
    ]

    cov_info = llvm.CoverageInformation(profdata, object_files, set(), report_dir)
    lcov_report_dir = operator_mock.create_lcov_report(cov_info)
    command_stdout = lcov_report_dir.joinpath("command_output.txt").read_text()

    for string in expected_strings_in_stdout:
        assert string in command_stdout


def test_operator_create_json_report(operator_mock: llvm.Operator):
    """Test operator.Operator.create_json_report method."""
    profdata = pathlib.Path("file1")
    object_files = {
        operator_mock.execroot / "file2",
        operator_mock.execroot / "file3",
    }
    report_dir = pathlib.Path("dir1")

    expected_strings_in_stdout = [
        str(operator_mock.llvm_cov_bin),
        "export",
        str(operator_mock.llvm_demangler_bin),
        f"--path-equivalence=/proc/self/cwd/,{operator_mock.execroot}",
        f"--compilation-dir={operator_mock.execroot}",
        f"--instr-profile={profdata}",
        "--object=file2",
        "--object=file3",
    ]

    cov_info = llvm.CoverageInformation(profdata, object_files, set(), report_dir)
    json_report_dir = operator_mock.create_json_report(cov_info)
    command_stdout = json_report_dir.joinpath("report.json").read_text()

    for string in expected_strings_in_stdout:
        assert string in command_stdout


@pytest.mark.parametrize("html_flat_view", [True, False])
@pytest.mark.parametrize("show_instantiations", [True, False])
@pytest.mark.parametrize("show_expansions", [True, False])
@pytest.mark.parametrize("show_mcdc", [True, False])
def test_operator_create_html_report(
    operator_mock: llvm.Operator,
    html_flat_view: bool,
    show_instantiations: bool,
    show_expansions: bool,
    show_mcdc: bool,
):
    """Test operator.Operator.create_html_report method."""
    profdata = pathlib.Path("file1")
    object_files = {
        operator_mock.execroot / pathlib.Path("file2"),
        operator_mock.execroot / pathlib.Path("file3"),
    }
    matched_sources = {
        operator_mock.execroot / pathlib.Path("file4"),
        operator_mock.execroot / pathlib.Path("file5"),
    }
    report_dir = pathlib.Path("dir1")

    expected_strings_in_stdout = [
        str(operator_mock.llvm_cov_bin),
        "show",
        "--format=html",
        str(operator_mock.llvm_demangler_bin),
        f"--show-directory-coverage={not html_flat_view}",
        f"--show-instantiations={show_instantiations}",
        f"--show-expansions={show_expansions}",
        f"--show-mcdc={show_mcdc}",
        f"--path-equivalence=/proc/self/cwd/,{operator_mock.execroot}",
        f"--output-dir={report_dir / 'html_report'}",
        "--show-branches=count",
        f"--compilation-dir={operator_mock.execroot}",
        f"--instr-profile={profdata}",
        "--object=file2",
        "--object=file3",
        "--sources",
        "file4",
        "file5",
    ]

    cov_info = llvm.CoverageInformation(profdata, object_files, matched_sources, report_dir)
    user_config = llvm.UserConfiguration(
        html_flat_view=html_flat_view,
        show_instantiations=show_instantiations,
        show_expansions=show_expansions,
        show_mcdc=show_mcdc,
        export_json=False,
    )

    html_report_dir = operator_mock.create_html_report(cov_info, user_config)
    command_stdout = html_report_dir.joinpath("command_output.txt").read_text()

    for string in expected_strings_in_stdout:
        assert string in command_stdout


@pytest.mark.parametrize("show_instantiations", [True, False])
@pytest.mark.parametrize("show_expansions", [True, False])
@pytest.mark.parametrize("show_mcdc", [True, False])
def test_operator_create_text_report(
    operator_mock: llvm.Operator,
    show_instantiations: bool,
    show_expansions: bool,
    show_mcdc: bool,
):
    """Test operator.Operator.create_text_report method."""
    profdata = pathlib.Path("file1")
    object_files = {
        operator_mock.execroot / pathlib.Path("file2"),
        operator_mock.execroot / pathlib.Path("file3"),
    }
    report_dir = pathlib.Path("dir1")

    expected_strings_in_stdout = [
        str(operator_mock.llvm_cov_bin),
        "show",
        str(operator_mock.llvm_demangler_bin),
        f"--show-instantiations={show_instantiations}",
        f"--show-expansions={show_expansions}",
        f"--show-mcdc={show_mcdc}",
        f"--path-equivalence=/proc/self/cwd/,{operator_mock.execroot}",
        f"--compilation-dir={operator_mock.execroot}",
        f"--instr-profile={profdata}",
        "--object=file2",
        "--object=file3",
    ]

    cov_info = llvm.CoverageInformation(profdata, object_files, set(), report_dir)
    user_config = llvm.UserConfiguration(
        html_flat_view=False,
        show_instantiations=show_instantiations,
        show_expansions=show_expansions,
        show_mcdc=show_mcdc,
        export_json=False,
    )
    text_report_dir = operator_mock.create_text_report(cov_info, user_config)
    command_stdout = text_report_dir.joinpath("command_output.txt").read_text()

    for string in expected_strings_in_stdout:
        assert string in command_stdout


@pytest.mark.parametrize("show_instantiations", [True, False])
@pytest.mark.parametrize("show_mcdc", [True, False])
def test_operator_create_text_summary_report(operator_mock: llvm.Operator, show_instantiations: bool, show_mcdc: bool):
    """Test operator.Operator.create_text_summary_report method."""
    profdata = pathlib.Path("file1")
    object_files = {
        operator_mock.execroot / pathlib.Path("file2"),
        operator_mock.execroot / pathlib.Path("file3"),
    }
    report_dir = pathlib.Path("dir1")

    expected_strings_in_stdout = [
        str(operator_mock.llvm_cov_bin),
        "report",
        "--summary-only",
        "--show-region-summary=0",
        "--show-branch-summary=1",
        f"--show-mcdc-summary={show_mcdc}",
        "--path-equivalence=/proc/self/cwd/bazel-out,execroot/bazel-out",
        "--path-equivalence=/proc/self/cwd/external,execroot/external",
        "--path-equivalence=/proc/self/cwd/,execroot",
        "--compilation-dir=execroot",
        "--instr-profile=file1",
        "--object=file2",
        "--object=file3",
    ]

    cov_info = llvm.CoverageInformation(profdata, object_files, set(), report_dir)
    user_config = llvm.UserConfiguration(
        html_flat_view=False,
        show_instantiations=show_instantiations,
        show_expansions=False,
        show_mcdc=show_mcdc,
        export_json=False,
    )

    text_report_dir = operator_mock.create_text_summary_report(cov_info, user_config)
    summary_content = text_report_dir.joinpath("summary.txt").read_text()

    for string in expected_strings_in_stdout:
        assert string in summary_content
