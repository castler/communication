# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

load("@swf_bazel_rules_quality//quality:defs.bzl", "py_pytest")

py_pytest(
    name = "test_logging",
    srcs = ["test_logging.py"],
    deps = ["//scripts/utils"],
)

py_pytest(
    name = "test_pathlib_utils",
    srcs = ["test_pathlib_utils.py"],
    deps = ["//scripts/utils:pathlib_utils"],
)
