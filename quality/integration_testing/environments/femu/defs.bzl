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

"""Helper rules for the Fuchsia emulator (FEMU) integration tests."""

def far_files_impl(ctx):
    fars = [f for f in ctx.attr.package[DefaultInfo].files.to_list() if f.extension == "far" and f.basename != "meta.far"]
    if len(fars) != 1:
        fail("Expected exactly one .far file in %s, got %s" % (ctx.attr.package.label, fars))
    return [DefaultInfo(files = depset(fars))]

far_files = rule(
    implementation = far_files_impl,
    attrs = {"package": attr.label(mandatory = True)},
)
