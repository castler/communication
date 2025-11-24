# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Interface module between every `output_generator` and the `report_generator`."""

import dataclasses
import json
import pathlib
import typing as t
import zipfile

from scripts.llvm_cov import llvm


@dataclasses.dataclass
class BaseInfo:
    """Common data between every `output_generator` and the `report_generator`."""

    llvm_bin_dir: pathlib.Path
    execroot: pathlib.Path
    user_config: llvm.UserConfiguration


@dataclasses.dataclass
class Info(BaseInfo):
    """Common data plus a single set of custom data of one `output_generator`."""

    object_files: t.List[pathlib.Path]
    matched_sources: t.List[pathlib.Path]
    profdata: pathlib.Path

    class JSONEncoder(json.JSONEncoder):
        """Encodes dataclass objects using asdict and other objects as strings."""

        def default(self, o):
            """Overrides default encoding."""
            if isinstance(o, pathlib.Path):
                return str(o)
            if isinstance(o, llvm.UserConfiguration):
                return o.__dict__
            # This is not used when mypy type enforcement is respected.
            # Left as a good practice and to improve readability.
            return super().default(o)  # pragma: no cover

    @classmethod
    def from_json(cls, file: pathlib.Path) -> "Info":
        """Create an instance from a JSON file."""
        raw_content = json.loads(file.read_text(encoding="utf-8"))
        return cls(
            llvm_bin_dir=pathlib.Path(raw_content["llvm_bin_dir"]).absolute(),
            execroot=pathlib.Path(raw_content["execroot"]).absolute(),
            profdata=pathlib.Path(raw_content["profdata"]).absolute(),
            object_files=list(map(lambda file: pathlib.Path(file).absolute(), raw_content["object_files"])),
            matched_sources=list(map(lambda file: pathlib.Path(file).absolute(), raw_content["matched_sources"])),
            user_config=llvm.UserConfiguration(**raw_content["user_config"]),
        )

    def to_json(self, path: pathlib.Path):
        """Write the fields to a JSON file."""
        path.write_text(json.dumps(self.__dict__, indent=4, cls=self.JSONEncoder))


class Data:
    """File interface of data shared between every `output_generator` and the `report_generator`."""

    _META_JSON = "meta.json"
    _PROFDATA_JSON = "target.profdata"

    def __init__(self, directory: pathlib.Path, info: Info):
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True)
        self.info = info

    @property
    def profdata(self) -> pathlib.Path:
        """Returns the profdata path."""
        return self.directory.joinpath(self._PROFDATA_JSON).absolute()

    @classmethod
    def from_zip(cls, directory: pathlib.Path, zip_file: pathlib.Path) -> "Data":
        """Create a code representation of a `output_generator` zip file."""
        with zipfile.ZipFile(zip_file, "r") as archive:
            archive.extractall(directory)
        return cls(directory, Info.from_json(directory.joinpath(cls._META_JSON)))

    def to_zip(self, path: pathlib.Path):
        """Compress all information of a `output_generator` into a zip file."""
        self.info.to_json(self.directory.joinpath(self._META_JSON))
        self.directory.joinpath(self._PROFDATA_JSON).write_bytes(self.info.profdata.read_bytes())

        with zipfile.ZipFile(path, "w") as archive:
            for file in self.directory.rglob("*"):
                archive.write(file, file.relative_to(self.directory))


@dataclasses.dataclass
class MergedInfo(BaseInfo):
    """Common data plus all custom datas from each `output_generator`."""

    object_files: t.Set[pathlib.Path]
    matched_sources: t.Set[pathlib.Path]
    profdata_files: t.Set[pathlib.Path]

    @classmethod
    def from_multiple_meta_datas(cls, meta_datas: t.List[Data]) -> "MergedInfo":
        """Merge multiple Fields instances into a single MergedFields instance."""
        merged_fields = cls(
            llvm_bin_dir=meta_datas[0].info.llvm_bin_dir,
            execroot=meta_datas[0].info.execroot,
            user_config=meta_datas[0].info.user_config,
            object_files=set(),
            matched_sources=set(),
            profdata_files=set(),
        )

        for meta_data in meta_datas:
            merged_fields.object_files.update(meta_data.info.object_files)
            merged_fields.matched_sources.update(meta_data.info.matched_sources)
            merged_fields.profdata_files.add(meta_data.profdata)

        return merged_fields
