# Bazel Cache Strategy — Design Document

## 1. Overview

This document describes the caching strategy for Bazel builds in the CI/CD
pipelines of the `eclipse-score-communication` repository. It defines
requirements, design decisions, and the implementation architecture.

## 2. Goals

- **Faster PR feedback** — pull requests and merge-queue runs restore
  pre-populated caches so builds complete significantly faster.
- **Cache freshness** — caches are recreated nightly to prevent staleness and
  unbounded growth.
- **Correctness** — cache usage never masks build failures; caches are
  read-only for untrusted code (pull requests).
- **Simplicity** — a single, well-documented mechanism replaces the
  previously opaque third-party action.

## 3. Requirements

### 3.1 Cache Types

| Cache | Bazel Flag | Content |
|-------|-----------|---------|
| **Repository cache** | `--repository_cache` | Downloaded external repositories (tarballs, git repos). Shared across all jobs. |
| **Disk cache** | `--disk_cache` | Build action outputs (object files, test results). One per job type. |

### 3.2 Behavioral Requirements

| ID | Requirement |
|----|-------------|
| R1 | Jobs triggered by **pull request** or **merge queue** SHALL use both caches in **read-only** mode (restore only, no save). |
| R2 | Jobs triggered by **push to main** SHALL **update** the disk cache (restore existing → execute → save new → delete old entry). |
| R3 | Jobs triggered by **push to main** SHALL use the repository cache in **read-only** mode. |
| R4 | Jobs triggered by triggers **other than** pull request, merge queue, or push to main SHALL **not** use any caches (unless explicitly opted in via the `cache-mode` / `cache_mode` input). |
| R5 | The **repository cache** SHALL be **recreated nightly** — no old cache is restored, the job runs from scratch, and a fresh cache is saved. |
| R6 | The **disk cache** SHALL be **recreated nightly** — same semantics as R5, applied per job type. |
| R7 | After recreation, **old cache entries** SHALL be **deleted** from the GitHub Actions cache store. |
| R8 | Nightly recreation SHALL also be **triggerable manually** via `workflow_dispatch`. |
| R9 | During nightly recreation, jobs SHALL run **sequentially** to avoid cache key conflicts and excessive parallel resource consumption. |
| R10 | The repository cache SHALL be **shared** across all jobs (single cache key based on content hash of Bazel's `content_addressable/sha256/` directory). |
| R11 | Disk caches SHALL be **per-job** (unique key per workflow/configuration). |

### 3.3 Non-Requirements

- Remote build caches (BuildBuddy, etc.) are out of scope.
- Bazelisk binary caching is handled separately and not part of this design.

## 4. Architecture

### 4.1 Workflow Organization

Workflows are organized into two layers:

**Layer 1 — Composite actions** (job logic defined once):

| Action | Description |
|--------|-------------|
| `build-and-test-host` | Full build + test + examples (wraps `bazel-job`) |
| `thread-sanitizer` | Tests with `--config=tsan` (wraps `bazel-job`) |
| `address-sanitizer` | Tests with `--config=asan_ubsan_lsan` (wraps `bazel-job`) |
| `clang-tidy` | Static analysis with findings collection and artifact upload |
| `codeql` | CodeQL / MISRA analysis with SARIF upload (wraps `bazel-job`) |
| `coverage-report` | Coverage report with HTML archive and artifact upload |

**Standalone callable workflows** (too complex for composite actions):

| Workflow | Description |
|----------|-------------|
| `build_and_test_qnx.yml` | QNX cross-compilation with approval gate and secrets |

**Layer 2 — Orchestrators** (arrange execution pattern):

| Orchestrator | Calls | Execution |
|-------------|-------|-----------|
| `pr_quality_host.yml` | host, tsan, asan, clang-tidy (composite actions) | **Parallel** (PR/push/merge_group) |
| `nightly_cache_recreation.yml` | All 7 job types (composite actions + callable workflows) | **Sequential** (repo cache accumulation) |
| `nightly_quality.yml` | coverage, clang-tidy, codeql | **Parallel** (KPI reporting) |
| `automated_release.yml` | `pr_quality_host`, QNX, coverage | **Parallel** (via `pr_quality_host`) |

Nesting depth: `automated_release → pr_quality_host → composite action` = 2 levels (max 10).

### 4.2 Components

```
.github/
├── actions/
│   ├── bazel-cache-restore/   # Composite action: conditional cache restore
│   │   └── action.yml
│   ├── bazel-cache-save/      # Composite action: conditional cache save + cleanup
│   │   └── action.yml
│   ├── bazel-job/             # Composite action: full job setup
│   │   └── action.yml         #   (env, cache-mode, disk space, cache, sandbox, commands, save)
│   ├── build-and-test-host/   # Layer 1: wraps bazel-job with host build commands
│   │   └── action.yml
│   ├── thread-sanitizer/      # Layer 1: wraps bazel-job with --config=tsan
│   │   └── action.yml
│   ├── address-sanitizer/     # Layer 1: wraps bazel-job with --config=asan_ubsan_lsan
│   │   └── action.yml
│   ├── codeql/                 # Layer 1: CodeQL/MISRA with SARIF + CSV upload
│   │   └── action.yml
│   ├── coverage-report/        # Layer 1: coverage with HTML report + artifact
│   │   └── action.yml
│   └── clang-tidy/            # Layer 1: static analysis with findings collection
│       └── action.yml
├── workflows/
│   │  # Standalone callable workflows (workflow_call)
│   ├── build_and_test_qnx.yml
│   │  # Orchestrators
│   ├── pr_quality_host.yml          # PR quality gates (parallel)
│   ├── nightly_cache_recreation.yml # Nightly sequential cache rebuild
│   ├── nightly_quality.yml          # Nightly KPI reporting
│   ├── automated_release.yml        # Release process
│   │  # Standalone
│   ├── deploy_docs.yml
│   └── stale_pr.yml
└── cache-strategy.md
```

### 4.3 Cache Modes

Cache mode is controlled by a single `cache-mode` input (composite actions)
or `cache_mode` input (callable workflows). When empty (the default), the
mode is auto-computed from the GitHub event context:

```yaml
# In bazel-job (composite action) — computed automatically:
CACHE_MODE: >-
  ${{
    inputs.cache-mode != '' && inputs.cache-mode ||
    (github.event_name == 'pull_request' || github.event_name == 'merge_group') && 'read-only' ||
    (github.event_name == 'push' && github.ref == 'refs/heads/main') && 'update-disk' ||
    'disabled'
  }}
```

Orchestrators pass an explicit value (e.g., `cache-mode: "recreate"`) to
override the auto-computed mode during nightly recreation.

| Mode | Restore Disk | Restore Repo | Save Disk | Save Repo | Delete Old |
|------|:---:|:---:|:---:|:---:|:---:|
| `read-only` | ✅ | ✅ | ❌ | ❌ | ❌ |
| `update-disk` | ✅ | ✅ | ✅ | ❌ | old disk entries |
| `recreate` | ❌ | ❌ | ✅ | ✅ (content-hash key) | old disk + repo entries |
| `recreate-update` | ❌ | ✅ (from prev job) | ✅ | ✅ (content-hash key) | old disk + repo entries |
| `disabled` | ❌ | ❌ | ❌ | ❌ | ❌ |

### 4.4 Cache Keys

| Cache | Key Pattern | Restore Keys |
|-------|-------------|-------------|
| Repository (final) | `repo-cache-<content-hash>` | `repo-cache-` (prefix match) |
| Disk | `disk-cache-<job-name>-<run_id>` | `disk-cache-<job-name>-` |

**Repository cache key rationale**: The repository cache is saved under
a content-hash key. This hash is derived from Bazel's own repository cache
structure: the `content_addressable/sha256/` directory contains files whose
**names are already their SHA-256 content hashes**. By sorting and hashing
this list of filenames, we get an accurate fingerprint of the entire cache
in milliseconds — no need to re-read multi-GB file content.

This avoids false invalidation — if nothing changed in the external
repositories overnight, the hash stays the same and the save becomes a no-op
(key already exists).

During nightly recreation, each sequential job computes its own content hash
and saves. Since jobs add new external dependencies, the hash changes with
each job. Each job deletes all old repo cache entries except its own, so only
the latest (most complete) entry survives for the next job to restore.

**Disk cache key**: Uses `run_id` to ensure each save creates a unique entry.
The `restore-keys` prefix match restores the most recent available entry.

### 4.5 Composite Actions

#### `bazel-cache-restore`

1. Creates cache directories (`~/.cache/bazel/{repository_cache,disk_cache}`).
2. Conditionally calls `actions/cache/restore@v4` for repository and disk
   caches (only in `read-only` and `update-disk` modes).
3. Appends `--repository_cache` and `--disk_cache` flags to `~/.bazelrc`.

#### `bazel-cache-save`

1. Conditionally calls `actions/cache/save@v4` for disk cache
   (`update-disk`, `recreate`) and repository cache (`recreate` only).
2. Deletes old cache entries via the GitHub Actions cache API using `gh api`.

Both actions are called explicitly (not via `post` hooks) because composite
actions do not support automatic post-steps.

#### `bazel-job`

Wraps the full job lifecycle into a single composite action call:
1. Set environment variables (ANDROID_HOME, etc.)
2. Compute CACHE_MODE from `cache-mode` input or event context
3. Free disk space
4. Restore caches
5. Allow linux-sandbox
6. Run commands (newline-separated)
7. Save caches (`if: always()`)

Used by Layer 1 composite actions (build-and-test-host, thread-sanitizer,
address-sanitizer) and directly by orchestrators (codeql job in
pr_quality_host). Complex workflows (coverage_report, build_and_test_qnx)
compute CACHE_MODE themselves and call restore/save directly.

### 4.6 Nightly Recreation Flow

```
┌─────────────────────────────────────────────────┐
│         nightly_cache_recreation.yml            │
│  (schedule: 0 3 * * * | workflow_dispatch)      │
└─────────────────────────────────────────────────┘
          │
          ▼ mode: recreate (first job, starts empty)
  ┌───────────────┐
  │  host build   │──▶ saves disk-cache-build_and_test_host-*
  └───────────────┘    saves repo-cache-<hash1>, deletes old
          │
          ▼ mode: recreate-update (restores repo from prev job)
  ┌───────────────┐
  │    tsan       │──▶ saves disk-cache-build_and_test_tsan-*
  └───────────────┘    saves repo-cache-<hash2>, deletes old
          │
          ▼ mode: recreate-update
  ┌───────────────┐
  │  asan/ubsan   │──▶ saves disk-cache-build_and_test_asan_ubsan_lsan-*
  └───────────────┘    saves repo-cache-<hash3>, deletes old
          │
          ▼ mode: recreate-update
  ┌───────────────┐
  │  clang-tidy   │──▶ saves disk-cache-clang_tidy-*
  └───────────────┘    saves repo-cache-<hash4>, deletes old
          │
          ▼ mode: recreate-update
  ┌───────────────┐
  │   coverage    │──▶ saves disk-cache-coverage_report-*
  └───────────────┘    saves repo-cache-<hash5>, deletes old
          │
          ▼ mode: recreate-update
  ┌───────────────┐
  │    codeql     │──▶ saves disk-cache-codeql-*
  └───────────────┘    saves repo-cache-<hash6>, deletes old
          │
          ▼ mode: recreate-update
  ┌───────────────┐
  │      qnx     │──▶ saves disk-cache-build_and_test_qnx-*
  └───────────────┘    saves repo-cache-<hash7>, deletes old
```

### 4.7 Push-to-Main Flow

```
PR merged → push to main
    │
    ▼
┌────────────────────────────────────┐
│ pr_quality_host.yml (parallel)     │
│  mode: update-disk                 │
│  ┌──────────────┬───────┬───────┐  │
│  │ host build   │ tsan  │ asan  │  │
│  └──────────────┴───────┴───────┘  │
│  Each: restore → run → save disk   │
└────────────────────────────────────┘
    (same for QNX, triggered separately on push)
```

## 5. Permissions

The cache save + cleanup actions require `actions: write` permission to:
- Save new cache entries
- Delete old cache entries via `DELETE /repos/{owner}/{repo}/actions/caches/{id}`

All workflows that save or delete caches already declare this permission.

## 6. Interaction with Other Workflows

| Workflow | Behavior |
|----------|----------|
| `automated_release.yml` | Calls `pr_quality_host.yml` (parallel composite actions). Cache mode auto-computes to `disabled` (no PR/push trigger). |
| `nightly_quality.yml` | Uses `coverage-report`, `clang-tidy`, and `codeql` composite actions. Cache mode auto-computes to `disabled`. |
| `deploy_docs.yml` | Standalone, uses cache restore/save directly. |
| `stale_pr.yml` | Does not use Bazel — unaffected. |

## 7. Disk Cache Names

| Job | Disk Cache Name |
|-----|----------------|
| `build-and-test-host` (action) | `build_and_test_host` |
| `thread-sanitizer` (action) | `build_and_test_tsan` |
| `address-sanitizer` (action) | `build_and_test_asan_ubsan_lsan` |
| `clang-tidy` (action) | `clang_tidy` |
| `build_and_test_qnx.yml` (workflow) | `build_and_test_qnx` |
| `coverage-report` (action) | `coverage_report` |
| `codeql` (action) | `codeql` |
| `deploy_docs.yml` (workflow) | `build_docs` |

## 8. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Cache corruption causes build failures | Caches are recreated nightly; any corruption is automatically resolved within 24h. Manual recreation is available via `workflow_dispatch`. |
| Cache size exceeds GitHub's 10 GB limit | Old entries are deleted after each save. Nightly recreation purges all stale entries. |
| GitHub API rate limits during cache cleanup | Cleanup uses simple pagination; the number of old entries per prefix is typically 1-2. |
| Nightly recreation takes too long (sequential) | Sequential execution is intentional to avoid cache conflicts. Total time ≈ sum of individual job times; acceptable for off-hours runs. |
