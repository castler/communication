# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Helper module that offers implementations related to LLVM tools."""

import dataclasses
import enum
import pathlib
import shutil
import subprocess
import typing as t

from scripts.utils import logger as logging
from scripts.utils import pathlib_utils


class ReportRelativeDir(enum.Enum):
    """Enum that defines the different report directories."""

    HTML = "html_report"
    TEXT = "text_report"
    LCOV = "lcov_report"
    JSON = "json_report"
    RAW = "raw_report"


@dataclasses.dataclass
class UserConfiguration:
    """Available user configurations."""

    html_flat_view: bool
    show_instantiations: bool
    show_expansions: bool
    show_mcdc: bool
    export_json: bool

    def __post_init__(self):
        """Validate the user configuration."""
        self.html_flat_view = bool(self.html_flat_view)
        self.show_instantiations = bool(self.show_instantiations)
        self.show_expansions = bool(self.show_expansions)
        self.show_mcdc = bool(self.show_mcdc)
        self.export_json = bool(self.export_json)


@dataclasses.dataclass
class CoverageInformation:
    """Dataclass that holds information to generate coverage reports."""

    profdata: pathlib.Path
    object_files: t.Set[pathlib.Path]
    matched_sources: t.Set[pathlib.Path]
    report_dir: pathlib.Path


