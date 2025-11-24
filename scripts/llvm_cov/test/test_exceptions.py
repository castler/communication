# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for the exception module."""

import pytest

from quality_tools.llvm_cov import exceptions


@pytest.mark.parametrize(
    "explanation",
    ["", "Invalid regex pattern"],
)
def test_exception_invalid_llvm_regex_pattern(explanation: str):
    """Test exceptions.InvalidLlvmRegexPattern."""
    regex_pattern = ".*["
    position = 2

    expected_strings = [regex_pattern, " " * position + "^", explanation]

    try:
        raise exceptions.InvalidLlvmRegexPattern(
            regex_pattern=regex_pattern,
            position=position,
            explanation=explanation,
        )
    except exceptions.InvalidLlvmRegexPattern as exception:
        for string in expected_strings:
            assert string in str(exception.args[0])


def test_exception_multiple_llvm_bin_root_paths_found():
    """Test exceptions.MultipleLlvmBinRootPathsFound."""
    with pytest.raises(FileExistsError):
        raise exceptions.MultipleLlvmBinRootPathsFound


def test_exception_llvm_bin_root_path_not_found():
    """Test exceptions.LlvmBinRootPathNotFound."""
    with pytest.raises(FileNotFoundError):
        raise exceptions.LlvmBinRootPathNotFound
