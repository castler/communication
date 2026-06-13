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
"""Per-test coverage output generator using llvm-cov.

This script is invoked by Bazel as the --coverage_output_generator for each test.
It receives profraw files from test execution, merges them into profdata, generates
an HTML coverage report using llvm-cov show, and packages everything into a zip file
that the reporter can later aggregate.

Expected Bazel interface (from collect_coverage.sh):
    --coverage_dir=<path>             Directory containing *.profraw files
    --output_file=<path>              Where to write the output (zip)
    --source_file_manifest=<path>     File listing instrumented sources and object files
    --filter_sources=<regex>          Source path regexes to exclude (repeatable)
    [--sources_to_replace_file=<path>] Optional source mapping file
"""

import argparse
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import List, Set, Tuple


def main() -> None:
    """Main entry point."""
    args = parse_args()

    # Get object files from the manifest.
    object_files = get_object_files_from_manifest(args.source_file_manifest)
    if not object_files:
        print("INFO: No instrumented object files found, skipping coverage.", file=sys.stderr)
        cleanup_dangling_symlinks(args.coverage_dir)
        sys.exit(0)

    # Find profraw files.
    profraw_files = sorted(args.coverage_dir.glob("*.profraw"))
    if not profraw_files:
        print("INFO: No *.profraw files found, skipping coverage.", file=sys.stderr)
        cleanup_dangling_symlinks(args.coverage_dir)
        sys.exit(0)

    # Find llvm tools.
    llvm_bin_path = find_llvm_bin()

    # Determine execution root and workspace.
    execroot = Path(str(llvm_bin_path).split("external", maxsplit=1)[0]) / "execroot" / os.environ.get("TEST_WORKSPACE", "_main")
    workspace_root = get_workspace_root(execroot, args.source_file_manifest)

    # Merge profraw → profdata.
    profdata_dir = args.coverage_dir / "profdata"
    profdata_dir.mkdir(exist_ok=True)
    profdata_file = profdata_dir / "target.profdata"

    run_command([
        str(llvm_bin_path / "llvm-profdata"), "merge",
        "--sparse",
        "--output", str(profdata_file),
    ] + [str(f) for f in profraw_files])

    # Build coverage arguments.
    coverage_args = ["--instr-profile", str(profdata_file)]
    for obj in sorted(object_files):
        coverage_args.extend(["--object", obj])

    # Build filter regexes for --ignore-filename-regex.
    filter_regexes = build_filter_regexes(args.source_file_manifest, workspace_root)

    # Generate HTML report.
    html_report_dir = args.coverage_dir / "html_report"
    run_llvm_cov_show(
        llvm_bin_path=llvm_bin_path,
        coverage_args=coverage_args,
        filter_regexes=filter_regexes,
        workspace_root=workspace_root,
        output_format="html",
        html_report_dir=html_report_dir,
    )

    # Create meta.json.
    meta_dir = args.coverage_dir / "meta"
    meta_dir.mkdir(exist_ok=True)
    meta = {
        "object_files": [os.path.realpath(f) for f in sorted(object_files)],
        "excluded_sources": sorted(filter_regexes),
        "llvm_bin_path": str(llvm_bin_path),
        "execroot": str(execroot),
        "workspace_root": workspace_root,
    }
    with open(meta_dir / "meta.json", "w", encoding="utf-8") as f:
        json.dump(meta, f)

    # Package into zip at output_file.
    create_zip(
        root=args.coverage_dir,
        directories=[profdata_dir, html_report_dir, meta_dir],
        output_file=args.output_file,
    )

    # Clean up dangling symlinks in coverage_dir that would cause Bazel tree
    # artifact validation to fail (e.g. the 'gcov' symlink created by
    # collect_cc_coverage.sh's init_gcov() pointing into the destroyed sandbox).
    cleanup_dangling_symlinks(args.coverage_dir)

    target = os.environ.get("TEST_TARGET", "unknown")
    print(f"INFO: Coverage merger completed for '{target}'", file=sys.stderr)


