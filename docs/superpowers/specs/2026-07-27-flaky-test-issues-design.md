# Design: Flaky-Test GitHub Issues

Date: 2026-07-27
Status: Approved (pending spec review)

## Problem

The nightly flaky-test detection pipeline currently emits its results as an
ephemeral GitHub Actions *step summary* plus uploaded artifacts (the closest
thing to a "dashboard"). This output is not persistent, not actionable, and
does not track how often a given test has been flaky over time.

We want, instead, **persistent, self-updating GitHub issues**: one issue per
flaky Bazel test target. When the same target is found flaky again on a later
nightly, the issue's "failed runs observed" counter accumulates and a history
comment is appended.

## Goals

- One GitHub issue per flaky test *target*, aggregating all configs.
- Cumulative counter = **total failed runs observed** across all nightlies
  (e.g. a night where the test failed 7 of 300 runs adds +7).
- Repeat findings **reopen** a closed issue; issues are **never auto-closed**
  (humans close them manually).
- Each recurrence updates the issue body counters **and** appends a comment
  (full history/audit trail).
- Robust against human edits to the issue body.

## Non-Goals / Removed Scope

- The notion of "acceptable" vs "non-acceptable" flaky tests is **removed
  entirely**. Any flaky test gets an issue. The
  `acceptable_failures_per_thousand` workflow input, the
  `--acceptable-failures-per-thousand` script args, and the accepted /
  non-acceptable split in the reports are all removed.
- The "Fail nightly when non-acceptable flaky tests are detected" gate is
  **removed**. The nightly no longer fails based on flakiness; it only records
  issues.
- No auto-close of issues after N clean nightlies.
- No dashboard / web UI.

## Approach

A new Python sync script (`quality/scripts/sync_flaky_issues.py`) with a
**pure-logic core** and an **injectable GitHub client**, so the decision logic
is fully unit-testable without network access. This mirrors the existing
`collect_flaky_tests.py` / `merge_flaky_reports.py` + `py_test` conventions.

## Data-Model Changes

### `collect_flaky_tests.py` (per config)

- Remove the acceptable/non-acceptable split, the
  `--acceptable-failures-per-thousand` arg, and the `accepted_flaky_tests` /
  `non_acceptable_flaky_tests` / `*_count` fields.
- Each flaky item keeps: `target`, `failed_runs`, `total_runs`,
  `failures_per_thousand` (kept as informational only).
- GitHub step outputs reduce to `flaky_count` and `failed_count`.

### `merge_flaky_reports.py` (aggregate)

Produce a **per-target aggregation across configs** instead of the
accepted/non-acceptable split:

```json
{
  "flaky_targets": [
    {
      "target": "//score/foo:bar_test",
      "failed_runs": 12,
      "total_runs": 600,
      "configs": {
        "tsan":            { "failed_runs": 7, "total_runs": 300 },
        "asan-ubsan-lsan": { "failed_runs": 5, "total_runs": 300 }
      }
    }
  ],
  "config_summaries": [ ... ]
}
```

`failed_runs` / `total_runs` at the top level are summed across configs for the
run. The consolidated markdown / step summary is simplified to a single "Flaky
targets" table (no acceptance columns).

### Workflow inputs

- Remove `acceptable_failures_per_thousand` from `nightly_flaky_detection.yml`.
- Remove `acceptable-failures-per-thousand` plumbing from
  `_nightly_flaky_detection_runner.yml` and its `prepare_bazel_environment`
  usage.

## Issue Identity & Body Format

### Identity / dedup

Each target maps to exactly one issue. Lookup is by label `flaky-test` plus a
hidden body marker placed just inside the managed region:

```
<!-- flaky-test-target: //score/foo:bar_test -->
```

The sync searches **open and closed** issues carrying that label + marker to
decide create / update / reopen. As a fallback it also matches the exact target
string. If a human strips **both** the label and the marker, dedup cannot find
the issue and a new one may be created — this is the single unavoidable edge
case and is documented behavior.

### Body layout

