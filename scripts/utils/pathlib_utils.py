# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Module for pathlib utils."""

import os
import pathlib
import typing as t


def is_relative_to(relative: t.Union[pathlib.Path, str], root: t.Union[pathlib.Path, str]):
    """Helper function that mimics what pathlib.Path.is_relative_to does.

    This solves pathlib limitation about never eliding `../`, see https://github.com/python/cpython/issues/99334.

    As pathlib python 3.8 does not have this method, this also improves support across different python versions.
    """
    try:
        relative_to(relative, root)
        return True
    except ValueError:
        return False


def relative_to(relative: t.Union[pathlib.Path, str], root: t.Union[pathlib.Path, str]):
    """Helper function that mimics what pathlib.Path.relative_to does.

    This solves pathlib limitation about never eliding `../`, see https://github.com/python/cpython/issues/99334.
    """
    return pathlib.Path(os.path.normpath(relative)).relative_to(os.path.normpath(root))


def try_relative_to(relative: t.Union[pathlib.Path, str], root: t.Union[pathlib.Path, str]) -> pathlib.Path:
    """Try to return a relative path from a root, else, return the original path.

    This always convert the input to a `pathlib.Path` object, so it can be used in a consistent way.
    """
    try:
        return relative_to(relative, root)
    except ValueError:
        return pathlib.Path(relative)