def cleanup_dangling_symlinks(directory: Path) -> None:
    """Remove symlinks in the coverage directory that would become dangling.

    Bazel's tree artifact validation rejects directories containing dangling
    symlinks. The 'gcov' symlink created by collect_cc_coverage.sh's init_gcov()
    points into the sandbox which is torn down before validation runs. Since we
    use llvm-cov directly, this symlink is not needed.
    """
    gcov_link = directory / "gcov"
    if gcov_link.is_symlink():
        gcov_link.unlink()

    # Also remove any other symlinks pointing into sandbox paths.
    for entry in directory.iterdir():
        if entry.is_symlink():
            target = os.readlink(entry)
            if "sandbox" in target:
                entry.unlink()


def run_llvm_cov_show(
    llvm_bin_path: Path,
    coverage_args: List[str],
    filter_regexes: List[str],
    workspace_root: str,
    output_format: str,
    html_report_dir: Path = None,
) -> subprocess.CompletedProcess:
    """Run llvm-cov show with the given arguments."""
    cmd = [
        str(llvm_bin_path / "llvm-cov"),
        "show",
        f"--format={output_format}",
        f"--path-equivalence=/proc/self/cwd/,{workspace_root}",
        f"--compilation-dir={workspace_root}",
        "--show-branches=count",
        "--show-region-summary=0",
    ]

    # Add demangler if available.
    cxxfilt = llvm_bin_path / "llvm-cxxfilt"
    if cxxfilt.exists():
        cmd.append(f"--Xdemangler={cxxfilt}")

    # Add filter regexes.
    for regex in sorted(filter_regexes):
        adjusted = regex.replace("/proc/self/cwd/", workspace_root)
        cmd.append(f"--ignore-filename-regex={adjusted}")

    if html_report_dir:
        cmd.append(f"--output-dir={html_report_dir}")
        cmd.append("--coverage-watermark=100,50")

    cmd.extend(coverage_args)
    return run_command(cmd)


def build_filter_regexes(source_file_manifest: Path, workspace_root: str) -> List[str]:
    """Build filter regexes to exclude non-project files from coverage.

    Excludes:
    - Mock files (equivalent of old lcov --remove '*mock*.h' '*mock*.cpp')
    - External dependencies (googletest, score_baselibs, etc.)
    - Test source files (*_test.cpp, *_test.h)
    """
    regexes = []

    # Exclude mock files.
    regexes.append(".*mock.*\\.(h|hpp|cpp)$")

    # Exclude external dependencies (anything under external/).
    regexes.append(".*/external/.*")

    # Exclude test files.
    regexes.append(".*_test\\.(cpp|h|hpp)$")
    regexes.append(".*/test/.*")

    return regexes


def get_workspace_root(execroot: Path, source_file_manifest: Path) -> str:
    """Determine the real workspace root for source file access.

    The goal is to find a path where source files exist and will persist
    after sandbox teardown, so the reporter can generate per-file HTML pages.

    Strategy:
    1. Find the main execroot by resolving a known file from the manifest.
    2. At the main execroot, follow the 'score' symlink to find the real workspace.
    3. If that fails, use the main execroot (sources accessible via symlinks there).
    """
    root = os.environ.get("ROOT", "")
    if not root:
        root = str(Path.cwd())

    root_path = Path(root)

    # Step 1: Find the main (non-sandbox) execroot by resolving a manifest entry.
    main_execroot = _find_main_execroot(root_path, source_file_manifest)
    if not main_execroot:
        main_execroot = root_path

    # Step 2: At the main execroot, check if source dirs are symlinks
    # pointing to the real workspace.
    for probe in ["score", "src", "lib"]:
        probe_path = main_execroot / probe
        if probe_path.is_symlink():
            real_path = str(probe_path.resolve())
            if real_path.endswith(probe):
                workspace = real_path[: -len(probe)]
                if not workspace.endswith("/"):
                    workspace += "/"
                return workspace

    # Fallback: use main execroot.
    result = str(main_execroot)
    if not result.endswith("/"):
        result += "/"
    return result


