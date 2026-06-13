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
"""Effective coverage calculator and HTML post-processor.

Takes the llvm-cov HTML report and the resolved justification manifest.
Modifies the HTML to show justified lines in a distinct color (yellow/orange)
and calculates effective coverage metrics.

Usage:
    python effective_coverage.py --html-dir <path> --manifest <manifest.json> --output <report.json>
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple


# Pattern to match a table row in llvm-cov HTML source pages
# Format: <tr><td class='line-number'>...</td><td class='uncovered-line'>...</td><td class='code'>...</td></tr>
LINE_NUMBER_RE = re.compile(r"<a name='L(\d+)'")
UNCOVERED_LINE_TD_RE = re.compile(r"<td class='uncovered-line'>")
COVERED_LINE_TD_RE = re.compile(r"<td class='covered-line'>")


def main() -> None:
    """Main entry point."""
    args = parse_args()

    # Load the justification manifest
    manifest = load_manifest(args.manifest)
    justified_files = manifest.get("justified_files", {})

    # Find all source HTML files in the report
    html_dir = args.html_dir
    if not html_dir.exists():
        print(f"ERROR: HTML report directory not found: {html_dir}", file=sys.stderr)
        sys.exit(1)

    # Parse raw coverage totals from the index page (matches llvm-cov exactly).
    raw_covered, raw_total = parse_index_page_totals(html_dir)

    # Process each source HTML file (restyle justified lines + count them)
    total_justified = 0
    total_stale = 0
    applied_justifications: List[Dict[str, Any]] = []
    stale_justifications: List[Dict[str, Any]] = []

    source_html_files = find_source_html_files(html_dir)
    for html_file in source_html_files:
        rel_source_path = extract_source_path_from_html(html_file, html_dir)
        if not rel_source_path:
            continue

        file_justifications = find_matching_justifications(
            rel_source_path, justified_files
        )

        file_stats = process_html_file(
            html_file, file_justifications, applied_justifications, stale_justifications
        )

        total_justified += file_stats["justified"]
        total_stale += file_stats["stale"]

    # Calculate stats using llvm-cov's exact numbers
    raw_uncovered = raw_total - raw_covered
    unjustified_uncovered = raw_uncovered - total_justified

    stats = {
        "total_instrumented_lines": raw_total,
        "covered_lines": raw_covered,
        "justified_lines": total_justified,
        "unjustified_uncovered_lines": max(0, unjustified_uncovered),
        "stale_justifications": total_stale,
        "raw_line_coverage_pct": round(100.0 * raw_covered / raw_total, 2) if raw_total > 0 else 0.0,
        "effective_line_coverage_pct": round(
            100.0 * (raw_covered + total_justified) / raw_total, 2
        ) if raw_total > 0 else 0.0,
    }

    # Inject CSS for justified lines into style.css
    inject_justified_css(html_dir)

    # Update the index page with effective coverage info
    update_index_page(html_dir, stats)

    # Write output report
    report = {
        "version": 1,
        "summary": stats,
        "applied_justifications": applied_justifications,
        "stale_justifications": stale_justifications,
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    # Write human-readable summary
    summary_path = output_path.parent / "summary.txt"
    write_summary(summary_path, stats, stale_justifications)

    # Print summary
    print(
        f"INFO: Effective coverage: {stats['effective_line_coverage_pct']}% "
        f"(raw: {stats['raw_line_coverage_pct']}%, "
        f"justified: {stats['justified_lines']} lines, "
        f"unjustified uncovered: {stats['unjustified_uncovered_lines']} lines)",
        file=sys.stderr,
    )
    if stale_justifications:
        print(
            f"WARNING: {len(stale_justifications)} stale justifications "
            f"(lines are actually covered, justification can be removed)",
            file=sys.stderr,
        )


def process_html_file(
    html_file: Path,
    justifications: Dict[int, Dict[str, str]],
    applied_justifications: List[Dict[str, Any]],
    stale_justifications: List[Dict[str, Any]],
) -> Dict[str, int]:
    """Process a single source HTML file. Modifies it in-place.

    Restyles justified lines: changes the count cell to show "J" with justified-line
    class, and changes red code regions to justified (orange) background.
    Only counts justified/stale lines for the justification report — raw coverage
    numbers are taken from the index page to match llvm-cov exactly.
    """
    file_stats = {
        "justified": 0,
        "stale": 0,
    }

    with open(html_file, "r", encoding="utf-8") as f:
        content = f.read()

    if not justifications:
        return file_stats

    # Determine effective line status (covered if ANY instantiation covers it)
    row_pattern = re.compile(
        r"<tr><td class='line-number'><a name='L(\d+)' href='[^']*'><pre>\d+</pre></a></td>"
        r"<td class='(covered-line|uncovered-line|skipped-line)'>"
    )
    line_effective_status: Dict[int, str] = {}
    for m in row_pattern.finditer(content):
        line_num = int(m.group(1))
        line_class = m.group(2)
        if line_class == "covered-line":
            line_effective_status[line_num] = "covered"
        elif line_class == "uncovered-line":
            if line_num not in line_effective_status:
                line_effective_status[line_num] = "uncovered"

    # Determine which justified lines are stale vs applicable
    for line_num, justification in justifications.items():
        status = line_effective_status.get(line_num)
        if status == "covered":
            file_stats["stale"] += 1
            stale_justifications.append({
                "file": html_file.stem,
                "line": line_num,
                "id": justification.get("id", ""),
                "reason": "Line is already covered — justification is stale",
            })
        elif status == "uncovered":
            file_stats["justified"] += 1
            applied_justifications.append({
                "file": html_file.stem,
                "line": line_num,
                "id": justification.get("id", ""),
                "category": justification.get("category", ""),
            })

    # Restyle justified lines in the HTML (all occurrences including instantiations).
    # Full row pattern to capture and replace the entire row:
    # <tr><td class='line-number'>...</td><td class='uncovered-line'><pre>0</pre></td><td class='code'><pre>...</pre>...</td></tr>
    full_row_pattern = re.compile(
        r"(<tr><td class='line-number'><a name='L(\d+)' href='[^']*'><pre>\d+</pre></a></td>)"
        r"(<td class='uncovered-line'><pre>)\d+(</pre></td>)"
        r"(<td class='code'><pre>)(.*?)(</pre>)"
    )

    modified = False

    def replace_full_row(match: re.Match) -> str:
        nonlocal modified
        line_num = int(match.group(2))
        if line_num not in justifications:
            return match.group(0)

        justification = justifications[line_num]
        reason = justification.get("reason", "").replace("'", "&#39;").replace('"', "&quot;")
        jid = justification.get("id", "")
        tooltip = f"Justified [{jid}]: {reason}"
        modified = True

        # Rebuild the row with justified styling:
        # 1. Line number td (unchanged)
        line_td = match.group(1)
        # 2. Count td: change class and show "J" instead of "0"
        count_td = f"<td class='justified-line' title='{tooltip}'><pre>J{match.group(4)}"
        # 3. Code td: replace 'region red' spans with 'region justified'
        code_start = match.group(5)
        code_content = match.group(6).replace("class='region red'", "class='region justified'")
        code_end = match.group(7)

        return line_td + count_td + code_start + code_content + code_end

    new_content = full_row_pattern.sub(replace_full_row, content)

    if modified:
        with open(html_file, "w", encoding="utf-8") as f:
            f.write(new_content)

    return file_stats


def parse_index_page_totals(html_dir: Path) -> Tuple[int, int]:
    """Parse the TOTALS row from the llvm-cov index.html to get exact coverage numbers.

    Returns (covered_lines, total_lines) matching what llvm-cov displays.
    The index page has a TOTALS row with format: "93.55% (17565/18777)"
    """
    index_file = html_dir / "index.html"
    if not index_file.exists():
        return 0, 0

    with open(index_file, "r", encoding="utf-8") as f:
        content = f.read()

    # Look for the line coverage percentage with (covered/total) format.
    # The TOTALS row has: function_pct (func_hit/func_total) line_pct (line_hit/line_total) branch_pct ...
    # The line coverage is the second percentage with parenthetical.
    # Pattern: XX.XX% (COVERED/TOTAL) — find all and take the second one (lines).
    pct_pattern = re.compile(r"(\d+\.\d+)%\s*\((\d+)/(\d+)\)")
    matches = pct_pattern.findall(content)

    if len(matches) >= 2:
        # Second match is line coverage (first is function coverage)
        # But we need to find it in the TOTALS row specifically.
        # The last 3 matches in the file are from the TOTALS row (func, line, branch)
        # Take the second-to-last group of 3.
        # Actually, just take the last 3 matches — they're from TOTALS.
        totals_matches = matches[-3:]  # func, line, branch from TOTALS
        if len(totals_matches) >= 2:
            _, covered_str, total_str = totals_matches[1]  # line coverage
            return int(covered_str), int(total_str)

    # Fallback: couldn't parse
    print("WARNING: Could not parse coverage totals from index.html", file=sys.stderr)
    return 0, 0


def inject_justified_css(html_dir: Path) -> None:
    """Add CSS for justified lines to style.css."""
    style_file = html_dir / "style.css"
    if not style_file.exists():
        return

    justified_css = """
