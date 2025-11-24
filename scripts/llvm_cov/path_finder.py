# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Helper module that finds llvm bin path."""

import os
import pathlib
from typing import Set

from scripts.llvm_cov import exceptions


def execroot(llvm_bin_directory: pathlib.Path) -> pathlib.Path:
    """Heuristic to find the execution root based on the LLVM cov bin.

    Assuming that llvm_bin_dir is under output_base/external/path/to/llvm_bin_dir,
     its path is splitted until 'external' and then appended with 'execroot'
     because coexist in the same directory level.

    Finally, TEST_WORKSPACE is defined by Bazel and is the relative path from the
     execution root to the workspace, so it's appended to the execroot path.
    """
    return (
        pathlib.Path(str(llvm_bin_directory).split("external", maxsplit=1)[0])
        / "execroot"
        / os.environ["TEST_WORKSPACE"]
    )


def llvm_bin_dir() -> pathlib.Path:
    """Find and return a resolved llvm bin directory path.

    A hard path "**/bin/llvm-cov" is used in order to search under runfiles.

    Exceptions are thrown in case none or multiple paths are found.
    """
    llvm_cov_search_path = pathlib.Path.cwd() / "external"

    llvm_cov_dir_set: Set[pathlib.Path] = set()

    for llvm_cov in llvm_cov_search_path.glob("*/**/bin/llvm-cov"):
        llvm_cov_dir_set.add(llvm_cov.resolve(strict=True).parent)

    if len(llvm_cov_dir_set) > 1:
        raise exceptions.MultipleLlvmBinRootPathsFound

    if len(llvm_cov_dir_set) < 1:
        raise exceptions.LlvmBinRootPathNotFound

    return llvm_cov_dir_set.pop()
