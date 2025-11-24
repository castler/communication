# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for the output_generator module."""

import os
import pathlib
import typing as t
import zipfile
from contextlib import nullcontext
from copy import deepcopy

import pytest
import pytest_mock

from quality_tools.llvm_cov import llvm, meta, report_generator
from quality_tools.llvm_cov.test import conftest


@pytest.mark.parametrize(
    "sys_argv_mock",
    [
        [
            "--output_file",
            "file1",
            "--reports_file",
            "file2",
        ],
    ],
    indirect=["sys_argv_mock"],
)
def test_parse_args(sys_argv_mock: t.List[str]):
    """Test that the parse_args function returns the expected Arguments instance from valid arguments."""
    expected_output_file = pathlib.Path(sys_argv_mock[1])
    expected_reports_file = pathlib.Path(sys_argv_mock[3])

    args = report_generator.parse_args()

    assert args.output_file == expected_output_file
    assert args.reports_file == expected_reports_file


@pytest.mark.parametrize(
    "sys_argv_mock",
    [
        [],
        [
            "--reports_file",
            "file2",
        ],
        [
            "--output_file",
            "file1",
        ],
        [
            "--output_file",
            "file1",
            "--reports_file",
            "file2",
            "--unknown_not_allowed",
            "random",
        ],
    ],
    indirect=["sys_argv_mock"],
)
@pytest.mark.usefixtures("sys_argv_mock")
def test_parse_args_with_invalid_args():
    """Test that the parse_args function fails from invalid arguments."""
    with pytest.raises(SystemExit):
        report_generator.parse_args()


def create_report_file_with_meta(
    root: pathlib.Path,
    no_matched_sources: bool,
    export_json: bool,
) -> t.Tuple[pathlib.Path, t.List[pathlib.Path], t.List[meta.Data]]:
    """Create a report file with meta information for testing."""
    profdata_files = [
        root / "1" / "file.profdata",
        root / "2" / "file.profdata",
    ]

    meta_zip_files = [
        root / "1" / "coverage.dat",
        root / "2" / "coverage.dat",
        root / "3" / "empty_coverage.dat",
        root / "4" / "baseline_coverage.dat",
    ]

    default_meta_info = conftest.default_meta_info()
    default_meta_info.execroot = report_generator.strip_sandbox(default_meta_info.execroot)
    for index, _ in enumerate(default_meta_info.matched_sources):
        default_meta_info.matched_sources[index] = report_generator.strip_sandbox(
            default_meta_info.matched_sources[index]
        )
    if no_matched_sources:
        default_meta_info.matched_sources.clear()
    if not export_json:
        default_meta_info.user_config.export_json = False

    meta_datas = [
        meta.Data(directory=root / "1", info=deepcopy(default_meta_info)),
        meta.Data(directory=root / "2", info=deepcopy(default_meta_info)),
    ]

    for index, zip_file in enumerate(meta_zip_files):
        if index < len(meta_datas):
            meta_datas[index].info.profdata = profdata_files[index]
            meta_datas[index].info.profdata.touch()
            meta_datas[index].to_zip(zip_file)
        else:
            zip_file.parent.mkdir()
            zip_file.touch()

    reports_file = root / "reports_file.txt"
    reports_file.write_text("\n".join(str(file) for file in meta_zip_files))
    return reports_file, meta_zip_files, meta_datas


def test_get_meta_data_from_reports(tmp_path: pathlib.Path):
    """Test the report_generator.get_meta_data_from_reports method."""
    mock_report_file, meta_zip_files, mock_meta_datas = create_report_file_with_meta(
        root=tmp_path,
        no_matched_sources=False,
        export_json=False,
    )

    meta_datas = report_generator.get_meta_datas_from_reports(mock_report_file)

    assert len(meta_datas) == 2
    for index, meta_data in enumerate(meta_datas):
        # pylint: disable-next=protected-access
        assert (meta_zip_files[index].parent / "meta" / meta.Data._META_JSON).exists()
        # pylint: disable-next=protected-access
        assert (meta_zip_files[index].parent / "meta" / meta.Data._PROFDATA_JSON).exists()
        # pylint: disable-next=protected-access
        assert meta_data.profdata == meta_zip_files[index].parent / "meta" / meta.Data._PROFDATA_JSON
        assert meta_data.info == mock_meta_datas[index].info


def test_create_zip(tmp_path: pathlib.Path):
    """Test the create_zip function."""
    root = tmp_path

    directories = [
        tmp_path / "dir1",
        tmp_path / "dir2",
        tmp_path / "dir3",
    ]
    for directory in directories:
        directory.mkdir()

    files = [
        directories[0] / "file1.txt",
        directories[0] / "subdir" / "file2.txt",
        directories[0] / "subdir" / "file3.txt",
        directories[1] / "file4.txt",
        directories[1] / "file5.txt",
        directories[1] / "subdir" / "subdir" / "file6.txt",
    ]
    for file in files:
        file.parent.mkdir(parents=True, exist_ok=True)
        file.touch()

    expected_files = sorted(map(lambda file: file.relative_to(root), set(tmp_path.rglob("*")) - set(directories)))

    output_file = tmp_path / "output.zip"

    report_generator.create_zip(root, set(directories), output_file)

    with zipfile.ZipFile(output_file, "r") as archive:
        assert len(archive.namelist()) == 9
        assert sorted(map(pathlib.Path, archive.namelist())) == expected_files