class Operator:
    """Operator class that ease the use of LLVM tools."""

    ##################
    # Common section #
    ##################

    def __init__(self, llvm_bin_dir: pathlib.Path, execroot: pathlib.Path):
        self.llvm_bin_dir = llvm_bin_dir
        self.execroot = execroot

        self.llvm_cov_bin = llvm_bin_dir / "llvm-cov"
        self.llvm_profdata_bin = llvm_bin_dir / "llvm-profdata"
        self.llvm_demangler_bin = llvm_bin_dir / "llvm-cxxfilt"

    def _execute(self, command: t.List[str]) -> str:
        """Executes a given command, expecting a zero return code.

        Commands are executed from the `execroot` directory to allow the use of relative paths,
        thereby reducing command length.

        Since Bazel frequently uses stderr for regular output, stderr is piped into stdout.
        """
        logging.debug(f"Executing command:\n{command}")

        try:
            result = subprocess.run(
                command,
                cwd=self.execroot,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=True,
                shell=False,
            )
        except subprocess.CalledProcessError as exception:
            logging.error(
                "The command ```\n%s\n``` returned code `%s` and the following error message:```\n%s\n```",
                command,
                exception.returncode,
                exception.stdout,
            )
            raise exception

        return result.stdout

    #########################
    # LLVM profdata section #
    #########################

    def profdata_merge(
        self,
        files: t.List[pathlib.Path],
        output: pathlib.Path,
    ) -> str:
        """Merge profdata or profraw files into a single profdata file."""
        command = [
            str(self.llvm_profdata_bin),
            "merge",
            "--output",
            str(output),
        ] + sorted(map(str, files))

        stdout = self._execute(command)

        assert output.exists(), f"Unexpected error, profdata file `{output}` was not created."

        return stdout

    ####################
    # LLVM cov section #
    ####################

    def _cov_select_covered_sources(self, sources: t.Set[pathlib.Path]):
        """Return llvm-cov arguments that set which files are to be covered."""
        return [
            "--sources",
            *sorted(map(lambda file: str(pathlib_utils.try_relative_to(file, self.execroot)), sources)),
        ]

    def _cov_path_equivalence_args(self):
        """Return llvm-cov arguments that modify Bazel internal paths into something accessible for the user.

        Bazel uses `/proc/self/cwd` as the internal root seen by the compiler.
        To make files accessible for the user, this path should be replaced with the current execroot.
        """
        return [
            f"--path-equivalence=/proc/self/cwd/bazel-out,{self.execroot / 'bazel-out/'}",
            f"--path-equivalence=/proc/self/cwd/external,{self.execroot / 'external'}",
            f"--path-equivalence=/proc/self/cwd/,{self.execroot}",
            f"--compilation-dir={self.execroot}",
        ]

    def _cov_instrumentation_args(self, profdata: pathlib.Path, object_files: t.Set[pathlib.Path]):
        """Return llvm-cov arguments that set the instrumentation profiles and its objects."""
        return [
            *sorted(map(lambda file: f"--object={pathlib_utils.try_relative_to(file, self.execroot)}", object_files)),
            f"--instr-profile={profdata}",
        ]

    def create_lcov_report(
        self,
        cov_info: CoverageInformation,
    ) -> pathlib.Path:
        """Execute llvm-cov to export a certain profdata to an lcov report.

        This command creates a directory containing all LCOV files.
        """
        command = [
            str(self.llvm_cov_bin),
            "export",
            f"--format=lcov",
            f"--show-region-summary=0",
            f"--Xdemangler={self.llvm_demangler_bin}",
            *self._cov_path_equivalence_args(),
            *self._cov_instrumentation_args(cov_info.profdata, cov_info.object_files),
            *self._cov_select_covered_sources(cov_info.matched_sources),
        ]

        stdout = self._execute(command)

        lcov_report_dir = cov_info.report_dir / ReportRelativeDir.LCOV.value
        lcov_report_dir.mkdir(parents=True, exist_ok=True)
        lcov_report_dir.joinpath("lcov.dat").write_text(stdout, encoding="utf-8")

        lcov_report_dir.joinpath("command_output.txt").write_text(stdout, encoding="utf-8")

        return lcov_report_dir

    def create_json_report(
        self,
        cov_info: CoverageInformation,
    ) -> pathlib.Path:
        """Execute llvm-cov to export a certain profdata to a json report.

        This command creates a directory containing the JSON file.
        """
        command = [
            str(self.llvm_cov_bin),
            "export",
            f"--format=text",
            f"--show-region-summary=0",
            f"--Xdemangler={self.llvm_demangler_bin}",
            f"-j=1",  # This is necessary as the JSON exporter as a race condition bug with large codebases.
            *self._cov_path_equivalence_args(),
            *self._cov_instrumentation_args(cov_info.profdata, cov_info.object_files),
            *self._cov_select_covered_sources(cov_info.matched_sources),
        ]

        stdout = self._execute(command)

        try:
            json_report_start = stdout.index("{")
        except ValueError:
            logging.warning("Failed to find the start of the JSON report in llvm-cov output.")
            json_report_start = 0

        json_report = stdout[json_report_start:]

        json_report_dir = cov_info.report_dir / ReportRelativeDir.JSON.value
        json_report_dir.mkdir(parents=True, exist_ok=True)
        json_report_dir.joinpath("report.json").write_text(json_report, encoding="utf-8")

        return json_report_dir

    def _cov_show_args(
        self,
        cov_info: CoverageInformation,
        output_format: str,
        extra_arguments: t.Optional[t.List[str]] = None,
    ) -> t.List[str]:
        return [
            str(self.llvm_cov_bin),
            "show",
            f"--format={output_format}",
            f"--show-region-summary=0",
            f"--Xdemangler={self.llvm_demangler_bin}",
            f"--show-branches=count",
            f"--coverage-watermark=100,50",
            *(extra_arguments or []),
            *self._cov_path_equivalence_args(),
            *self._cov_instrumentation_args(cov_info.profdata, cov_info.object_files),
            *self._cov_select_covered_sources(cov_info.matched_sources),
        ]

    def create_html_report(
        self,
        cov_info: CoverageInformation,
        user_config: UserConfiguration,
    ) -> pathlib.Path:
        """Execute llvm-cov to show a certain profdata as an HTML format.

        This command creates a directory containing all HTML files.
        """
        html_report_dir = cov_info.report_dir / ReportRelativeDir.HTML.value
        html_report_dir.mkdir(parents=True, exist_ok=True)

        extra_arguments = [f"--output-dir={html_report_dir}"]
        extra_arguments.append(f"--show-directory-coverage={not user_config.html_flat_view}")
        extra_arguments.append(f"--show-instantiations={user_config.show_instantiations}")
        extra_arguments.append(f"--show-expansions={user_config.show_expansions}")
        extra_arguments.append(f"--show-mcdc={user_config.show_mcdc}")
        extra_arguments.append(f"--show-mcdc-summary={user_config.show_mcdc}")

        command = self._cov_show_args(cov_info, "html", extra_arguments)

        stdout = self._execute(command)
        html_report_dir.joinpath("command_output.txt").write_text(stdout, encoding="utf-8")

        return html_report_dir

    def create_text_report(
        self,
        cov_info: CoverageInformation,
        user_config: UserConfiguration,
    ) -> pathlib.Path:
        """Execute llvm-cov to show a certain profdata as a textual format.

        This command creates a directory containing all text files.
        """
        extra_arguments = []
        extra_arguments.append(f"--show-instantiations={user_config.show_instantiations}")
        extra_arguments.append(f"--show-expansions={user_config.show_expansions}")
        extra_arguments.append(f"--show-mcdc={user_config.show_mcdc}")
        extra_arguments.append(f"--show-mcdc-summary={user_config.show_mcdc}")

        command = self._cov_show_args(cov_info, "text", extra_arguments)

        stdout = self._execute(command)

        text_report_dir = cov_info.report_dir / ReportRelativeDir.TEXT.value
        text_report_dir.mkdir(parents=True, exist_ok=True)
        text_report_dir.joinpath("report.txt").write_text(stdout)

        text_report_dir.joinpath("command_output.txt").write_text(stdout, encoding="utf-8")

        return text_report_dir

    def create_text_summary_report(
        self,
        cov_info: CoverageInformation,
        user_config: UserConfiguration,
    ) -> pathlib.Path:
        """Execute llvm-cov to create a coverage summary report."""
        command = [
            str(self.llvm_cov_bin),
            "report",
            f"--summary-only",
            f"--show-region-summary=0",
            f"--show-branch-summary=1",
            f"--show-mcdc-summary={user_config.show_mcdc}",
            *self._cov_path_equivalence_args(),
            *self._cov_instrumentation_args(cov_info.profdata, cov_info.object_files),
            *self._cov_select_covered_sources(cov_info.matched_sources),
        ]

        stdout = self._execute(command)

        text_report_dir = cov_info.report_dir / ReportRelativeDir.TEXT.value
        text_report_dir.mkdir(parents=True, exist_ok=True)
        text_report_dir.joinpath("summary.txt").write_text(stdout)

        return text_report_dir

    def create_raw_report(self, cov_info: CoverageInformation) -> pathlib.Path:
        """Create a raw report containing profdata and object files plus filter regexes."""

        raw_report_dir = cov_info.report_dir / ReportRelativeDir.RAW.value

        objects_dir = raw_report_dir / "objects"
        objects_dir.mkdir(parents=True, exist_ok=True)

        shutil.copyfile(cov_info.profdata, raw_report_dir / cov_info.profdata.name)
        objects_dir.joinpath("matches_sources.txt").write_text("\n".join(map(str, cov_info.matched_sources)))
        for object_file in cov_info.object_files:
            shutil.copyfile(object_file, objects_dir / object_file.name)

        return raw_report_dir
