# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

load("@rules_python//python:defs.bzl", "py_library")
load("@swf_bazel_rules_quality//quality:defs.bzl", "py_pytest")
load("@swf_quality_tools_pip_hub//:loaders.bzl", "pkg")

py_library(
    name = "conftest",
    testonly = True,
    srcs = ["conftest.py"],
    deps = [
        pkg("pytest"),
        pkg("pytest-mock"),
        "//quality_tools/llvm_cov:llvm",
        "//quality_tools/llvm_cov:meta",
    ],
)

py_pytest(
    name = "test_output_generator",
    srcs = ["test_output_generator.py"],
    data = ["//quality_tools/llvm_cov/test/data:test_data"],
    deps = [
        ":conftest",
        "//quality_tools/llvm_cov:exceptions",
        "//quality_tools/llvm_cov:llvm",
        "//quality_tools/llvm_cov:output_generator",
    ],
)

py_pytest(
    name = "test_report_generator",
    srcs = ["test_report_generator.py"],
    deps = [
        ":conftest",
        "//quality_tools/llvm_cov:meta",
        "//quality_tools/llvm_cov:report_generator",
    ],
)

py_pytest(
    name = "test_llvm",
    srcs = ["test_llvm.py"],
    deps = [
        ":conftest",
        "//quality_tools/llvm_cov:llvm",
    ],
)

py_pytest(
    name = "test_path_finder",
    srcs = ["test_path_finder.py"],
    deps = ["//quality_tools/llvm_cov:path_finder"],
)

py_pytest(
    name = "test_meta",
    srcs = ["test_meta.py"],
    deps = [
        ":conftest",
        "//quality_tools/llvm_cov:meta",
    ],
)

py_pytest(
    name = "test_exceptions",
    srcs = ["test_exceptions.py"],
    deps = ["//quality_tools/llvm_cov:exceptions"],
)
