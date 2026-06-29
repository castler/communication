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

// main: just save inputs to state for the post step to use
function run() {
  const mode = core.getInput("cache-mode");
  const diskCacheName = core.getInput("disk-cache-name");
  core.saveState("cache-mode", mode);
  core.saveState("disk-cache-name", diskCacheName);
  core.info(`Post cache save registered (mode: ${mode}, disk-cache: ${diskCacheName || "(none)"})`);
}

run();
