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
const exec = require("@actions/exec");
const cache = require("@actions/cache");
const path = require("path");
const os = require("os");
const fs = require("fs");

async function computeCacheMode() {
  const explicitMode = core.getInput("cache-mode");
  if (explicitMode) return explicitMode;

  const eventName = process.env.GITHUB_EVENT_NAME || "";
  const ref = process.env.GITHUB_REF || "";

  if (
    eventName === "pull_request" ||
    eventName === "pull_request_target" ||
    eventName === "merge_group"
  ) {
    return "read-only";
  }
  if (eventName === "push" && ref === "refs/heads/main") {
    return "update-disk";
  }
  return "disabled";
}

async function freeDiskSpace() {
  core.info("Freeing disk space (level 4)...");
  const commands = [
    "sudo rm -rf /usr/share/dotnet",
    "sudo rm -rf /usr/local/lib/android",
    "sudo rm -rf /opt/ghc",
    "sudo rm -rf /opt/hostedtoolcache",
    "sudo rm -rf /usr/local/share/boost",
    "sudo rm -rf /usr/share/swift",
    "sudo rm -rf /usr/local/graalvm",
    "sudo rm -rf /usr/local/share/powershell",
    "sudo rm -rf /usr/local/share/chromium",
    "sudo rm -rf /usr/local/lib/node_modules",
    "sudo rm -rf /opt/az",
  ];
  for (const cmd of commands) {
    await exec.exec("bash", ["-c", cmd], { ignoreReturnCode: true });
  }
  core.info("Disk space freed.");
}

async function restoreCaches(mode, diskCacheName) {
  const home = os.homedir();
  const repoCacheDir = path.join(home, ".cache", "bazel", "repository_cache");
  const diskCacheDir = path.join(home, ".cache", "bazel", "disk_cache");

  if (mode === "disabled") return;

  // Create cache directories
  fs.mkdirSync(repoCacheDir, { recursive: true });
  fs.mkdirSync(diskCacheDir, { recursive: true });

  // Restore repository cache
  if (mode === "read-only" || mode === "update-disk" || mode === "recreate-update") {
    core.info("Restoring repository cache...");
    const repoKey = await cache.restoreCache(
      [repoCacheDir],
      "repo-cache-impossible-exact-match",
      ["repo-cache-"]
    );
    if (repoKey) {
      core.info(`Repository cache restored from key: ${repoKey}`);
      core.saveState("repo-cache-restored-key", repoKey);
    } else {
      core.info("No repository cache found.");
    }
  }

  // Restore disk cache
  if (diskCacheName && (mode === "read-only" || mode === "update-disk")) {
    core.info(`Restoring disk cache '${diskCacheName}'...`);
    const runId = process.env.GITHUB_RUN_ID || "0";
    const diskKey = await cache.restoreCache(
      [diskCacheDir],
      `disk-cache-${diskCacheName}-${runId}`,
      [`disk-cache-${diskCacheName}-`]
    );
    if (diskKey) {
      core.info(`Disk cache restored from key: ${diskKey}`);
      core.saveState("disk-cache-restored-key", diskKey);
    } else {
      core.info("No disk cache found.");
    }
  }

  // Configure Bazel cache paths in ~/.bazelrc
  const bazelrc = path.join(home, ".bazelrc");
  const config = [
    `common --repository_cache=${repoCacheDir}`,
    `common --disk_cache=${diskCacheDir}`,
  ].join("\n") + "\n";
  fs.appendFileSync(bazelrc, config);
  core.info(`Bazel cache paths written to ${bazelrc}`);
}

async function enableSandbox() {
  core.info("Enabling user namespaces for Bazel sandbox...");
  await exec.exec("sudo", [
    "sysctl",
    "-w",
    "kernel.apparmor_restrict_unprivileged_userns=0",
  ], { ignoreReturnCode: true });
}

async function run() {
  try {
    // Set environment variables
    core.exportVariable("ANDROID_HOME", "");
    core.exportVariable("ANDROID_SDK_ROOT", "");
    core.exportVariable("FORCE_JAVASCRIPT_ACTIONS_TO_NODE24", "true");

    // Compute and export cache mode
    const mode = await computeCacheMode();
    core.exportVariable("CACHE_MODE", mode);
    core.info(`Cache mode: ${mode}`);

    // Save inputs to state for post step
    const diskCacheName = core.getInput("disk-cache");
    core.saveState("cache-mode", mode);
    core.saveState("disk-cache-name", diskCacheName);

    // Free disk space
    await freeDiskSpace();

    // Restore caches
    await restoreCaches(mode, diskCacheName);

    // Enable sandbox
    await enableSandbox();

    core.info("Bazel job setup complete.");
  } catch (error) {
    core.setFailed(`Pre step failed: ${error.message}`);
  }
}

run();
