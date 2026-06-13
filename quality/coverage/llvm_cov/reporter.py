#!/usr/bin/env python3
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
"""Final coverage report generator using llvm-cov.

This script is invoked by Bazel as the --coverage_report_generator after all tests
complete. It reads the per-test zip files produced by the merger, merges all profdata
into one, and generates the final combined HTML report.

Expected Bazel interface:
    --reports_file=<path>    Text file listing paths to all per-test coverage outputs
    --output_file=<path>     Where to write the final report (zip)
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Dict, List, Set, Tuple


def main() -> None:
    """Main entry point."""
    args = parse_args()

    # Read the list of per-test report files.
    reports = read_reports_file(args.reports_file)
    if not reports:
        print("INFO: No coverage reports found.", file=sys.stderr)
        write_empty_output(args.output_file)
        sys.exit(0)

    # Extract profdata and metadata from each per-test zip.
    valid_profdata_files, valid_object_files, meta = extract_reports(reports)

    if not valid_profdata_files or not valid_object_files:
        print("INFO: No valid profdata or object files found.", file=sys.stderr)
        write_empty_output(args.output_file)
        sys.exit(0)

    # Get llvm tools path from meta.
    llvm_bin_path = Path(meta["llvm_bin_path"])
    if not (llvm_bin_path / "llvm-cov").exists():
        # Try finding it ourselves.
        llvm_bin_path = find_llvm_bin()

    # Merge all profdata files.
    merged_profdata = Path.cwd() / "merged_coverage.profdata"
    run_command([
        str(llvm_bin_path / "llvm-profdata"), "merge",
        "--sparse",
        "--output", str(merged_profdata),
    ] + sorted(valid_profdata_files))

    # Build coverage arguments.
    coverage_args = ["--instr-profile", str(merged_profdata)]
    for obj in sorted(valid_object_files):
        coverage_args.extend(["--object", obj])

    # Get filter regexes and workspace root from meta.
    filter_regexes = set(meta.get("excluded_sources", []))
    # Use the workspace_root from meta — it was resolved from sandbox symlinks
    # to point to the actual workspace where source files exist on disk.
    workspace_root = meta.get("workspace_root", str(Path.cwd()) + "/")

    common_show_args = {
        "llvm_bin_path": llvm_bin_path,
        "coverage_args": coverage_args,
        "filter_regexes": sorted(filter_regexes),
        "workspace_root": workspace_root,
    }

    # Generate HTML report.
    html_report_dir = Path.cwd() / "html_report"
    run_llvm_cov_show(
        **common_show_args,
        output_format="html",
        html_report_dir=html_report_dir,
    )

    # Generate LCOV report (for backward compatibility with dashboards).
    lcov_report_dir = Path.cwd() / "lcov_report"
    lcov_report_dir.mkdir(exist_ok=True)
    lcov_result = run_llvm_cov_export(
        llvm_bin_path=llvm_bin_path,
        coverage_args=coverage_args,
        filter_regexes=sorted(filter_regexes),
        workspace_root=workspace_root,
    )
    with open(lcov_report_dir / "lcov.dat", "w", encoding="utf-8") as f:
        f.write(lcov_result.stdout)

    # Generate text summary.
    text_report_dir = Path.cwd() / "text_report"
    text_report_dir.mkdir(exist_ok=True)
    summary = run_llvm_cov_report(
        llvm_bin_path=llvm_bin_path,
        coverage_args=coverage_args,
        filter_regexes=sorted(filter_regexes),
    )
    with open(text_report_dir / "summary.txt", "w", encoding="utf-8") as f:
        f.write(summary.stdout)
    print(summary.stdout, file=sys.stderr)

    # Run coverage justification processing if YAML exists.
    justification_report_dir = Path.cwd() / "justification_report"
    justification_yaml = find_justification_yaml(workspace_root)
    if justification_yaml:
        justification_report_dir.mkdir(exist_ok=True)
        run_justification_processing(
            justification_yaml=justification_yaml,
            source_root=workspace_root,
            html_report_dir=html_report_dir,
            output_dir=justification_report_dir,
        )

    # Package everything into the output zip.
    directories = [html_report_dir, lcov_report_dir, text_report_dir]
    if justification_report_dir.exists():
        directories.append(justification_report_dir)
    create_zip(
        root=Path.cwd(),
        directories=directories,
        output_file=args.output_file,
    )

    print(f"INFO: Coverage reporter completed. Output: {args.output_file}", file=sys.stderr)


def run_llvm_cov_show(
    llvm_bin_path: Path,
    coverage_args: List[str],
    filter_regexes: List[str],
    workspace_root: str,
    output_format: str,
    html_report_dir: Path = None,
) -> subprocess.CompletedProcess:
    """Run llvm-cov show."""
    cmd = [
        str(llvm_bin_path / "llvm-cov"),
        "show",
        f"--format={output_format}",
        f"--path-equivalence=/proc/self/cwd/,{workspace_root}",
        f"--compilation-dir={workspace_root}",
        "--show-branches=count",
        "--show-region-summary=0",
    ]

    cxxfilt = llvm_bin_path / "llvm-cxxfilt"
    if cxxfilt.exists():
        cmd.append(f"--Xdemangler={cxxfilt}")

    for regex in filter_regexes:
        adjusted = regex.replace("/proc/self/cwd/", workspace_root)
        cmd.append(f"--ignore-filename-regex={adjusted}")

    if html_report_dir:
        cmd.append(f"--output-dir={html_report_dir}")
        cmd.append("--coverage-watermark=100,50")
        cmd.append("--show-expansions")

    cmd.extend(coverage_args)
    return run_command(cmd)


def run_llvm_cov_export(
    llvm_bin_path: Path,
    coverage_args: List[str],
    filter_regexes: List[str],
    workspace_root: str,
) -> subprocess.CompletedProcess:
    """Run llvm-cov export to produce LCOV format."""
    cmd = [
        str(llvm_bin_path / "llvm-cov"),
        "export",
        "--format=lcov",
        f"--path-equivalence=/proc/self/cwd/,{workspace_root}",
        f"--compilation-dir={workspace_root}",
    ]

    for regex in filter_regexes:
        adjusted = regex.replace("/proc/self/cwd/", workspace_root)
        cmd.append(f"--ignore-filename-regex={adjusted}")

    cmd.extend(coverage_args)
    return run_command(cmd)


def run_llvm_cov_report(
    llvm_bin_path: Path,
    coverage_args: List[str],
    filter_regexes: List[str],
) -> subprocess.CompletedProcess:
    """Run llvm-cov report for a summary."""
    cmd = [
        str(llvm_bin_path / "llvm-cov"),
        "report",
        "--summary-only",
        "--show-region-summary=0",
        "--show-branch-summary=1",
    ]

    for regex in filter_regexes:
        cmd.append(f"--ignore-filename-regex={regex}")

    cmd.extend(coverage_args)
    return run_command(cmd)


def extract_reports(reports: List[str]) -> Tuple[Set[str], Set[str], Dict]:
    """Extract profdata and metadata from per-test zip files."""
    valid_profdata_files = set()
    valid_object_files = set()
    overall_meta: Dict = {
        "excluded_sources": set(),
        "llvm_bin_path": "",
        "execroot": "",
        "workspace_root": "",
    }

    for i, report_path in enumerate(reports):
        # Skip baseline_coverage files (LCOV format, not our zip).
        if "baseline_coverage" in report_path:
            continue

        report = Path(report_path)
        if not report.exists() or report.stat().st_size == 0:
            continue

        # Check if it's a valid zip.
        if not zipfile.is_zipfile(report):
            continue

        profdata_name = f"coverage_report_{i:08d}.profdata"

        try:
            with zipfile.ZipFile(report, "r") as archive:
                # Extract meta.
                meta_json = archive.read("meta/meta.json")
                target_meta = json.loads(meta_json)

                # Extract profdata.
                profdata_content = archive.read("profdata/target.profdata")
                profdata_path = Path.cwd() / profdata_name
                with open(profdata_path, "wb") as f:
                    f.write(profdata_content)

                valid_profdata_files.add(str(profdata_path))

                # Collect object files.
                for obj in target_meta.get("object_files", []):
                    if obj and Path(obj).exists():
                        valid_object_files.add(os.path.realpath(obj))

                # Merge meta.
                overall_meta["excluded_sources"] |= set(target_meta.get("excluded_sources", []))
                overall_meta["llvm_bin_path"] = target_meta.get("llvm_bin_path", "")
                overall_meta["execroot"] = target_meta.get("execroot", "")
                if target_meta.get("workspace_root"):
                    overall_meta["workspace_root"] = target_meta["workspace_root"]

        except (zipfile.BadZipFile, KeyError, json.JSONDecodeError) as e:
            print(f"WARNING: Skipping invalid report {report_path}: {e}", file=sys.stderr)
            continue

    # Convert set to list for JSON serialization later.
    overall_meta["excluded_sources"] = list(overall_meta["excluded_sources"])

    return valid_profdata_files, valid_object_files, overall_meta


def find_llvm_bin() -> Path:
    """Find the llvm bin directory."""
    # Search in external/ for llvm-cov.
    for search_root in [Path.cwd() / "external", Path.cwd()]:
        if search_root.exists():
            for llvm_cov in search_root.glob("*/**/bin/llvm-cov"):
                return llvm_cov.resolve().parent

    print("ERROR: Could not find llvm-cov binary.", file=sys.stderr)
    sys.exit(1)


def read_reports_file(reports_file: Path) -> List[str]:
    """Read the reports file listing all per-test coverage outputs."""
    with open(reports_file, encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip()]


def write_empty_output(output_file: Path) -> None:
    """Write an empty file as output when there's nothing to report."""
    with open(output_file, "w", encoding="utf-8") as f:
        f.write("")


