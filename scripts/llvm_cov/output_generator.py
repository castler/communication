# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Custom Bazel output genertor that supports LLVM Cov.

This script is called by a `bazel coverage` command when `--coverage_output_generator` flag is properly set.

It creates one coverage report for every instrumented test plus some additional information and then output it to the
 respective Bazel `report_generator`.

The output_generator output is a zip file containing the following directories:
- meta: meta information for Bazel `report_generator` containing profdata and additional information
"""

import argparse
import dataclasses
import mimetypes
import os
import pathlib
import re
import sys
import typing as t

from scripts.llvm_cov import exceptions, llvm, meta, path_finder
from scripts.utils import logger as logging
from scripts.utils import pathlib_utils


@dataclasses.dataclass
class Arguments:
    """Class that provides a clean interface to the module raw arguments."""

    coverage_dir: pathlib.Path
    output_file: pathlib.Path
    source_file_manifest: pathlib.Path


def parse_args() -> Arguments:
    """Parse raw arguments and return them as an Arguments object.

    Note that this interface is fed and maintained by Bazel itself.

    Other arguments provided by Bazel but not used in this module are:
        --sources_to_replace_file
            File that contains generated paths that should be replaced.
            This is true, for example, for virtual includes.
        --filter_sources
            List of patterns to filter source files.
    """
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawTextHelpFormatter,
    )

    parser_required = parser.add_argument_group("required arguments")
    parser_required.add_argument(
        "--coverage_dir",
        type=pathlib.Path,
        required=True,
        help="Directory from which `.profraw` files are found.",
    )
    parser_required.add_argument(
        "--output_file",
        type=pathlib.Path,
        required=True,
        help="Path to which the output file is written.",
    )
    parser_required.add_argument(
        "--source_file_manifest",
        type=pathlib.Path,
        required=True,
        help="File that contains the `objects_list.txt` path, which is used to get real paths from profdata files.",
    )

    return Arguments(**vars(parser.parse_known_args()[0]))


def get_profdata_file_names_as_seen_by_llvm_cov(lcov_stdout: str) -> t.Set[str]:
    """Get profdata files for filtering.

    This is required to get the real path of the profdata files as Bazel creates a symlink to the real file and
    the real path is required for filtering.
    """
    profdata_files = set()
    lcov_file_token = "SF:"
    for lcov_line in lcov_stdout.splitlines():
        if not lcov_line.startswith(lcov_file_token):
            continue
        original_path = lcov_line.split(lcov_file_token, maxsplit=1)[-1]
        profdata_files.add(original_path)
    return profdata_files


def resolve_regex_or_file(regex_or_file: str):
    """Resolves the input being it a regex pattern or a file path.

    If the input is a file, the file is read and its lines are returned.
    If the input is a regex, the regex is verified and returned.
    """
    if not regex_or_file:
        return regex_or_file

    if pathlib.Path(regex_or_file).exists():
        logging.debug(f"Using sources list from file `{regex_or_file}`")
        with open(regex_or_file, encoding="utf-8") as file_handle:
            return list(map(str.strip, file_handle.read().splitlines()))

    try:
        re.compile(regex_or_file)
    except re.error as exception:
        raise exceptions.InvalidLlvmRegexPattern(
            regex_pattern=regex_or_file,
            position=exception.pos - 1 if exception.pos else None,
            explanation=exception.msg,
        ) from None

    logging.debug(f"Using regex pattern `{regex_or_file}`")
    return regex_or_file


def filter_files(files: t.Set[str], regex_or_filelist: t.Union[t.List[str], str], execroot: pathlib.Path) -> t.Set[str]:
    """Filter files by a regex or a filelist."""
    if not regex_or_filelist:
        return set()

    if isinstance(regex_or_filelist, list):
        return {file for file in files if file in regex_or_filelist}
    return {file for file in files if re.match(regex_or_filelist, str(pathlib_utils.try_relative_to(file, execroot)))}


def get_object_list_files(source_file_manifest: pathlib.Path) -> t.Set[pathlib.Path]:
    """Get `objects_list.txt` files from Bazel coverage source file manifest."""
    with source_file_manifest.open(encoding="utf-8") as stream:
        object_list_files = {pathlib.Path(line.strip()) for line in stream if "objects_list.txt" in line}
    return object_list_files


def get_object_files(object_list_files: t.Set[pathlib.Path]) -> t.Set[pathlib.Path]:
    """Get resolved path of object files from a set of `objects_list.txt` files."""
    object_files: t.Set[pathlib.Path] = set()
    for object_list_file in object_list_files:
        with object_list_file.open(encoding="utf-8") as stream:
            object_files.update(pathlib.Path(line.strip()).resolve() for line in stream)
    return object_files


def get_profraw_files(coverage_dir: pathlib.Path) -> t.Set[pathlib.Path]:
    """Get `.profraw` files from Bazel coverage directory."""
    return set(map(lambda file: file.absolute(), coverage_dir.rglob("*.profraw")))


def get_matched_sources(
    profdata_source_files: t.Set[str],
    include_regex_or_file: str,
    exclude_regex_or_file: str,
    execroot: pathlib.Path,
) -> t.Set[str]:
    """Match `.profdata` source files baesd on inclusion and exclusion filters.

    Both inclusion and exclusion filter are based on either a regex or a file containing a list of paths.
    """
    include_regex_or_filelist = resolve_regex_or_file(include_regex_or_file)
    exclude_regex_or_filelist = resolve_regex_or_file(exclude_regex_or_file)

    matched_sources = filter_files(
        files=profdata_source_files, regex_or_filelist=include_regex_or_filelist, execroot=execroot
    )
    ignored_sources = filter_files(
        files=profdata_source_files, regex_or_filelist=exclude_regex_or_filelist, execroot=execroot
    )

    return matched_sources - ignored_sources


def exit_if_source_file_not_part_of_manifest(source_file_manifest):
    """Filter manifest_sources based on mimetype.

    The function mimetypes.guess_type returns a tuple where the first entry is
    the guessed type and the second one is the encoding of that file.

    If the type is not known, it is certainly not a source file.
    If the type does not start with text/x-c, it is not a C/C++ source nor header file.
    If there are no sources for the current target, exit normally, but early.
    """

    with open(source_file_manifest, encoding="utf-8") as file_handle:
        manifest_sources = {line.strip() for line in file_handle.readlines()}

    # Filter out those files from the manifest_sources which are binary.
    # Binary files will have the mimetype "None". Source files have mimetype "text/<detail>".
    # Reason being, that when using a instrumentation filter, the manifest_sources still
    # contains the binary files (the test executables) but not the source files anymore.
    # If for a specific test, no source files are requested to be instrumented, we do not want
    # to run the llvm_cov profdata merger, and therefore just skip it by returning exit code 0.
    def filter_lambda(manifest_source):
        guessed_type = mimetypes.guess_type(manifest_source)[0]
        return guessed_type is not None and guessed_type.startswith("text/x-c")

    filtered_manifest_sources = list(filter(filter_lambda, manifest_sources))
    if not filtered_manifest_sources:
        logging.warning(f"No instrumentation sources were found. Ignoring '{os.environ['TEST_TARGET']}'.")
        sys.exit(0)


def main():
    """Main logic for the output generator."""

    logging.setup(verbose=False)
    logging.debug(f"Running `{__name__}` on `{os.environ['TEST_TARGET']}`")

    args = parse_args()

    exit_if_source_file_not_part_of_manifest(args.source_file_manifest)

    object_list_files = get_object_list_files(args.source_file_manifest)
    object_files = get_object_files(object_list_files)
    profraw_files = get_profraw_files(args.coverage_dir)

    if not object_files or not profraw_files:
        logging.warning(f"No instrumentation files were found. Ignoring '{os.environ['TEST_TARGET']}'.")
        sys.exit(0)

    llvm_bin_dir = path_finder.llvm_bin_dir()
    execroot = path_finder.execroot(llvm_bin_dir)

    llvm_operator = llvm.Operator(llvm_bin_dir, execroot)

    profdata = args.coverage_dir.joinpath("target.profdata").absolute()
    llvm_operator.profdata_merge(files=profraw_files, output=profdata)

    cov_info = llvm.CoverageInformation(
        profdata=profdata,
        object_files=object_files,
        matched_sources=set(),
        report_dir=args.coverage_dir,
    )
    lcov_dir = llvm_operator.create_lcov_report(cov_info)
    lcov_file = lcov_dir.joinpath("lcov.dat")
    profdata_source_files = get_profdata_file_names_as_seen_by_llvm_cov(lcov_file.read_text(encoding="utf-8"))

    matched_sources = get_matched_sources(
        profdata_source_files=profdata_source_files,
        include_regex_or_file=os.environ.get("LLVM_COV_INCLUDE", ".*"),
        exclude_regex_or_file=os.environ.get("LLVM_COV_EXCLUDE", ""),
        execroot=execroot,
    )

    meta_data = meta.Data(
        directory=args.coverage_dir / "meta",
        info=meta.Info(
            llvm_bin_dir=llvm_bin_dir,
            execroot=execroot,
            profdata=profdata,
            object_files=list(object_files),
            matched_sources=list(matched_sources),
            user_config=llvm.UserConfiguration(
                html_flat_view=os.environ.get("HTML_FLAT_VIEW", False),
                show_instantiations=os.environ.get("SHOW_INSTANTIATIONS", False),
                show_expansions=os.environ.get("SHOW_EXPANSIONS", False),
                show_mcdc=os.environ.get("SHOW_MCDC", False),
                export_json=os.environ.get("EXPORT_JSON", False),
            ),
        ),
    )
    meta_data.to_zip(args.output_file)


if __name__ == "__main__":  # pragma: no cover
    main()
