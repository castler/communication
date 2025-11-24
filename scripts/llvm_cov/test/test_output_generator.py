# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for the output_generator module."""

import pathlib
import typing as t
from contextlib import nullcontext

import pytest
import pytest_mock

from quality_tools.llvm_cov import exceptions, output_generator, path_finder


@pytest.mark.parametrize(
    "sys_argv_mock",
    [
        [
            "--coverage_dir",
            "file1",
            "--output_file",
            "file2",
            "--source_file_manifest",
            "file3",
        ],
        [
            "--coverage_dir",
            "file1",
            "--output_file",
            "file2",
            "--source_file_manifest",
            "file3",
            "--sources_to_replace_file",
            "unknown_not_used",
            "--filter_sources",
            "unknown_not_used",
        ],
    ],
    indirect=["sys_argv_mock"],
)
def test_parse_args(sys_argv_mock: t.List[str]):
    """Test that the parse_args function returns the expected Arguments instance from valid arguments."""
    expected_coverage_dir = pathlib.Path(sys_argv_mock[1])
    expected_output_file = pathlib.Path(sys_argv_mock[3])
    expected_source_file_manifest = pathlib.Path(sys_argv_mock[5])

    args = output_generator.parse_args()

    assert args.coverage_dir == expected_coverage_dir
    assert args.output_file == expected_output_file
    assert args.source_file_manifest == expected_source_file_manifest


@pytest.mark.parametrize(
    "sys_argv_mock",
    [
        [],
        [
            "--output_file",
            "file2",
            "--source_file_manifest",
            "file3",
        ],
        [
            "--coverage_dir",
            "file1",
            "--source_file_manifest",
            "file3",
        ],
        [
            "--coverage_dir",
            "file1",
            "--output_file",
            "file2",
        ],
    ],
    indirect=["sys_argv_mock"],
)
@pytest.mark.usefixtures("sys_argv_mock")
def test_parse_args_with_invalid_args():
    """Test that the parse_args function fails from invalid arguments."""
    with pytest.raises(SystemExit):
        output_generator.parse_args()


def test_get_profdata_file_names_as_seen_by_llvm_cov():
    """Test that the get_profdata_file_names_as_seen_by_llvm_cov returns the expected source files from lcov file."""
    lcov_stdout = pathlib.Path(__file__).parent.joinpath("data/lcov.dat").read_text(encoding="utf-8")

    expected_source_files = {
        "file1.cpp",
        "file2.cpp",
        "file3.cpp",
    }

    source_files = output_generator.get_profdata_file_names_as_seen_by_llvm_cov(lcov_stdout)

    assert source_files == expected_source_files


@pytest.mark.parametrize(
    "regex_or_file, file_content",
    [
        ("", []),
        (".*", []),
        (pathlib.Path("file1"), []),
        (pathlib.Path("file2"), ["path1", "path2"]),
    ],
)
def test_resolve_regex_or_file(
    tmp_path: pathlib.Path,
    regex_or_file: t.Union[str, pathlib.Path],
    file_content: t.List[str],
):
    """Test that the resolve_regex_or_file function returns the expected regex or file content."""
    if isinstance(regex_or_file, pathlib.Path):
        file = tmp_path.joinpath(regex_or_file)
        file.write_text("\n".join(file_content))
        input_regex_or_file = str(file)
    else:
        input_regex_or_file = regex_or_file

    resolved_regex_or_file = output_generator.resolve_regex_or_file(input_regex_or_file)

    if isinstance(regex_or_file, pathlib.Path):
        assert resolved_regex_or_file == file_content
    else:
        assert resolved_regex_or_file == input_regex_or_file


@pytest.mark.parametrize(
    "regex",
    [
        "[",  # Unclosed character class
        "(",  # Unclosed parenthesis
        "\\",  # Trailing backslash
        "*",  # Quantifier without preceding token
        "a{5,2}",  # Invalid quantifier range
    ],
)
def test_resolve_regex_or_file_with_invalid_regexes(regex: str):
    """Test that the resolve_regex_or_file function raises an exception for invalid regex patterns."""
    with pytest.raises(exceptions.InvalidLlvmRegexPattern):
        output_generator.resolve_regex_or_file(regex)


