#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test SieveFilter() algorithm.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
###############################################################################
# Copyright (c) 2008, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2009-2010, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################


import gdaltest
import pytest

from osgeo import gdal

###############################################################################
# Test a fairly default case.


@pytest.mark.require_driver("AAIGRID")
def test_sieve_1():

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("data/sieve_src.grd")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("tmp/sieve_1.tif", 5, 7, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, None, dst_band, 2, 4)

    cs_expected = 364
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("tmp/sieve_1.tif")

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Try eight connected.


@pytest.mark.require_driver("AAIGRID")
def test_sieve_2():

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("data/sieve_src.grd")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("tmp/sieve_2.tif", 5, 7, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, None, dst_band, 2, 8)

    cs_expected = 370
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("tmp/sieve_2.tif")

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Do a sieve resulting in unmergable polygons.


@pytest.mark.require_driver("AAIGRID")
def test_sieve_3():

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("data/unmergable.grd")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("tmp/sieve_3.tif", 5, 7, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, None, dst_band, 2, 8)

    # cs_expected = 472
    cs_expected = 451
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("tmp/sieve_3.tif")

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Try the bug 2634 simplified data.


@pytest.mark.require_driver("AAIGRID")
def test_sieve_4():

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("data/sieve_2634.grd")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("tmp/sieve_4.tif", 10, 8, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, None, dst_band, 2, 4)

    cs_expected = 98
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("tmp/sieve_4.tif")

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Same as sieve_1, but we provide a mask band
# This should yield the same result as we use an opaque band


@pytest.mark.require_driver("AAIGRID")
def test_sieve_5():

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("data/sieve_src.grd")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("tmp/sieve_1.tif", 5, 7, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, dst_band.GetMaskBand(), dst_band, 2, 4)

    cs_expected = 364
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("tmp/sieve_1.tif")

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Performance test. When increasing the 'size' parameter, performance
# should stay roughly linear with the number of pixels (i.e. size^2)


def test_sieve_6():

    gdaltest.importorskip_gdal_array()
    numpy = pytest.importorskip("numpy")

    # Try 3002. Should run in less than 10 seconds
    # size = 3002
    size = 102
    ds = gdal.GetDriverByName("MEM").Create("", size + 1, size)
    ar = numpy.zeros((size, size + 1), dtype=numpy.uint8)
    for i in range(size):
        for j in range(int(size / 3)):
            ar[i][size + 1 - 1 - i - 1 - 3 * j] = 255
            ar[i][size + 1 - 1 - i - 3 * j] = 255
        ar[i][0] = 255
    ar[size - 1] = 255
    ds.GetRasterBand(1).WriteArray(ar)

    band = ds.GetRasterBand(1)

    gdal.SieveFilter(band, None, band, 2, 4)

    # ar = band.ReadAsArray()
    # print(ar)

    cs = band.Checksum()
    if (size == 102 and cs != 60955) or (size == 3002 and cs != 63178):
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Test with nodata


@pytest.mark.require_driver("AAIGRID")
def test_sieve_7():

    gdal.FileFromMemBuffer(
        "/vsimem/sieve_7.asc",
        """ncols        7
nrows        7
xllcorner    440720.000000000000
yllcorner    3750120.000000000000
cellsize     60.000000000000
NODATA_value 0
 0 0 0 0 0 0 0
 0 1 1 1 1 1 1
 0 1 0 0 1 1 1
 0 1 0 2 2 2 1
 0 1 1 2 1 2 1
 0 1 1 2 2 2 1
 0 1 1 1 1 1 1
 """,
    )

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("/vsimem/sieve_7.asc")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("/vsimem/sieve_7.tif", 7, 7, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, src_band.GetMaskBand(), dst_band, 4, 4)

    cs_expected = 42
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    gdal.Unlink("/vsimem/sieve_7.asc")

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("/vsimem/sieve_7.tif")

    # Expected:
    # [[0 0 0 0 0 0 0]
    # [0 1 1 1 1 1 1]
    # [0 1 0 0 1 1 1]
    # [0 1 0 2 2 2 1]
    # [0 1 1 2 2 2 1]
    # [0 1 1 2 2 2 1]
    # [0 1 1 1 1 1 1]]

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Test propagation in our search of biggest neighbour


@pytest.mark.require_driver("AAIGRID")
def test_sieve_8():

    gdal.FileFromMemBuffer(
        "/vsimem/sieve_8.asc",
        """ncols        7
nrows        7
xllcorner    440720.000000000000
yllcorner    3750120.000000000000
cellsize     60.000000000000
 0 0 0 0 0 0 0
 0 5 5 0 0 0 0
 0 5 2 3 4 0 0
 0 0 8 1 5 0 0
 0 0 7 6 5 9 0
 0 0 0 0 9 9 0
 0 0 0 0 0 0 0
 """,
    )

    drv = gdal.GetDriverByName("GTiff")
    src_ds = gdal.Open("/vsimem/sieve_8.asc")
    src_band = src_ds.GetRasterBand(1)

    dst_ds = drv.Create("/vsimem/sieve_8.tif", 7, 7, 1, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    gdal.SieveFilter(src_band, src_band.GetMaskBand(), dst_band, 4, 4)

    # All non 0 should be mapped to 0
    cs_expected = 0
    cs = dst_band.Checksum()

    dst_band = None
    dst_ds = None

    gdal.Unlink("/vsimem/sieve_8.asc")

    if cs == cs_expected or gdal.GetConfigOption("CPL_DEBUG", "OFF") != "ON":
        drv.Delete("/vsimem/sieve_8.tif")

    if cs != cs_expected:
        print("Got: ", cs)
        pytest.fail("got wrong checksum")


###############################################################################
# Test source bands with all masked pixels


def test_sieve_all_masked():

    drv = gdal.GetDriverByName("MEM")
    src_ds = drv.Create("", 10, 10, gdal.GDT_Byte)
    src_band = src_ds.GetRasterBand(1)
    src_band.Fill(1)

    mask_ds = drv.Create("", 10, 10, gdal.GDT_Byte)
    mask_band = mask_ds.GetRasterBand(1)

    dst_ds = drv.Create("", 10, 10, gdal.GDT_Byte)
    dst_band = dst_ds.GetRasterBand(1)

    expected_cs = src_band.Checksum()

    gdal.SieveFilter(src_band, mask_band, dst_band, 4, 4)

    assert dst_band.Checksum() == expected_cs

    gdal.SieveFilter(src_band, mask_band, src_band, 4, 4)

    assert src_band.Checksum() == expected_cs
