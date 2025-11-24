#!/usr/bin/env python3

# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Integration test module.

Must be called from the workspace root.

The reason why this is a "local" test is that using bazel test and invoking bazel commands
inside those test is troublesome.
"""

import json
import logging
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

# To enable runs from bazel/tox and via plain python.
sys.path.append(str(Path(__file__).parents[4]))

from quality_tools.llvm_cov.support import commands  # pylint: disable=wrong-import-position

logging.basicConfig(level=logging.DEBUG)

# Check under quality_tools/metrics/templates/llvm_cov.j2.sh.
LLVM_COV_TEMPLATE_CONFIGS = [
    "--combined_report=lcov",
    "--coverage_output_generator=@swf_quality_tools//quality_tools/llvm_cov:merger",
    "--coverage_report_generator=@swf_quality_tools//quality_tools/llvm_cov:reporter",
    "--experimental_fetch_all_coverage_outputs",
    "--experimental_generate_llvm_lcov",
    "--experimental_use_llvm_covmap",
    "--test_env=COVERAGE_GCOV_PATH=/usr/bin/true",
    "--test_env=GENERATE_LLVM_LCOV=0",
    "--test_env=HTML_FLAT_VIEW=1",
    "--build_runfile_links",
    "--nocache_test_results",
]


class TestIntegration(unittest.TestCase):
    """Integration test running from outside bazel."""

    def test_runs(self):
        """Runs the entire pipeline in sandbox and local mode."""
        strategies = ["sandboxed", "local"]

        with tempfile.TemporaryDirectory() as temp_dir:
            for strategy in strategies:
                result = commands.execute_command(
                    [
                        "bazel",
                        "--output_base",
                        temp_dir,
                        "coverage",
                        "--spawn_strategy",
                        strategy,
                        *LLVM_COV_TEMPLATE_CONFIGS,
                        "--",
                        "//quality_tools/llvm_cov/examples/...",
                    ],
                    cwd=Path.cwd(),
                )

                std_out = result.stdout

                target_reports, report_location = self.find_results(std_out)
                self.assertEqual(len(target_reports), 2)
                self.assertTrue(report_location)

                meta_jsons, _, _ = self.read_zip(target_reports, is_output_generator=True)

                self.assert_target_meta(meta_jsons)

                meta_jsons, text_reports, html_reports = self.read_zip([report_location], is_output_generator=False)

                self.assert_overall_meta(meta_jsons)
                self.assert_overall_reports(text_reports, html_reports)

                commands.execute_command(["bazel", "--output_base", temp_dir, "clean", "--async"])

    def assert_target_reports(self, text_reports, html_reports):
        """Assert existence and content of reports per target."""
        self.assertEqual(len(text_reports), 2)
        self.assertEqual(len(html_reports), 2)

        for text_report in text_reports:
            self.assertTrue(text_report)

        sources_report1 = [
            "quality_tools/llvm_cov/examples/package/integration_test.cpp",
            "quality_tools/llvm_cov/examples/package/source.cpp",
        ]
        sources_report2 = [
            "quality_tools/llvm_cov/examples/header.h",
            "quality_tools/llvm_cov/examples/package/source.cpp",
            "quality_tools/llvm_cov/examples/test.cpp",
        ]
        self.assert_expected_sources_in_report(text_reports[0], sources_report1)
        self.assert_expected_sources_in_html(html_reports[0], sources_report1)

        self.assert_expected_sources_in_report(text_reports[1], sources_report2)
        self.assert_expected_sources_in_html(html_reports[1], sources_report2)

    def assert_target_meta(self, meta_jsons):
        """Assert existence and content of meta info per target."""
        self.assertEqual(len(meta_jsons), 2)

        for meta_json in meta_jsons:
            self.assertIn("llvm_bin_dir", meta_json)
            self.assertIn("execroot", meta_json)
            self.assertIn("profdata", meta_json)
            self.assertIn("object_files", meta_json)
            self.assertIn("matched_sources", meta_json)
            self.assertIn("user_config", meta_json)

        self.assertEqual(len(meta_jsons[0]["object_files"]), 1)
        self.assertEqual(len(meta_jsons[1]["object_files"]), 1)

        self.assertIn(
            "quality_tools/llvm_cov/examples/package/integration_test",
            meta_jsons[0]["object_files"][0],
        )
        self.assertIn("quality_tools/llvm_cov/examples/test", meta_jsons[1]["object_files"][0])

    def assert_overall_reports(self, text_reports, html_reports):
        """Assert existence and content of reports overall."""
        self.assertEqual(len(text_reports), 1)

        sources_report = [
            "quality_tools/llvm_cov/examples/package/integration_test.cpp",
            "quality_tools/llvm_cov/examples/package/source.cpp",
            "quality_tools/llvm_cov/examples/header.h",
            "quality_tools/llvm_cov/examples/package/source.cpp",
            "quality_tools/llvm_cov/examples/test.cpp",
        ]

        self.assert_expected_sources_in_report(text_reports[0], sources_report)
        self.assert_expected_sources_in_html(html_reports[0], sources_report)

    def assert_overall_meta(self, meta_jsons):
        """Assert (non)existence and content of meta info overall."""
        self.assertEqual(len(meta_jsons), 0)

    def assert_expected_sources_in_report(self, text_report, expected_sources):
        """Assert expected sources in text report."""
        for expected_source in expected_sources:
            self.assertIn(
                f"/{expected_source}:",
                text_report,
            )

    def assert_expected_sources_in_html(self, html_report, expected_sources):
        """Assert expected sources in html report."""
        for expected_source in expected_sources:
            self.assertIn(
                f"/{expected_source}.html",
                html_report,
            )

    def read_zip(self, target_reports, is_output_generator):
        """Helper to read the output zip file."""
        meta_jsons = []
        text_reports = []
        html_reports = []
        for target_report in sorted(target_reports):
            with zipfile.ZipFile(target_report, "r") as archive:
                try:
                    meta_json = json.loads(archive.read(("meta.json")))
                    meta_jsons.append(meta_json)
                except KeyError:
                    pass

                if is_output_generator:
                    # The output generator only creates meta information.
                    # Reports are create by the reporter generator only.
                    continue

                text_report = archive.read(str(Path("text_report") / "report.txt"))
                text_reports.append(text_report.decode("utf-8"))

                html_report = archive.read(str(Path("html_report") / "index.html"))
                html_reports.append(html_report.decode("utf-8"))
        return meta_jsons, text_reports, html_reports

    def find_results(self, std_out):
        """Find the location of the output zips by parsing the stdout."""
        next_line_target_report = False
        target_reports = []
        report_location = ""

        for line in std_out.split("\n"):
            anchor = "INFO: LCOV coverage report is located at"
            if line.startswith(anchor):
                report_location = line.replace(anchor, "").strip()

            if next_line_target_report:
                target_reports.append(line.strip())
                next_line_target_report = False

            if "PASSED in" in line:
                next_line_target_report = True
        return target_reports, report_location


if __name__ == "__main__":
    unittest.main()