@pytest.mark.parametrize(
    "files, regex_or_filelist, expected_filtered_files",
    [
        ({"exec/file1", "exec/file2", "exec/file3"}, "", set()),
        ({"exec/file1", "exec/file2", "exec/file3"}, ["exec/file1"], {"exec/file1"}),
        ({"file1", "file2", "file3"}, ["file1"], {"file1"}),
        ({"exec/file1", "exec/file2", "exec/file3"}, ["exec/file4"], set()),
        ({"exec/file1", "exec/file2", "exec/file3"}, ".*", {"exec/file1", "exec/file2", "exec/file3"}),
        ({"exec/path/to/file1.cpp"}, r"^path/to/.*\.cpp$", {"exec/path/to/file1.cpp"}),
        ({"exec/path/to/file1.h"}, r"^path/to/.*\.cpp$", set()),
        ({"exec/path/test/file1.h"}, r".*test.*$", {"exec/path/test/file1.h"}),
        # Does NOT match, cause path is relative to "exec" and therefore not considered in the exclusion
        (
            {"exec/path/test/file1.h"},
            r".*exec.*$",
            set(),
        ),
        ({"other_root/path/test/file1.h"}, r".*path.*$", {"other_root/path/test/file1.h"}),
        ({"other_root/path/test/file1.h"}, r".*bla.*$", set()),
        # Does match, cause path is NOT relative to "exec" and therefore considered in the exclusion
        ({"other_root/path/test/file1.h"}, r".*other_root.*$", {"other_root/path/test/file1.h"}),
    ],
)
def test_filter_files(
    files: t.Set[str],
    regex_or_filelist: t.Union[t.List[str], str],
    expected_filtered_files: t.Set[str],
):
    """Test that the filter_files function returns the expected filtered files."""
    filtered_files = output_generator.filter_files(files, regex_or_filelist, pathlib.Path("exec"))

    assert filtered_files == expected_filtered_files


@pytest.mark.parametrize(
    "source_file_manifest_content, expected_object_list_files",
    [
        (
            [],
            set(),
        ),
        (
            ["path/to/random_file"],
            set(),
        ),
        (
            ["path/to/objects_list.txt"],
            {pathlib.Path("path/to/objects_list.txt")},
        ),
        (
            [
                "path/to/objects_list.txt",
                "path/to/random_file",
            ],
            {pathlib.Path("path/to/objects_list.txt")},
        ),
    ],
)
def test_get_object_list_files(
    tmp_path: pathlib.Path,
    source_file_manifest_content: t.List[str],
    expected_object_list_files: t.Set[pathlib.Path],
):
    """Test that the get_object_list_files function returns the expected object list files."""
    source_file_manifest = tmp_path.joinpath("source_file_manifest.txt")
    source_file_manifest.write_text("\n".join(source_file_manifest_content))

    object_list_files = output_generator.get_object_list_files(source_file_manifest)

    assert object_list_files == expected_object_list_files


@pytest.mark.parametrize(
    "object_files_content",
    [
        [[]],
        [["file1"]],
        [["file1", "file1"]],
        [["file1", "file2"]],
        [["file1", "file2"], ["file2"]],
        [["file1", "file2"], ["file3"]],
    ],
)
def test_get_object_files(
    tmp_path: pathlib.Path,
    object_files_content: t.List[t.List[str]],
):
    """Test that the get_object_files function returns the expected object files."""
    object_list_files = set()
    expected_object_files: t.Set[pathlib.Path] = set()
    for index, content in enumerate(object_files_content):
        object_list_file = tmp_path.joinpath(f"objects_list_{index}.txt")
        object_list_file.write_text("\n".join(content))
        object_list_files.add(object_list_file)
        expected_object_files.update(map(lambda file: pathlib.Path(file).resolve(), content))

    object_files = output_generator.get_object_files(object_list_files)

    assert object_files == expected_object_files


