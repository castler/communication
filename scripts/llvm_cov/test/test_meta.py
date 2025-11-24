# Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

"""Tests for the meta module."""

import json
import pathlib
import zipfile

from quality_tools.llvm_cov import meta
from quality_tools.llvm_cov.test import conftest


def test_meta_info_to_json(tmp_path: pathlib.Path):
    """Test meta.Info.to_json method."""
    json_file = tmp_path / "test.json"
    meta_info = conftest.default_meta_info()

    meta_info.to_json(json_file)

    assert conftest.DEFAULT_META_INFO_DICT == json.loads(json_file.read_text(encoding="utf-8"))


def test_meta_info_from_json(tmp_path: pathlib.Path):
    """Test meta.Info.from_json method."""
    json_file = tmp_path / "test.json"
    json_file.write_text(json.dumps(conftest.DEFAULT_META_INFO_DICT), encoding="utf-8")

    meta_info = meta.Info.from_json(json_file)

    assert meta_info == conftest.default_meta_info()


def test_meta_data_to_zip(tmp_path: pathlib.Path):
    """Test meta.Data.to_zip method."""
    zip_dir = tmp_path / "test"
    zip_file = tmp_path / "test.zip"
    meta_info = conftest.default_meta_info()
    profdata_file = tmp_path / "test.profdata"
    profdata_file.touch()
    meta_info.profdata = profdata_file
    meta_data = meta.Data(
        directory=zip_dir,
        info=meta_info,
    )

    expected_zipped_files = [
        meta.Data._META_JSON,  # pylint: disable=protected-access
        meta.Data._PROFDATA_JSON,  # pylint: disable=protected-access
    ]

    meta_data.to_zip(zip_file)

    with zipfile.ZipFile(zip_file, "r") as archive:
        for file in archive.filelist:
            assert file.filename in expected_zipped_files


def test_meta_data_from_zip(tmp_path: pathlib.Path):
    """Test meta.Data.from_zip method."""
    zip_dir = tmp_path / "test"
    zip_file = tmp_path / "test.zip"
    profdata_file = tmp_path / meta.Data._PROFDATA_JSON  # pylint: disable=protected-access
    profdata_file.touch()
    meta_file = tmp_path / "meta.json"
    meta_file.write_text(json.dumps(conftest.DEFAULT_META_INFO_DICT))
    with zipfile.ZipFile(zip_file, "w") as archive:
        archive.write(profdata_file, profdata_file.relative_to(tmp_path))
        archive.write(meta_file, meta_file.relative_to(tmp_path))

    expected_meta_info = conftest.default_meta_info()
    expected_zipped_files = [
        meta.Data._META_JSON,  # pylint: disable=protected-access
        meta.Data._PROFDATA_JSON,  # pylint: disable=protected-access
    ]

    meta_data = meta.Data.from_zip(
        directory=zip_dir,
        zip_file=zip_file,
    )

    assert meta_data.directory == zip_dir
    assert meta_data.directory.exists()
    for file in meta_data.directory.rglob("*"):
        assert file.name in expected_zipped_files
    assert meta_data.info == expected_meta_info


def test_meta_merged_info(tmp_path: pathlib.Path):
    """Test meta.MergedInfo.from_multiple_meta_datas method."""
    directories = [
        tmp_path / "dir1",
        tmp_path / "dir2",
    ]
    meta_infos = [
        conftest.default_meta_info(),
        conftest.default_meta_info(),
    ]
    meta_infos[1].object_files = [pathlib.Path("file4")]
    meta_infos[1].matched_sources = [pathlib.Path("file5")]
    meta_infos[1].profdata = pathlib.Path("profdata2")
    meta_datas = [
        meta.Data(
            directory=directories[index],
            info=meta_infos[index],
        )
        for index in range(2)
    ]

    meta_merged_info = meta.MergedInfo.from_multiple_meta_datas(meta_datas)

    assert meta_merged_info.llvm_bin_dir == meta_infos[0].llvm_bin_dir
    assert meta_merged_info.execroot == meta_infos[0].execroot
    assert meta_merged_info.user_config == meta_infos[0].user_config
    assert meta_merged_info.object_files == set(meta_infos[0].object_files + meta_infos[1].object_files)
    assert meta_merged_info.matched_sources == set(meta_infos[0].matched_sources + meta_infos[1].matched_sources)
    assert meta_merged_info.profdata_files == {meta_datas[0].profdata, meta_datas[1].profdata}
