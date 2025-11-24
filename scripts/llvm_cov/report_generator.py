# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Custom Bazel report genertor that supports LLVM Cov.

This script is called by a `bazel coverage` command when `--coverage_report_generator` flag is properly set.

It merges all outputs from the respective Bazel `output_generator` and outputs a single report.

The report_generator output is a zip file containing the following directories:
- html_report: The HTML report
- text_report: The text report
- lcov_report: The LCOV report
- raw_report: The raw reports (profdata and object files)
"""

import argparse
import dataclasses
import pathlib
import re
import sys
import typing as t
import zipfile

from scripts.llvm_cov import llvm, meta
from scripts.utils import logger as logging


@dataclasses.dataclass
class Arguments:
    """Class that provides a clean interface to the module raw arguments."""

    output_file: pathlib.Path
    reports_file: pathlib.Path


def parse_args() -> Arguments:
    """Parse raw arguments and return them as an Arguments object.

    Note that this interface is fed and maintained by Bazel itself.
    """
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawTextHelpFormatter,
    )

    parser_required = parser.add_argument_group("required arguments")
    parser_required.add_argument(
        "--output_file",
        type=pathlib.Path,
        required=True,
        help="Path to which the output file is written.",
    )
    parser_required.add_argument(
        "--reports_file",
        type=pathlib.Path,
        required=True,
        help="File containing tbe list of reports created by the `output_generator` for each target.",
    )

    return Arguments(**vars(parser.parse_args()))


def get_meta_datas_from_reports(reports_file: pathlib.Path) -> t.List[meta.Data]:
    """Collect `output_generator` meta data zip files from Bazel coverage reports file."""
    with reports_file.open(encoding="utf-8") as stream:
        meta_zip_files = [pathlib.Path(line.strip()) for line in stream if "baseline_coverage.dat" not in line]

    return [
        meta.Data.from_zip(directory=file.parent / "meta", zip_file=file)
        for file in meta_zip_files
        if file.stat().st_size > 0
    ]


def create_zip(root: pathlib.Path, directories: t.Set[pathlib.Path], output_file: pathlib.Path) -> None:
    """Create a zip file from a list of directories that are relative to the given root."""
    with zipfile.ZipFile(output_file, "w") as archive:
        for directory in directories:
            for file in directory.rglob("*"):
                archive.write(file, file.relative_to(root))


def strip_sandbox(path: pathlib.Path) -> pathlib.Path:
    """Strips put sandbox parts from a certain path."""
    return pathlib.Path(re.sub(r"sandbox/[A-Za-z0-9]*-sandbox/\d*/", r"", str(path)))


def main():
    """Main logic for the reporter generator."""

    logging.setup(verbose=False)
    logging.debug(f"Running `{__name__}`")

    args = parse_args()

    meta_datas = get_meta_datas_from_reports(args.reports_file)

    if not meta_datas:
        logging.error("Did not find any instrumented targets.")
        sys.exit(1)

    meta_info = meta.MergedInfo.from_multiple_meta_datas(meta_datas)

    if not meta_info.matched_sources:
        logging.error(
            "Did not match any source file to display the report for."
            " Please check your targets, exclusion and inclusion patterns, and instrumentation filter."
        )
        sys.exit(1)

    meta_info.object_files = {strip_sandbox(file).resolve() for file in meta_info.object_files}

    llvm_operator = llvm.Operator(meta_info.llvm_bin_dir, meta_info.execroot)

    merged_profdata = pathlib.Path("coverage_report_merged.dat").absolute()
    llvm_operator.profdata_merge(
        files=meta_info.profdata_files,
        output=merged_profdata,
    )

    report_dir = pathlib.Path.cwd()
    cov_info = llvm.CoverageInformation(
        profdata=merged_profdata,
        object_files=meta_info.object_files,
        matched_sources=meta_info.matched_sources,
        report_dir=report_dir,
    )

    zip_directories = set()

    zip_directories.add(llvm_operator.create_raw_report(cov_info))

    zip_directories.add(llvm_operator.create_lcov_report(cov_info))

    if meta_info.user_config.export_json:
        zip_directories.add(llvm_operator.create_json_report(cov_info))

    zip_directories.add(llvm_operator.create_text_report(cov_info, meta_info.user_config))
    zip_directories.add(llvm_operator.create_text_summary_report(cov_info, meta_info.user_config))

    zip_directories.add(llvm_operator.create_html_report(cov_info, meta_info.user_config))

    create_zip(
        root=pathlib.Path.cwd(),
        directories=zip_directories,
        output_file=args.output_file,
    )


if __name__ == "__main__":
    main()
