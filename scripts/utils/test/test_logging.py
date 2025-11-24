# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for default logging module."""

import logging as _logging
from pathlib import Path
from typing import Callable

import pytest

from scripts.utils import logger as logging


@pytest.mark.parametrize(
    "verbose",
    [False, True],
)
def test_logging_setup_verbose(verbose):
    """Test logging module setup method with verbose option."""
    level = _logging.DEBUG if verbose else _logging.INFO

    logging.setup(verbose=verbose)

    assert logging._logger.isEnabledFor(level)  # pylint: disable=protected-access


@pytest.mark.parametrize(
    "logging_method",
    [logging.debug, logging.info, logging.warning, logging.error, logging.critical],
)
def test_logging_setup_messages(capsys, logging_method: Callable):
    """Test logging module setup with logged messages."""

    expected_name = "test_logging"
    expected_log_level = logging_method.__name__.upper()
    log_message = "Some log message"

    expected_message = f"{expected_name} | {expected_log_level}: {log_message}\n"
    expected_empty = ""

    # With verbose set to true all log level messages are directed to stderr.
    logging.setup(verbose=True)

    logging_method(log_message)

    capture = capsys.readouterr()
    assert expected_empty == capture.out
    assert expected_message == capture.err


@pytest.mark.parametrize(
    "name",
    ["small_checks", None],
)
def test_logging_setup_name(capsys, name):
    """Test logging module setup method with verbose option."""

    log_message = "test_logging"

    expected_name = "test_logging" if name is None else name

    logging.setup(verbose=True, name=name)

    logging.debug(log_message)

    capture = capsys.readouterr()
    assert expected_name in capture.err


@pytest.mark.parametrize(
    "logging_method",
    [logging.debug, logging.info, logging.warning, logging.error, logging.critical],
)
def test_logging_setup_log_file(capsys, logging_method: Callable, tmp_path: Path):
    """Test logging module setup with log file."""

    test_log_file = tmp_path / "test_file.txt"

    log_message = "Testing FileHandler log message"
    expected_log_level = logging_method.__name__.upper()
    expected_message = f"test_logging | {expected_log_level}: {log_message}\n"
    expected_empty = ""

    logging.setup(verbose=True, log_file=test_log_file)

    logging_method(log_message)

    capture = capsys.readouterr()
    assert expected_empty == capture.out
    assert expected_message == capture.err

    with open(test_log_file, 'r', encoding="UTf-8") as test_file:
        log_contents = test_file.read()

    assert expected_message in log_contents


def test_multiple_loggers(capsys, tmp_path: Path):
    """Test that multiple loggers can be set up without interference."""

    logging_msg = "Message from logging"
    logger1_msg = "Message from logger1"
    logger2_msg = "Message from logger2"

    expected_logging_msg = f"test_logging | INFO: {logging_msg}\n"
    expected_logger1_msg = f"logger1 | INFO: {logger1_msg}\n"
    expected_logger2_msg = f"logger2 | INFO: {logger2_msg}\n"
    logger2_file = tmp_path / "logger2.log"

    logger1 = logging.get_logger("logger1")
    logger2 = logging.get_logger("logger2")

    logging.setup(verbose=True)
    logging.setup(verbose=True, name=logger1.name, logger=logger1)
    logging.setup(verbose=True, name=logger2.name, logger=logger2, log_file=logger2_file)

    logging.info(logging_msg)
    logger1.info(logger1_msg)
    logger2.info(logger2_msg)

    capture = capsys.readouterr()
    assert expected_logging_msg in capture.err
    assert expected_logger1_msg in capture.err
    assert expected_logger2_msg in capture.err
    assert capture.out == ""

    logger2_file_content = logger2_file.read_text(encoding="utf-8")
    assert expected_logging_msg not in logger2_file_content
    assert expected_logger1_msg not in logger2_file_content
    assert expected_logger2_msg == logger2_file_content
