// *******************************************************************************
// Copyright (c) 2026 Contributors to the Eclipse Foundation
//
// See the NOTICE file(s) distributed with this work for additional
// information regarding copyright ownership.
//
// This program and the accompanying materials are made available under the
// terms of the Apache License Version 2.0 which is available at
// https://www.apache.org/licenses/LICENSE-2.0
//
// SPDX-License-Identifier: Apache-2.0
// *******************************************************************************

const core = require("@actions/core");

function run() {
  const mode = process.env.CACHE_MODE || "unknown";
  const diskCache = core.getInput("disk-cache") || "(none)";
  core.info(`Bazel job configured — cache-mode: ${mode}, disk-cache: ${diskCache}`);
  core.info("Subsequent steps can now run bazel commands.");
}

run();