The managed markers wrap the **entire generated stats section** — the
human-readable counters, the per-config table, **and** the machine-readable JSON.
`merge_body` regenerates everything between the markers each run, so the visible
counters never go stale. Human prose/triage notes live **outside** the markers
(before `flaky-stats:begin` or after `flaky-stats:end`) and are preserved. The
target marker sits just inside the begin marker for identity lookup.

```markdown
<!-- flaky-stats:begin -->
<!-- flaky-test-target: //score/foo:bar_test -->
## Flaky test: `//score/foo:bar_test`

**Cumulative failed runs observed:** 42 (over 1800 total runs, 12 nightlies)
**First seen:** 2026-07-20 · **Last seen:** 2026-07-27

### Per-config cumulative
| Config          | Failed | Total |
|-----------------|-------:|------:|
| tsan            |     30 |   900 |
| asan-ubsan-lsan |     12 |   900 |

~~~json
{ "target": "//score/foo:bar_test",
  "cumulative_failed_runs": 42, "cumulative_total_runs": 1800,
  "nightly_count": 12, "first_seen": "2026-07-20", "last_seen": "2026-07-27",
  "configs": { "tsan": {"failed_runs":30,"total_runs":900},
               "asan-ubsan-lsan": {"failed_runs":12,"total_runs":900} } }
~~~
<!-- flaky-stats:end -->

<!-- Everything below this managed region is free for human triage notes. -->
```

## Robustness Against Human Edits

**Comments are the durable ledger; the body JSON block is a regenerable cache.**

- Each nightly recurrence posts a comment carrying its own machine-readable
  per-run record with a marker:
  `<!-- flaky-run: {run_id, date, configs, failed_runs, total_runs} -->`.
  Comments are append-only; humans essentially never edit them.
- Cumulative totals are **recomputed every run by scanning all bot comments**
  (identified by the marker), not by trusting the body. If a human corrupts,
  edits, or deletes the body JSON block, the next run regenerates it correctly
  from the comment history.
- Body prose **outside** the managed region is preserved; the sync only ever
  replaces the region between `flaky-stats:begin` / `end`, which contains **all**
  generated visible content (counters, per-config table, JSON). Because the
  whole stats section is regenerated, the visible counters never go stale. If
  the markers are missing, a fresh managed region is appended at the bottom.
- The issue **title is set once at creation and never overwritten** — humans may
  rename freely.
- **Overlap is prevented by a workflow `concurrency` group** (see Workflow
  Integration): scheduled and manually-dispatched nightlies cannot run the sync
  simultaneously, so there are no write races. Even so, the `run_id` idempotency
  check makes a re-run safe against double-counting.

## Sync Script Architecture (`quality/scripts/sync_flaky_issues.py`)

### GitHub client interface (abstract)

- `search_issue(target) -> Issue | None` (label + marker; fallback target string)
- `list_run_comments(issue) -> list[str]` (bodies of bot comments)
- `create_issue(title, body, labels) -> Issue`
- `update_issue_body(issue, body)`
- `reopen_issue(issue)`
- `add_comment(issue, body)`
- `ensure_label(name)` (create `flaky-test` label if missing)

Implementations: `RestGitHubClient` (real, uses `GITHUB_TOKEN` via the REST
API) and `FakeGitHubClient` (in-memory, for tests).

### Pure functions (no I/O — the tested core)

- `parse_run_records(comment_bodies) -> list[RunRecord]` — extract
  `<!-- flaky-run: … -->` markers; ignore non-bot / malformed comments.
- `aggregate(records) -> Stats` — cumulative failed/total, per-config sums,
  `nightly_count`, first/last seen.
- `render_body(target, stats) -> str` — renders the full managed region
  (counters + per-config table + JSON, wrapped in the begin/end markers).
- `render_run_comment(run_record) -> str`
- `merge_body(existing_body, new_region) -> str` — replace the entire managed
  region (between `flaky-stats:begin` / `end`), preserve all prose outside it;
  append the region if the markers are absent.

### Orchestration `sync(merged_summary, client, run_context)`

For each flaky target:

For each flaky target, `search_issue` first, then branch:

**Create branch** (no existing issue):
1. `create_issue` with title, initial body, label + marker.
2. `add_comment(render_run_comment(this_run))` — seeds the ledger.
3. Recompute `aggregate([this_run])` → `render_body` → `update_issue_body`.

**Update branch** (existing issue found):
1. `list_run_comments`; if this `run_id` is already recorded, **skip entirely**
   (idempotency — safe re-runs, no double counting; no comment, no body change).
2. `add_comment(render_run_comment(this_run))`.
3. Recompute `aggregate(all records incl. this run)` → `render_body` →
   `merge_body` → `update_issue_body`.
4. If the issue was closed → `reopen_issue` and note the reopen in the comment.

Exactly one run comment is written per target per run in both branches. The
create branch does not fall through to the update branch.

`list_run_comments` and issue search MUST page through all results (the REST
client follows pagination) so cumulative recomputation and dedup never miss
older comments/issues.

### Entry point

`py_binary` `sync_flaky_issues` with args: `--merged-summary`, `--repo`,
`--run-id`, `--run-url`, `--dry-run`. Token read from `GITHUB_TOKEN` env.
`--dry-run` prints intended actions without calling the API.

## Workflow Integration

### `nightly_flaky_detection.yml` (`aggregate-flaky-results` job)

- Add a workflow-level `concurrency` group so scheduled and manually-dispatched
  runs cannot overlap (serialize the sync):

```yaml
concurrency:
  group: nightly-flaky-detection
  cancel-in-progress: false
