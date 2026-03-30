#!/usr/bin/env pytest
# -*- coding: utf-8 -*-
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test ISO-8211
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2026, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import os
import sys

import gdaltest
import pytest

# Or any other driver requiring iso8211 support
pytestmark = pytest.mark.require_driver("S57")


def test_iso8211(tmp_path):

    if sys.platform == "win32":
        ext_binary = ".exe"
    else:
        ext_binary = ""

    build_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
    dump_path = os.path.join(build_dir, "frmts", "iso8211", "8211dump" + ext_binary)
    if not os.path.exists(dump_path):
        pytest.skip("8211dump not found")

    ret = gdaltest.runexternal(
        f"{dump_path} -xml_all_details ../ogr/data/s57/1B5X02NE.000"
    )
    open(tmp_path / "out.xml", "wb").write(ret.encode("utf-8"))

    ref = open("../ogr/data/s57/1B5X02NE.000.xml", "rb").read().decode("utf-8")
    assert ret == ref

    create_from_xml_path = os.path.join(
        build_dir, "frmts", "iso8211", "8211createfromxml" + ext_binary
    )
    in_file = tmp_path / "out.xml"
    out_file = tmp_path / "out.000"
    gdaltest.runexternal(f"{create_from_xml_path} {in_file} {out_file}")

    ret2 = gdaltest.runexternal(f"{dump_path} -xml_all_details {out_file}")

    open(tmp_path / "out2.xml", "wb").write(ret2.encode("utf-8"))

    assert ret == ret2
