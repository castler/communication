# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_test(
    name = "test",
    srcs = ["test.cpp"],
    deps = [
        "//scripts/llvm_cov/examples:lib",
        "//scripts/llvm_cov/examples/package:source",
    ],
)

cc_library(
    name = "lib",
    srcs = ["lib.cpp"],
    hdrs = ["header.h"],
)
