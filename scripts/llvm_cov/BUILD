# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

load("@rules_python//python:defs.bzl", "py_binary", "py_library")

package(default_visibility = ["//:__subpackages__"])

py_binary(
    name = "output_generator",
    srcs = ["output_generator.py"],
    tags = ["standalone"],
    deps = [
        ":exceptions",
        ":llvm",
        ":meta",
        ":path_finder",
        "//scripts/utils",
        "//scripts/utils:pathlib_utils",
    ],
)

# Alias to keep backwards compatibility
alias(
    name = "merger",
    actual = ":output_generator",
)

py_binary(
    name = "report_generator",
    srcs = ["report_generator.py"],
    deps = [
        ":exceptions",
        ":llvm",
        ":meta",
        ":path_finder",
        "//scripts/utils",
    ],
)

# Alias to keep backwards compatibility
alias(
    name = "reporter",
    actual = ":report_generator",
)

py_library(
    name = "exceptions",
    srcs = ["exceptions.py"],
)

py_library(
    name = "meta",
    srcs = ["meta.py"],
)

py_library(
    name = "llvm",
    srcs = ["llvm.py"],
    deps = [
        "//scripts/utils",
        "//scripts/utils:pathlib_utils",
    ],
)

label_flag(
    name = "llvm-cov-provider",
    build_setting_default = "@llvm_toolchain//:llvm-cov",
)

py_library(
    name = "path_finder",
    srcs = ["path_finder.py"],
    data = [":llvm-cov-provider"],
    deps = [":exceptions"],
)
