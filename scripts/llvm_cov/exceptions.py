# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Collection of exceptions."""

import typing as t


class InvalidLlvmRegexPattern(Exception):
    """Exception raised for invalid regular expressions.

    Attributes:
        regex_pattern: The regular expression pattern.
        position: Optional. The exception position.
        explanation: Optional. The exception explanation.
    """

    def __init__(
        self,
        regex_pattern: str,
        position: t.Union[int, None] = None,
        explanation: str = "",
    ):
        message = f"\nInvalid LLVM regex detected: {regex_pattern}"
        if isinstance(position, int):
            message += (
                "\n"
                + str(" ") * (len(message) - len(regex_pattern))
                + "".join(["^" if x == position else " " for x in range(len(regex_pattern))])
            )
        if explanation:
            message += "\nExplanation: " + explanation
        super().__init__(message)


class MultipleLlvmBinRootPathsFound(FileExistsError):
    """This exception occurs when multiple paths are found for LLVM bin path."""


class LlvmBinRootPathNotFound(FileNotFoundError):
    """This exception occurs when a LLVM bin path was not found."""