```

- Remove the `acceptable_failures_per_thousand` input and the "Fail nightly when
  non-acceptable" step.
- Job permissions: add `issues: write` (keep `contents: read`, `actions: read`).
- After the existing "Build consolidated report" (merge) step, add:

```yaml
- name: Sync flaky test issues
  if: always()
  env:
    GITHUB_TOKEN: ${{ github.token }}
  run: |
    if [[ ! -f /tmp/nightly_flaky/merged/summary.json ]]; then
      echo "No merged summary found; skipping flaky issue sync."
      exit 0
    fi
    python3 quality/scripts/sync_flaky_issues.py \
      --merged-summary /tmp/nightly_flaky/merged/summary.json \
      --repo "${{ github.repository }}" \
      --run-id "${{ github.run_id }}" \
      --run-url "${{ github.server_url }}/${{ github.repository }}/actions/runs/${{ github.run_id }}"
```

The merge and sync scripts are stdlib-only and run with `python3` directly
(no Bazel), so the aggregate job needs only `actions/checkout` — it does not
call `prepare_bazel_environment`.

- Keep the step-summary + artifact upload (run output, not a dashboard); the
  markdown just loses the acceptance columns.

### `_nightly_flaky_detection_runner.yml`

- Remove the `acceptable-failures-per-thousand` plumbing.

### Label bootstrap

The sync step calls `ensure_label("flaky-test")` so no manual setup is required.

## Testing

Extend the existing `py_test` (`flaky_reports_test.py`) coverage.

Unit tests for the pure core (no network, `FakeGitHubClient`):

- `parse_run_records`: extracts records; ignores non-bot / malformed comments.
- `aggregate`: correct cumulative failed/total, per-config sums, nightly_count,
  first/last seen.
- `merge_body`: replaces the whole managed region; preserves prose before/after;
  re-appends the region when markers are missing/corrupted; regenerated visible
  counters reflect the new totals (not stale).
- `sync` orchestration:
  - New target → `create_issue` once with label + marker, and **exactly one**
    run comment is written (no duplicate first-run comment).
  - Recurrence → adds comment, updates body counter, no duplicate issue.
  - Idempotency → same run_id twice does not double-count.
  - Closed issue reappears → `reopen_issue` + reopen note in comment.
  - Corrupted body block → cumulative still correct (recomputed from comments).

`collect` / `merge` tests updated for removed acceptable fields and the new
per-target aggregation. All tests hermetic; the real REST client is not
exercised.

## Rollout

1. Land data-model changes (collect/merge) + updated tests.
2. Add `sync_flaky_issues.py` + `FakeGitHubClient` tests + `py_binary`/`py_test`.
3. Wire the workflow step and permissions; remove the gate and inputs.
4. First live nightly seeds issues for currently-flaky targets.
