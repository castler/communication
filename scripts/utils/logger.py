# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Default logging module to be used by quality tools.

This module sets up logging in a standardized way, such that log messages are generated
 in a known format and include the module name that set the logger up.
"""

import logging
import sys
import typing as t
from pathlib import Path


def get_logger(name: t.Optional[str] = None) -> logging.Logger:
    """Expose `logging.getLogger` to the user.

    Please refer to the `logging.getLogger` documentation for more details.
    """
    return logging.getLogger(name)


_logger = get_logger(name=__name__)

debug = _logger.debug
info = _logger.info
warning = _logger.warning
error = _logger.error
critical = _logger.critical


class QualityToolsStreamHandler(logging.StreamHandler):
    """Simple class wrapper to improve handler to stderr by default.

    This class is used to avoid modifying StreamHandler not configured by this module.
    """


class QualityToolsFileHandler(logging.FileHandler):
    """Simple class wrapper to improve handler to a file filtering.

    This class is used to avoid modifying FileHandler not configured by this module.
    """


def setup(
    verbose: bool = False,
    name: t.Optional[str] = None,
    log_file: t.Optional[Path] = None,
    logger: logging.Logger = _logger,
) -> None:
    """Set up logging according to the given verbosity level.

    verbose -- indicate if debug messages should be logged or not
    name -- set a name to be added to log messages. If None, the calling module name is used
    log_file -- absolute path to a log file. If None, no file logging is set up
    logger -- the logger to be configured. If None, defaults to the module logger
    """

    level = logging.DEBUG if verbose else logging.INFO

    if not name:
        # Looks one frame back for the module that invoked setup.
        # This is the same approach the built-in `logging` module uses to add source
        # file to log messages.
        caller_frame = sys._getframe(1)  # pylint: disable=protected-access
        name = Path(caller_frame.f_code.co_filename).stem

    msg_format = f"{name} | %(levelname)s: %(message)s"

    # Creating the following logger.handler certify that our loggingig in case of
    # consecutive setup calls will not create an unnecessary number of handlers.
    # The assertLogs context manager may use a handler that is not part of
    # this logging, so to ensure that the handler will not be altered within the
    # context manager we use the following code.
    logger.handlers = [
        handler
        for handler in logger.handlers
        if not isinstance(handler, (QualityToolsStreamHandler, QualityToolsFileHandler))
    ]
    logger.propagate = False
    logger.setLevel(level)

    stream_handler = QualityToolsStreamHandler()
    stream_handler.setFormatter(logging.Formatter(fmt=msg_format))
    stream_handler.setLevel(level)
    logger.addHandler(stream_handler)

    if log_file:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        file_handler = QualityToolsFileHandler(log_file, mode="w")
        file_handler.setFormatter(logging.Formatter(fmt=msg_format))
        file_handler.setLevel(level)
        logger.addHandler(file_handler)