def _find_main_execroot(root_path: Path, source_file_manifest: Path) -> Path:
    """Find the main (non-sandbox) execroot by resolving manifest entries.

    Inside a Bazel sandbox, build output files (.so) are symlinks pointing to
    the main execroot's bazel-out/. By resolving one, we can extract the
    main execroot path.
    """
    try:
        with open(source_file_manifest, encoding="utf-8") as f:
            manifests = [line.strip() for line in f.readlines()]
    except (OSError, IOError):
        return None

    for manifest_line in manifests:
        if "objects_list.txt" in manifest_line:
            continue
        # Manifest lines are relative paths like "bazel-out/k8-fastbuild/bin/..."
        if not manifest_line.startswith("bazel-out/"):
            continue
        candidate = root_path / manifest_line
        if candidate.exists():
            real_path = str(candidate.resolve())
            # Extract execroot: look for "execroot/<workspace>/" in the path.
            # real_path is like /mnt/data/bazel/<hash>/execroot/_main/bazel-out/...
            idx = real_path.find("/execroot/")
            if idx >= 0:
                # Find end of workspace name after "execroot/"
                rest = real_path[idx + len("/execroot/"):]
                ws_end = rest.find("/")
                if ws_end >= 0:
                    execroot_str = real_path[: idx + len("/execroot/") + ws_end]
                    execroot = Path(execroot_str)
                    if execroot.exists() and "sandbox" not in str(execroot):
                        return execroot
        break  # Only need to check one file.

    return None


def get_object_files_from_manifest(source_file_manifest: Path) -> Set[str]:
    """Parse the coverage manifest to find instrumented object files."""
    runfiles_dir = Path(os.environ.get("RUNFILES_DIR", "")) / os.environ.get("TEST_WORKSPACE", "_main")
    exec_root = Path(os.environ.get("ROOT", "."))

    object_files = set()
    with open(source_file_manifest, encoding="utf-8") as f:
        manifests = [line.strip() for line in f.readlines()]

    for manifest in manifests:
        if "objects_list.txt" in manifest:
            with open(manifest, encoding="utf-8") as f:
                for line in f:
                    obj_path = line.strip()
                    if not obj_path:
                        continue
                    # Try runfiles first, then exec_root.
                    candidate = runfiles_dir / obj_path
                    if candidate.exists():
                        object_files.add(str(candidate))
                    else:
                        object_files.add(str(exec_root / obj_path))

    return object_files


def find_llvm_bin() -> Path:
    """Find the llvm bin directory from the environment or runfiles."""
    # Check environment variable first.
    llvm_bin_root = os.environ.get("LLVM_BIN_ROOT")
    if llvm_bin_root:
        path = Path(llvm_bin_root)
        if path.is_absolute() and path.exists():
            return path
        # Relative to exec root.
        root = Path(os.environ.get("ROOT", "."))
        candidate = root / llvm_bin_root
        if candidate.exists():
            return candidate

    # Search in external/ for llvm-cov.
    search_root = Path.cwd() / "external"
    if search_root.exists():
        for llvm_cov in search_root.glob("*/**/bin/llvm-cov"):
            return llvm_cov.resolve().parent

    # Search from ROOT.
    root = Path(os.environ.get("ROOT", "."))
    search_root = root / "external"
    if search_root.exists():
        for llvm_cov in search_root.glob("*/**/bin/llvm-cov"):
            return llvm_cov.resolve().parent

    print("ERROR: Could not find llvm-cov binary.", file=sys.stderr)
    sys.exit(1)


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
    """Parse command-line arguments matching the Bazel LCOV_MERGER interface."""
    parser = argparse.ArgumentParser(description="LLVM coverage merger for Bazel")
    parser.add_argument("--coverage_dir", type=Path, required=True)
    parser.add_argument("--output_file", type=Path, required=True)
    parser.add_argument("--source_file_manifest", type=Path, required=True)
    parser.add_argument("--filter_sources", action="append", default=[])
    parser.add_argument("--sources_to_replace_file", type=str, default=None)
    return parser.parse_args()


if __name__ == "__main__":
    main()
