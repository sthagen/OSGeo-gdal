#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test SAR_CEOS driver.
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2009, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import gdaltest
import pytest

from osgeo import gdal

###############################################################################


@gdaltest.disable_exceptions()
def test_sar_ceos_app_1():
    tst = gdaltest.GDALTest(
        "SAR_CEOS", "data/sar_ceos/ottawa_patch.img", 1, -1, filename_absolute=1
    )
    tst.testOpen()


@gdaltest.disable_exceptions()
def test_sar_ceos_asf_2():
    tst = gdaltest.GDALTest(
        "SAR_CEOS", "data/sar_ceos/R1_26161_FN1_F164.D", 1, -1, filename_absolute=1
    )
    tst.testOpen()


@pytest.mark.network
@pytest.mark.require_curl
def test_sar_ceos_alos2_L1_1():
    url = "https://www.eorc.jaxa.jp/ALOS-2/en/doc/sampledata/stripmap3m/stripmap3m_L1.1_CEOS.zip"

    if gdaltest.gdalurlopen(url, timeout=5) is None:
        pytest.skip(f"Could not read from {url}")

    with gdaltest.error_raised(gdal.CE_None):
        ds = gdal.Open(
            f"/vsizip//vsicurl/{url}/ud2_L1.1_CEOS/IMG-HH-ALOS2403684200-200620-UBSL1.1__D"
        )

    assert ds.RasterXSize == 25600
    assert ds.RasterYSize == 32971
    assert ds.RasterCount == 1
    assert ds.GetRasterBand(1).DataType == gdal.GDT_CFloat32
    assert ds.GetMetadata() == {
        "CEOS_ACQUISITION_TIME": "20200620103907093",
        "CEOS_ELLIPSOID": "GRS80",
        "CEOS_FACILITY": "SCMO",
        "CEOS_INC_ANGLE": "35.631",
        "CEOS_LINE_SPACING_METERS": "1.4304222",
        "CEOS_LOGICAL_VOLUME_ID": "AL2SAR20140212",
        "CEOS_MISSION_ID": "ALOS2",
        "CEOS_ORBIT_NUMBER": "40368",
        "CEOS_PIXEL_SPACING_METERS": "2.1960598",
        "CEOS_PROCESSING_AGENCY": "JAXA",
        "CEOS_PROCESSING_COUNTRY": "JAPAN",
        "CEOS_PROCESSING_FACILITY": "SCMO",
        "CEOS_SEMI_MAJOR": "6378.1370000",
        "CEOS_SEMI_MINOR": "6356.7523141",
        "CEOS_SENSOR_CLOCK_ANGLE": "-90.000",
        "CEOS_SENSOR_ID": "ALOS2 -L -0115-",
        "CEOS_SOFTWARE_ID": "0.00",
        "CEOS_VOLSET_ID": "ALOS2  SAR",
    }
    assert ds.GetGCPCount() == 0


@pytest.mark.network
@pytest.mark.require_curl
def test_sar_ceos_alos2_L1_5():
    url = "https://www.eorc.jaxa.jp/ALOS/en/alos-2/datause/sample_data/010_fuji/0000021462_001001_ALOS2004060740-140620.zip"

    if gdaltest.gdalurlopen(url, timeout=5) is None:
        pytest.skip(f"Could not read from {url}")

    with gdaltest.error_raised(gdal.CE_None):
        ds = gdal.Open(
            f"/vsizip//vsicurl/{url}/IMG-HH-ALOS2004060740-140620-UBDL1.5GUA"
        )

    assert ds.RasterXSize == 27233
    assert ds.RasterYSize == 32294
    assert ds.RasterCount == 1
    assert ds.GetRasterBand(1).DataType == gdal.GDT_UInt16
    assert ds.GetMetadata() == {
        "CEOS_LOGICAL_VOLUME_ID": "AL2SAR20150316",
        "CEOS_PROCESSING_FACILITY": "EICS",
        "CEOS_PROCESSING_AGENCY": "JAXA",
        "CEOS_PROCESSING_COUNTRY": "JAPAN",
        "CEOS_SOFTWARE_ID": "002.011",
        "CEOS_VOLSET_ID": "ALOS2  SAR",
        "CEOS_ACQUISITION_TIME": "20140620135638128",
        "CEOS_TRUE_HEADING": "-16.4281130",
        "CEOS_ELLIPSOID": "GRS80",
        "CEOS_SEMI_MAJOR": "6378.1370000",
        "CEOS_SEMI_MINOR": "6356.7523141",
        "CEOS_MISSION_ID": "ALOS2",
        "CEOS_SENSOR_ID": "ALOS2 -L -0115-",
        "CEOS_ORBIT_NUMBER": "406",
        "CEOS_PLATFORM_LATITUDE": "36.752",
        "CEOS_PLATFORM_LONGITUDE": "144.846",
        "CEOS_PLATFORM_HEADING": "-4.505",
        "CEOS_SENSOR_CLOCK_ANGLE": "-90.000",
        "CEOS_INC_ANGLE": "45.765",
        "CEOS_FACILITY": "EICS",
        "CEOS_LINE_SPACING_METERS": "2.5000000",
        "CEOS_PIXEL_SPACING_METERS": "2.5000000",
    }
    assert ds.GetGCPCount() == 15


@pytest.mark.network
@pytest.mark.require_curl
def test_sar_ceos_alos4_L1_1():
    url = "https://www.eorc.jaxa.jp/ALOS/en/alos-4/sample_product/2025_May/Tokyo/L11/ALOS40182880250307UWD-RD0109_1.1__-.zip"

    if gdaltest.gdalurlopen(url, timeout=5) is None:
        pytest.skip(f"Could not read from {url}")

    with gdaltest.error_raised(gdal.CE_None):
        ds = gdal.Open(
            f"/vsizip//vsicurl/{url}/ALOS40182880250307UWD-RD0109_1.1__-/ALOS40182880250307UWD-RD0109_1.1__-/IMG-HH-ALOS40182880250307UWD-RD0109-1.1__-"
        )

    assert ds.RasterXSize == 19652
    assert ds.RasterYSize == 40633
    assert ds.RasterCount == 1
    assert ds.GetRasterBand(1).DataType == gdal.GDT_CFloat32
    assert ds.GetMetadata() == {
        "CEOS_LOGICAL_VOLUME_ID": "AL4SAR20250509",
        "CEOS_PROCESSING_FACILITY": "EICS",
        "CEOS_PROCESSING_AGENCY": "JAXA",
        "CEOS_PROCESSING_COUNTRY": "JAPAN",
        "CEOS_SOFTWARE_ID": "100.101",
        "CEOS_VOLSET_ID": "ALOS4  SAR",
        "CEOS_ACQUISITION_TIME": "20250307024249121",
        "CEOS_ELLIPSOID": "GRS80",
        "CEOS_SEMI_MAJOR": "6378.1370000",
        "CEOS_SEMI_MINOR": "6356.7523141",
        "CEOS_MISSION_ID": "ALOS4",
        "CEOS_SENSOR_ID": "ALOS4 -L -0215",
        "CEOS_ORBIT_NUMBER": "3682",
        "CEOS_SENSOR_CLOCK_ANGLE": "90.000",
        "CEOS_INC_ANGLE": "42.502",
        "CEOS_FACILITY": "EICS",
    }
    assert ds.GetGCPCount() == 0