@pytest.mark.parametrize(
    "path, expected_path",
    [
        # Matching sandbox paths
        (pathlib.Path("path/to/sandbox/1234-sandbox/5678/file"), pathlib.Path("path/to/file")),
        (pathlib.Path("/absolute/path/to/sandbox/1234-sandbox/5678/file"), pathlib.Path("/absolute/path/to/file")),
        (pathlib.Path("sandbox/abcd-sandbox/1234/file.txt"), pathlib.Path("file.txt")),
        (pathlib.Path("/absolute/sandbox/abcd-sandbox/1234/file.txt"), pathlib.Path("/absolute/file.txt")),
        (pathlib.Path("some/dir/sandbox/xyz123-sandbox/4567/another/file"), pathlib.Path("some/dir/another/file")),
        (
            pathlib.Path("/absolute/some/dir/sandbox/xyz123-sandbox/4567/another/file"),
            pathlib.Path("/absolute/some/dir/another/file"),
        ),
        (pathlib.Path("sandbox/5678-sandbox/91011/deeply/nested/file"), pathlib.Path("deeply/nested/file")),
        (
            pathlib.Path("/absolute/sandbox/5678-sandbox/91011/deeply/nested/file"),
            pathlib.Path("/absolute/deeply/nested/file"),
        ),
        # Non matching sandbox paths
        (pathlib.Path("path/to/sandbox/1234-sandbox/file"), pathlib.Path("path/to/sandbox/1234-sandbox/file")),
        (
            pathlib.Path("/absolute/path/to/sandbox/1234-sandbox/file"),
            pathlib.Path("/absolute/path/to/sandbox/1234-sandbox/file"),
        ),
        (pathlib.Path("sandbox/abcd-sandbox/file"), pathlib.Path("sandbox/abcd-sandbox/file")),
        (pathlib.Path("/absolute/sandbox/abcd-sandbox/file"), pathlib.Path("/absolute/sandbox/abcd-sandbox/file")),
        (pathlib.Path("no/sandbox/here/file"), pathlib.Path("no/sandbox/here/file")),
        (pathlib.Path("/absolute/no/sandbox/here/file"), pathlib.Path("/absolute/no/sandbox/here/file")),
    ],
)
def test_strip_sandbox(path: pathlib.Path, expected_path: pathlib.Path):
    """Test the report_generator.strip_sandbox method."""
    assert report_generator.strip_sandbox(path) == expected_path


@pytest.mark.parametrize(
    "no_meta",
    [False, True],
)
@pytest.mark.parametrize(
    "no_matched_sources",
    [False, True],
)
@pytest.mark.parametrize(
    "export_json",
    [False, True],
)
@pytest.mark.usefixtures("subprocess_mock")
def test_report_generator(
    tmp_path: pathlib.Path,
    mocker: pytest_mock.MockerFixture,
    no_meta: bool,
    no_matched_sources: bool,
    export_json: bool,
):
    """Test the report_generator.main method."""
    old_path = pathlib.Path.cwd()
    os.chdir(tmp_path)

    # Create a report file containing meta information.
    report_file = pathlib.Path(tmp_path / "fake_reports_file.txt")
    report_file.touch()
    if not no_meta:
        report_file, _, _ = create_report_file_with_meta(tmp_path, no_matched_sources, export_json)

    # Define output location.
    output_file = tmp_path / "output.dat"

    # Mock `parse_args` to ease setup.
    args = report_generator.Arguments(
        output_file=output_file,
        reports_file=report_file,
    )
    mocker.patch.object(report_generator, "parse_args", side_effect=[args])

    # Mock `shutil.copyfile` as we can not write files to execroot while testing.
    mocker.patch.object(llvm.shutil, "copyfile")

    # Spy on `llvm.Operator.create_json_report` to check call count.
    create_json_report_spy = mocker.spy(llvm.Operator, "create_json_report")
    expected_create_json_report_call_count = 1 if export_json else 0

    # As LLVM tools are not actually called, mock `target.profdata` as it is expected to be created.
    target_profdata_file = tmp_path / "coverage_report_merged.dat"
    target_profdata_file.touch()

    is_early_exit_expected = no_meta or no_matched_sources

    with pytest.raises(SystemExit) if is_early_exit_expected else nullcontext():
        report_generator.main()

    # Assert that `output_file` was created.
    if is_early_exit_expected:
        assert not output_file.exists()
    else:
        assert output_file.exists()
        assert create_json_report_spy.call_count == expected_create_json_report_call_count

    os.chdir(old_path)