def run_command(cmd: List[str]) -> subprocess.CompletedProcess:
    """Run a command and exit on failure."""
    try:
        return subprocess.run(
            cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except subprocess.CalledProcessError as e:
        print(f"ERROR: Command failed with code {e.returncode}:", file=sys.stderr)
        print(f"  {' '.join(cmd)}", file=sys.stderr)
        if e.stdout:
            print(e.stdout, file=sys.stderr)
        sys.exit(1)


def create_zip(root: Path, directories: List[Path], output_file: Path) -> None:
    """Create a zip file from the given directories relative to root."""
    with zipfile.ZipFile(output_file, "w", zipfile.ZIP_DEFLATED) as zf:
        for directory in directories:
            if not directory.exists():
                continue
            for dirpath, _, files in os.walk(directory):
                for filename in files:
                    file_path = Path(dirpath) / filename
                    arcname = file_path.relative_to(root)
                    zf.write(file_path, arcname)


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments matching the Bazel coverage_report_generator interface."""
    parser = argparse.ArgumentParser(description="LLVM coverage reporter for Bazel")
    parser.add_argument("--output_file", type=Path, required=True)
    parser.add_argument("--reports_file", type=Path, required=True)
    return parser.parse_args()


def find_justification_yaml(workspace_root: str) -> Path | None:
    """Find the coverage_justifications.yaml file in the workspace."""
    candidates = [
        Path(workspace_root) / "quality" / "coverage" / "coverage_justifications.yaml",
        Path(workspace_root) / "coverage_justifications.yaml",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def run_justification_processing(
    justification_yaml: Path,
    source_root: str,
    html_report_dir: Path,
    output_dir: Path,
) -> None:
    """Run the justification processing pipeline.

    Calls justify.py and effective_coverage.py as subprocesses. These scripts
    are standalone and only require Python stdlib + PyYAML (for justify.py).
    """
    script_dir = Path(__file__).resolve().parent

    manifest_path = output_dir / "manifest.json"
    report_path = output_dir / "report.json"

    # Run justify.py
    justify_script = script_dir / "justify.py"
    if not justify_script.exists():
        # Try runfiles location
        justify_script = _find_in_runfiles("quality/coverage/llvm_cov/justify.py")
    if not justify_script or not justify_script.exists():
        print("WARNING: justify.py not found, skipping justification processing.",
              file=sys.stderr)
        return

    justify_cmd = [
        sys.executable,
        str(justify_script),
        "--yaml", str(justification_yaml),
        "--source-root", source_root.rstrip("/"),
        "--output", str(manifest_path),
    ]

    try:
        result = subprocess.run(
            justify_cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
    except subprocess.CalledProcessError as e:
        print(f"WARNING: Justification processing failed: {e.stderr}", file=sys.stderr)
        return

    if not manifest_path.exists():
        return

    # Run effective_coverage.py
    effective_script = script_dir / "effective_coverage.py"
    if not effective_script.exists():
        effective_script = _find_in_runfiles(
            "quality/coverage/llvm_cov/effective_coverage.py"
        )
    if not effective_script or not effective_script.exists():
        print("WARNING: effective_coverage.py not found.", file=sys.stderr)
        return

    effective_cmd = [
        sys.executable,
        str(effective_script),
        "--html-dir", str(html_report_dir),
        "--manifest", str(manifest_path),
        "--output", str(report_path),
    ]

    try:
        result = subprocess.run(
            effective_cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
    except subprocess.CalledProcessError as e:
        print(
            f"WARNING: Effective coverage calculation failed: {e.stderr}",
            file=sys.stderr,
        )


def _find_in_runfiles(rel_path: str) -> Path | None:
    """Find a file in Bazel runfiles."""
    # Try RUNFILES_DIR env var
    runfiles_dir = os.environ.get("RUNFILES_DIR", "")
    if runfiles_dir:
        # Try common workspace names
        for ws in ["_main", "score", ""]:
            candidate = Path(runfiles_dir) / ws / rel_path if ws else Path(runfiles_dir) / rel_path
            if candidate.exists():
                return candidate

    # Try relative to this script's location
    script_dir = Path(__file__).resolve().parent
    candidate = script_dir / Path(rel_path).name
    if candidate.exists():
        return candidate

    return None


if __name__ == "__main__":
    main()