@pytest.mark.parametrize(
    "files",
    [
        [],
        ["file1.profraw"],
        ["file1.random", "file2"],
        ["file1.profraw", "file2.profraw"],
        ["file1.profraw", "file2.profraw", "file3.txt"],
        ["file1.profraw", "file2.profraw", "file3.txt", "file4.profraw"],
    ],
)
def test_get_profraw_files(tmp_path: pathlib.Path, files: t.List[str]):
    """Test that the get_profraw_files function returns the expected profraw files."""
    expected_files = {tmp_path.joinpath(file) for file in files if file.endswith(".profraw")}
    for file in files:
        tmp_path.joinpath(file).touch()

    profraw_files = output_generator.get_profraw_files(tmp_path)

    assert profraw_files == expected_files


@pytest.mark.parametrize(
    "matched_sources, ignored_sources, all_sources, expected_matched_sources",
    [
        (set(), set(), set(), set()),
        (set(), set(), {"file1", "file2"}, set()),
        ({"file1"}, set(), {"file1", "file2"}, {"file1"}),
        (set(), {"file1"}, {"file1", "file2"}, set()),
        ({"file1"}, {"file2"}, {"file1", "file2"}, {"file1"}),
        ({"file1"}, {"file1", "file2"}, {"file1", "file2"}, set()),
        ({"file1", "file2"}, set(), {"file1", "file2"}, {"file1", "file2"}),
    ],
)
def test_get_matched_sources(
    mocker: pytest_mock.MockerFixture,
    matched_sources: t.Set[str],
    ignored_sources: t.Set[str],
    all_sources: t.Set[str],
    expected_matched_sources: t.Set[str],
):
    """Test that the get_matched_sources method returns the expected excluded sources."""
    mocker.patch.object(output_generator, "resolve_regex_or_file")
    mocker.patch.object(
        output_generator,
        "filter_files",
        side_effect=[
            matched_sources,  # First filter_files usage
            ignored_sources,  # Second filter_files usage
        ],
    )

    matched_sources = output_generator.get_matched_sources(
        all_sources,
        "include_regex_or_file",
        "exclude_regex_or_file",
        pathlib.Path("exec"),
    )

    assert matched_sources == expected_matched_sources


@pytest.mark.parametrize(
    "no_profraw_files, no_object_files, no_source_files",
    [
        (False, False, False),
        (True, False, False),
        (False, True, False),
        (True, True, False),
        (False, False, True),
    ],
)
@pytest.mark.usefixtures("subprocess_mock")
def test_output_generator(
    tmp_path: pathlib.Path,
    mocker: pytest_mock.MockerFixture,
    no_profraw_files: bool,
    no_object_files: bool,
    no_source_files: bool,
):
    """Test the output_generator.main method."""
    execroot = path_finder.execroot(path_finder.llvm_bin_dir())

    # Create profraw files.
    profraw_file = tmp_path / "coverage.profraw"
    if not no_profraw_files:
        profraw_file.touch()

    # Create `objects_list.txt` file.
    # Objects must be relative to the execroot
    objects_list = tmp_path / "objects_list.txt"
    objects_list.touch()
    if not no_object_files:
        objects_list.write_text(str(execroot / "does_not_matter"))

    # Create `source_file_manifest.txt` file.
    source_file_manifest = tmp_path / "source_file_manifest.txt"
    source_file_manifest_content = [str(objects_list)]

    if not no_source_files:
        source_file_manifest_content.append("test.cpp")

    source_file_manifest.write_text("\n".join(source_file_manifest_content))

    # Define output location.
    output_file = tmp_path / "output.dat"

    # Mock `parse_args`.
    args = output_generator.Arguments(
        coverage_dir=tmp_path,
        output_file=output_file,
        source_file_manifest=source_file_manifest,
    )
    mocker.patch.object(output_generator, "parse_args", side_effect=[args])

    # As LLVM tools are not actually called, mock `target.profdata` as it is expected to be created.
    target_profdata_file = tmp_path / "target.profdata"
    target_profdata_file.touch()

    # Set if an early exit is expected.
    is_early_exit_expected = no_profraw_files or no_object_files or no_source_files

    # Run output generator main function.
    with pytest.raises(SystemExit) if is_early_exit_expected else nullcontext():
        output_generator.main()

    # Assert that `output_file` was created.
    if is_early_exit_expected:
        assert not output_file.exists()
    else:
        assert output_file.exists()
