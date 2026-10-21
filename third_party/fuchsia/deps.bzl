# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
"""Dev-only module extension instantiating the Fuchsia helper repositories."""

load("@fuchsia_infra//cipd:defs.bzl", "cipd_tool_repository")
load("@fuchsia_infra//infra:host_toolchains.bzl", "fuchsia_clang_host_toolchain_repo")
load("@rules_fuchsia//fuchsia/workspace:register_fuchsia_sdk_toolchain.bzl", "fuchsia_toolchain_decl")
load(":product_bundle.bzl", "fuchsia_product_bundle_download")

def _fuchsia_deps_impl(_module_ctx):
    cipd_tool_repository(name = "cipd_tool")

    fuchsia_toolchain_decl(
        name = "fuchsia_sdk_toolchain",
        toolchain_path = "@fuchsia_sdk//:fuchsia_toolchain_info",
    )

    fuchsia_clang_host_toolchain_repo(name = "host_clang_toolchain")

    # Prebuilt core.x64 product bundle (same version as the SDK) used to boot the emulator.
    fuchsia_product_bundle_download(
        name = "core_x64_product_bundle",
        build_path = "builds/8670199271704615345/product_bundles/core.x64",
        transfer_sha256 = "e4c353a308cf5a2cf70536bce2f82717169b1f9e12c8ab4b764ed27ab7001c3d",
    )

fuchsia_deps = module_extension(implementation = _fuchsia_deps_impl)
