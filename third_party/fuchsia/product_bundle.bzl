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

"""Repository rule that downloads a prebuilt Fuchsia product bundle.

`ffx product download` cannot be used inside a repository rule (it ignores HTTP proxies and needs a
daemon), so the bundle is fetched with Bazel's downloader based on its `transfer.json` manifest.
The manifest is pinned by sha256. Its entries (blobs are content addressed) are not hashed
individually.
"""

_BATCH_SIZE = 64

def _fuchsia_product_bundle_impl(ctx):
    bucket_url = ctx.attr.bucket_url.rstrip("/")
    ctx.download(
        url = bucket_url + "/" + ctx.attr.build_path.strip("/") + "/transfer.json",
        output = "transfer.json",
        sha256 = ctx.attr.transfer_sha256,
    )
    manifest = json.decode(ctx.read("transfer.json"))

    downloads = []
    for group in manifest["entries"]:
        remote_prefix = group["remote"].strip("/")
        local_prefix = group["local"].strip("/")
        for entry in group["entries"]:
            name = entry["name"]
            downloads.append((
                bucket_url + "/" + remote_prefix + "/" + name,
                local_prefix + "/" + name,
            ))

    pending = []
    for url, output in downloads:
        pending.append(ctx.download(url = url, output = output, block = False))
        if len(pending) >= _BATCH_SIZE:
            for p in pending:
                p.wait()
            pending = []
    for p in pending:
        p.wait()

    ctx.file("BUILD.bazel", """package(default_visibility = ["//visibility:public"])

filegroup(
    name = "product_bundle",
    srcs = glob(["product_bundle/**"]),
)

exports_files(["product_bundle/product_bundle.json"])
""")

fuchsia_product_bundle_download = repository_rule(
    implementation = _fuchsia_product_bundle_impl,
    doc = "Downloads a prebuilt Fuchsia product bundle described by a transfer.json manifest.",
    attrs = {
        "bucket_url": attr.string(
            default = "https://storage.googleapis.com/fuchsia-public-artifacts-release",
            doc = "HTTPS base URL of the GCS bucket hosting the artifacts.",
        ),
        "build_path": attr.string(
            mandatory = True,
            doc = "Path of the product bundle below the bucket, e.g. builds/<id>/product_bundles/core.x64.",
        ),
        "transfer_sha256": attr.string(
            mandatory = True,
            doc = "sha256 of transfer.json.",
        ),
    },
)
