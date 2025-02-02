#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  NTv2 Testing.
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2010, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import os

import gdaltest
import pytest

from osgeo import gdal

pytestmark = pytest.mark.require_driver("NTV2")

###############################################################################
# Open a little-endian NTv2 grid


def test_ntv2_1():

    tst = gdaltest.GDALTest("NTV2", "ntv2/test_ntv2_le.gsb", 2, 10)
    gt = (-5.52, 7.8, 0.0, 52.05, 0.0, -5.55)
    tst.testOpen(check_gt=gt, check_prj="WGS84")


###############################################################################
# Open a big-endian NTv2 grid


def test_ntv2_2():

    tst = gdaltest.GDALTest("NTV2", "ntv2/test_ntv2_be.gsb", 2, 10)
    gt = (-5.52, 7.8, 0.0, 52.05, 0.0, -5.55)
    tst.testOpen(check_gt=gt, check_prj="WGS84")


###############################################################################
# Test creating a little-endian NTv2 grid


def test_ntv2_3():

    tst = gdaltest.GDALTest(
        "NTV2", "ntv2/test_ntv2_le.gsb", 2, 10, options=["ENDIANNESS=LE"]
    )
    tst.testCreateCopy(vsimem=1)


###############################################################################
# Test creating a big-endian NTv2 grid


def test_ntv2_4():

    tst = gdaltest.GDALTest(
        "NTV2", "ntv2/test_ntv2_le.gsb", 2, 10, options=["ENDIANNESS=BE"]
    )
    tst.testCreateCopy(vsimem=1)


###############################################################################
# Test appending to a little-endian NTv2 grid


def test_ntv2_5():

    src_ds = gdal.Open("data/ntv2/test_ntv2_le.gsb")
    gdal.GetDriverByName("NTv2").Create(
        "/vsimem/ntv2_5.gsb", 1, 1, 4, gdal.GDT_Float32, options=["ENDIANNESS=LE"]
    )
    ds = gdal.GetDriverByName("NTv2").CreateCopy(
        "/vsimem/ntv2_5.gsb", src_ds, options=["APPEND_SUBDATASET=YES"]
    )
    assert ds.FlushCache() == gdal.CE_None
    assert ds.GetRasterBand(2).Checksum() == 10
    ds = None
    ds = gdal.Open("NTv2:1:/vsimem/ntv2_5.gsb")
    assert ds.GetRasterBand(2).Checksum() == 10
    ds = None
    gdal.GetDriverByName("NTv2").Delete("/vsimem/ntv2_5.gsb")


###############################################################################
# Test appending to a big-endian NTv2 grid


def test_ntv2_6():

    src_ds = gdal.Open("data/ntv2/test_ntv2_le.gsb")
    gdal.GetDriverByName("NTv2").Create(
        "/vsimem/ntv2_6.gsb", 1, 1, 4, gdal.GDT_Float32, options=["ENDIANNESS=BE"]
    )
    ds = gdal.GetDriverByName("NTv2").CreateCopy(
        "/vsimem/ntv2_6.gsb", src_ds, options=["APPEND_SUBDATASET=YES"]
    )
    assert ds.GetRasterBand(2).Checksum() == 10
    ds = None
    ds = gdal.Open("NTv2:1:/vsimem/ntv2_6.gsb")
    assert ds.GetRasterBand(2).Checksum() == 10
    ds = None
    gdal.GetDriverByName("NTv2").Delete("/vsimem/ntv2_6.gsb")


###############################################################################
# Test creating a file with invalid filename


def test_ntv2_7():

    with pytest.raises(Exception):
        gdal.GetDriverByName("NTv2").Create(
            "/does/not/exist.gsb", 1, 1, 4, gdal.GDT_Float32
        )

    with pytest.raises(Exception):
        gdal.GetDriverByName("NTv2").Create(
            "/does/not/exist.gsb",
            1,
            1,
            4,
            gdal.GDT_Float32,
            options=["APPEND_SUBDATASET=YES"],
        )


###############################################################################


def test_ntv2_online_1():

    gdaltest.download_or_skip(
        "http://download.osgeo.org/proj/nzgd2kgrid0005.gsb", "nzgd2kgrid0005.gsb"
    )

    try:
        os.stat("tmp/cache/nzgd2kgrid0005.gsb")
    except OSError:
        pytest.skip()

    tst = gdaltest.GDALTest(
        "NTV2", "tmp/cache/nzgd2kgrid0005.gsb", 1, 54971, filename_absolute=1
    )
    gt = (165.95, 0.1, 0.0, -33.95, 0.0, -0.1)
    tst.testOpen(check_gt=gt, check_prj="WGS84")


###############################################################################


def test_ntv2_online_2():

    try:
        os.stat("tmp/cache/nzgd2kgrid0005.gsb")
    except OSError:
        pytest.skip()

    tst = gdaltest.GDALTest(
        "NTV2", "tmp/cache/nzgd2kgrid0005.gsb", 1, 54971, filename_absolute=1
    )
    tst.testCreateCopy(vsimem=1)


###############################################################################


def test_ntv2_online_3():

    try:
        os.stat("tmp/cache/nzgd2kgrid0005.gsb")
    except OSError:
        pytest.skip()

    tst = gdaltest.GDALTest(
        "NTV2", "tmp/cache/nzgd2kgrid0005.gsb", 1, 54971, filename_absolute=1
    )
    tst.testCreate(vsimem=1, out_bands=4)