/* Coverage justification styling */
.justified-line {
  text-align: right;
  color: #a60;
}
.region.justified {
  background-color: #fa04;
}
tr:has(> td.justified-line) > td.code {
  background-color: #fff3e0;
}
@media (prefers-color-scheme: dark) {
  .justified-line {
    color: #fa0;
  }
  tr:has(> td.justified-line) > td.code {
    background-color: #3d2800;
  }
  .region.justified {
    background-color: #fa03;
  }
}
"""

    with open(style_file, "a", encoding="utf-8") as f:
        f.write(justified_css)


def update_index_page(html_dir: Path, stats: Dict[str, Any]) -> None:
    """Add effective coverage banner to the index page."""
    index_file = html_dir / "index.html"
    if not index_file.exists():
        return

    with open(index_file, "r", encoding="utf-8") as f:
        content = f.read()

    banner = (
        f"<div style='background:#ffe4b5;padding:10px;margin:10px 0;border-radius:5px;"
        f"border:1px solid #daa520;'>"
        f"<strong>Effective Line Coverage: {stats['effective_line_coverage_pct']}%</strong> "
        f"(Raw: {stats['raw_line_coverage_pct']}% | "
        f"Justified: {stats['justified_lines']} lines | "
        f"Unjustified Uncovered: {stats['unjustified_uncovered_lines']} lines)"
        f"</div>"
    )

    # Insert after the <body> tag or after the first <h2>
    if "<h2>" in content:
        content = content.replace("<h2>", banner + "<h2>", 1)
    else:
        content = content.replace("<body>", f"<body>{banner}", 1)

    with open(index_file, "w", encoding="utf-8") as f:
        f.write(content)


def find_source_html_files(html_dir: Path) -> List[Path]:
    """Find all per-source HTML files (not index.html, style.css, etc.)."""
    coverage_dir = html_dir / "coverage"
    if not coverage_dir.exists():
        # Some llvm-cov versions put source files directly in html_dir
        coverage_dir = html_dir

    files = []
    for html_file in coverage_dir.rglob("*.html"):
        if html_file.name in ("index.html",):
            continue
        files.append(html_file)
    return sorted(files)


def extract_source_path_from_html(html_file: Path, html_dir: Path) -> str:
    """Extract the relative source file path from the HTML file path.

    llvm-cov creates paths like: html_report/coverage/<full-path-to-source>.html
    We need to extract the relative path within the project.
    """
    rel = str(html_file.relative_to(html_dir))
    # Remove "coverage/" prefix if present
    if rel.startswith("coverage/"):
        rel = rel[len("coverage/"):]
    # Remove .html suffix
    if rel.endswith(".html"):
        rel = rel[:-5]
    return rel


def find_matching_justifications(
    source_path: str, justified_files: Dict[str, Dict[str, Dict[str, str]]]
) -> Dict[int, Dict[str, str]]:
    """Find justifications that match the given source path.

    The source_path from HTML may be an absolute path or relative.
    The justified_files keys are relative to source root.
    We match by suffix.
    """
    result: Dict[int, Dict[str, str]] = {}

    for justified_path, line_justifications in justified_files.items():
        # Match if the source_path ends with the justified_path
        if source_path.endswith(justified_path) or justified_path.endswith(source_path):
            for line_str, justification in line_justifications.items():
                result[int(line_str)] = justification

    return result


def write_summary(
    path: Path, stats: Dict[str, Any], stale: List[Dict[str, Any]]
) -> None:
    """Write human-readable summary."""
    with open(path, "w", encoding="utf-8") as f:
        f.write("Coverage Justification Summary\n")
        f.write("=" * 40 + "\n\n")
        f.write(f"Total instrumented lines: {stats['total_instrumented_lines']}\n")
        f.write(f"Covered lines:            {stats['covered_lines']}\n")
        f.write(f"Justified lines:          {stats['justified_lines']}\n")
        f.write(f"Unjustified uncovered:    {stats['unjustified_uncovered_lines']}\n")
        f.write(f"\n")
        f.write(f"Raw line coverage:        {stats['raw_line_coverage_pct']}%\n")
        f.write(f"Effective line coverage:  {stats['effective_line_coverage_pct']}%\n")
        f.write(f"\n")
        if stale:
            f.write(f"Stale justifications ({len(stale)}):\n")
            for s in stale:
                f.write(f"  - {s['file']}:{s['line']} [{s['id']}]\n")
            f.write("\n")


def load_manifest(path: Path) -> Dict[str, Any]:
    """Load the justification manifest JSON."""
    if not path.exists():
        print(f"ERROR: Manifest not found: {path}", file=sys.stderr)
        sys.exit(1)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Effective coverage calculator and HTML post-processor"
    )
    parser.add_argument(
        "--html-dir",
        type=Path,
        required=True,
        help="Path to llvm-cov HTML report directory",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        required=True,
        help="Path to resolved justification manifest (from justify.py)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output path for justification report (JSON)",
    )
    return parser.parse_args()


if __name__ == "__main__":
    main()
