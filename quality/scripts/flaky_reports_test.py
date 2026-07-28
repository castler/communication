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

import json
import os
import tempfile
import unittest

from quality.scripts import collect_flaky_tests, merge_flaky_reports


class CollectFlakyTest(unittest.TestCase):
    def test_collect_reports_flaky_and_failed_targets(self):
        with tempfile.TemporaryDirectory() as tmp:
            bep_path = os.path.join(tmp, "bep.json")
            raw_path = os.path.join(tmp, "raw.log")
            out_dir = os.path.join(tmp, "out")
            gh_out = os.path.join(tmp, "gh.out")

            with open(bep_path, "w", encoding="utf-8") as stream:
                stream.write(
                    json.dumps(
                        {
                            "id": {"testSummary": {"label": "//pkg:flaky_low"}},
                            "testSummary": {"overallStatus": "FLAKY", "failed": [{}] * 5, "passed": [{}] * 995},
                        }
                    )
                    + "\n"
                )
                stream.write(
                    json.dumps(
                        {
                            "id": {"testSummary": {"label": "//pkg:flaky_high"}},
                            "testSummary": {"overallStatus": "FLAKY", "failed": [{}] * 11, "passed": [{}] * 989},
                        }
                    )
                    + "\n"
                )
                stream.write(
                    json.dumps(
                        {
                            "id": {"testSummary": {"label": "//pkg:failed"}},
                            "testSummary": {"overallStatus": "FAILED", "failed": [{}], "passed": []},
                        }
                    )
                    + "\n"
                )
            with open(raw_path, "w", encoding="utf-8") as stream:
                stream.write("//pkg:passed PASSED in 0.2s\n")

            exit_code = collect_flaky_tests.main(
                [
                    "--config-name",
                    "gcc15",
                    "--bep-json",
                    bep_path,
                    "--raw-log",
                    raw_path,
                    "--output-dir",
                    out_dir,
                    "--test-exit-code",
                    "1",
                    "--runs-per-test",
                    "1000",
                    "--github-output",
                    gh_out,
                ]
            )
            self.assertEqual(exit_code, 0)

            with open(os.path.join(out_dir, "summary.json"), encoding="utf-8") as stream:
                summary = json.load(stream)
            self.assertEqual(summary["flaky_count"], 2)
            self.assertEqual(summary["failed_count"], 1)
            self.assertEqual(summary["passed_count"], 1)
            self.assertEqual(summary["flaky_tests"], ["//pkg:flaky_high", "//pkg:flaky_low"])
            self.assertEqual(summary["failed_tests"], ["//pkg:failed"])
            self.assertNotIn("accepted_flaky_tests", summary)
            self.assertNotIn("non_acceptable_flaky_count", summary)

            with open(gh_out, encoding="utf-8") as stream:
                output_text = stream.read()
            self.assertIn("flaky_count=2", output_text)
            self.assertIn("failed_count=1", output_text)
            self.assertNotIn("accepted_flaky_count", output_text)


class MergeFlakyTest(unittest.TestCase):
    def test_merge_aggregates_targets_across_configs(self):
        with tempfile.TemporaryDirectory() as tmp:
            reports_root = os.path.join(tmp, "reports")
            os.makedirs(os.path.join(reports_root, "a"), exist_ok=True)
            os.makedirs(os.path.join(reports_root, "b"), exist_ok=True)
            out_json = os.path.join(tmp, "merged", "summary.json")
            out_md = os.path.join(tmp, "merged", "summary.md")
            gh_out = os.path.join(tmp, "gh.out")

            with open(os.path.join(reports_root, "a", "summary.json"), "w", encoding="utf-8") as stream:
                json.dump(
                    {
                        "config_name": "tsan",
                        "flaky_count": 1,
                        "flaky_test_details": [
                            {"target": "//pkg:shared", "failed_runs": 7, "total_runs": 300, "failures_per_thousand": 23.3}
                        ],
                        "failed_count": 0,
                        "passed_count": 10,
                        "test_exit_code": 1,
                    },
                    stream,
                )
            with open(os.path.join(reports_root, "b", "summary.json"), "w", encoding="utf-8") as stream:
                json.dump(
                    {
                        "config_name": "asan",
                        "flaky_count": 1,
                        "flaky_test_details": [
                            {"target": "//pkg:shared", "failed_runs": 5, "total_runs": 300, "failures_per_thousand": 16.6}
                        ],
                        "failed_count": 2,
                        "passed_count": 5,
                        "test_exit_code": 2,
                    },
                    stream,
                )

            exit_code = merge_flaky_reports.main(
                [
                    "--reports-root",
                    reports_root,
                    "--output-json",
                    out_json,
                    "--output-md",
                    out_md,
                    "--github-output",
                    gh_out,
                ]
            )
            self.assertEqual(exit_code, 0)

            with open(out_json, encoding="utf-8") as stream:
                merged = json.load(stream)
            self.assertEqual(merged["total_flaky_count"], 1)
            self.assertEqual(merged["total_failed_count"], 2)
            self.assertEqual(len(merged["flaky_targets"]), 1)
            target = merged["flaky_targets"][0]
            self.assertEqual(target["target"], "//pkg:shared")
            self.assertEqual(target["failed_runs"], 12)
            self.assertEqual(target["total_runs"], 600)
            self.assertEqual(set(target["configs"]), {"tsan", "asan"})
            self.assertEqual(target["configs"]["tsan"]["failed_runs"], 7)

            with open(gh_out, encoding="utf-8") as stream:
                output_text = stream.read()
            self.assertIn("total_flaky_count=1", output_text)
            self.assertIn("total_failed_count=2", output_text)


if __name__ == "__main__":
    unittest.main()
