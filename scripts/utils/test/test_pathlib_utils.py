# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Test the pathlib_utils module."""

import dataclasses
import pathlib
import typing as t

import pytest

from scripts.utils import pathlib_utils


@dataclasses.dataclass
class UseCase:
    """Hold paths for testing relativeness."""

    root: t.Union[pathlib.Path, str]
    relative: t.Union[pathlib.Path, str]
    relative_to: t.Optional[pathlib.Path] = None


relative_paths = [
    UseCase(pathlib.Path("a/b"), pathlib.Path("a/b/c"), pathlib.Path("c")),
    UseCase(pathlib.Path("../a"), pathlib.Path("../a/b"), pathlib.Path("b")),
    UseCase(pathlib.Path("a"), "a/b", pathlib.Path("b")),
    UseCase(pathlib.Path("/a/b/c"), "/../a/b/c", pathlib.Path(".")),
    UseCase("a/b/../c", pathlib.Path("a/c"), pathlib.Path(".")),
    UseCase(pathlib.Path("../a/b"), "../a/b/c", pathlib.Path("c")),
    UseCase("./a", pathlib.Path("a/b"), pathlib.Path("b")),
    UseCase(pathlib.Path("/./a"), "/a/b", pathlib.Path("b")),
    UseCase("a/./b", "a/b", pathlib.Path(".")),
]

non_relative_paths = [
    UseCase(pathlib.Path("a/b"), pathlib.Path("/a/b")),
    UseCase(pathlib.Path("../a"), pathlib.Path("/a/b")),
    UseCase(pathlib.Path("a"), "/a/b"),
    UseCase(pathlib.Path("/a/b/c"), "../a/b/c"),
    UseCase(pathlib.Path("a/b/c"), "../a/b/c"),
    UseCase(pathlib.Path("a/b/../c"), pathlib.Path("/a/c")),
    UseCase(pathlib.Path("/a/b"), pathlib.Path("../a/b")),
    UseCase(pathlib.Path("a/./b"), pathlib.Path("/a/b")),
    UseCase(pathlib.Path("/a/b/../c"), pathlib.Path("a/c")),
]


@pytest.mark.parametrize("use_case", [*relative_paths, *non_relative_paths])
def test_is_relative_to(use_case: UseCase):
    """Test whether a path is relative to another, including both valid and invalid cases."""
    expectation = bool(use_case.relative_to)

    assert pathlib_utils.is_relative_to(use_case.relative, use_case.root) == expectation


@pytest.mark.parametrize("use_case", [*relative_paths, *non_relative_paths])
def test_relative_to(use_case: UseCase):
    """Test the relative path from one path to another, including both valid and invalid cases."""
    if use_case.relative_to:
        assert pathlib_utils.relative_to(use_case.relative, use_case.root) == use_case.relative_to
    else:
        with pytest.raises(ValueError):
            pathlib_utils.relative_to(use_case.relative, use_case.root)


@pytest.mark.parametrize("use_case", [*relative_paths, *non_relative_paths])
def test_try_relative_to(use_case: UseCase):
    """Test the attempt to get a relative path from another without raising any error."""
    expected_relative = pathlib.Path(use_case.relative_to or use_case.relative)

    assert pathlib_utils.try_relative_to(use_case.relative, use_case.root) == expected_relative
