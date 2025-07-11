/******************************************************************************
 *
 * Project:  Horizontal Datum Formats
 * Purpose:  Implementation of NOAA/NADCON los/las datum shift format.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 * Financial Support: i-cubed (http://www.i-cubed.com)
 *
 ******************************************************************************
 * Copyright (c) 2010, Frank Warmerdam
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_srs_api.h"
#include "rawdataset.h"

#include <algorithm>

/**

NOAA .LOS/.LAS Datum Grid Shift Format

Also used for .geo file from https://geodesy.noaa.gov/GEOID/MEXICO97/

All values are little endian

Header
------

char[56] "NADCON EXTRACTED REGION" or
         "GEOID EXTRACTED REGION "
char[8]  "NADGRD  " or
         "GEOGRD  "
int32    grid width
int32    grid height
int32    z count (1)
float32  origin longitude
float32  grid cell width longitude
float32  origin latitude
float32  grid cell height latitude
float32  angle (0.0)

Data
----

int32   ? always 0
float32*gridwidth offset in arcseconds (or in metres for geoids)

Note that the record length is always gridwidth*4 + 4, and
even the header record is this length though it means some waste.

**/

/************************************************************************/
/* ==================================================================== */
/*                              LOSLASDataset                           */
/* ==================================================================== */
/************************************************************************/

class LOSLASDataset final : public RawDataset
{
    VSILFILE *fpImage;  // image data file.

    int nRecordLength;

    OGRSpatialReference m_oSRS{};
    GDALGeoTransform m_gt{};

    CPL_DISALLOW_COPY_ASSIGN(LOSLASDataset)

    CPLErr Close() override;

  public:
    LOSLASDataset();
    ~LOSLASDataset() override;

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return &m_oSRS;
    }

    static GDALDataset *Open(GDALOpenInfo *);
    static int Identify(GDALOpenInfo *);
};

/************************************************************************/
/* ==================================================================== */
/*                              LOSLASDataset                           */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                             LOSLASDataset()                          */
/************************************************************************/

LOSLASDataset::LOSLASDataset() : fpImage(nullptr), nRecordLength(0)
{
    m_oSRS.SetFromUserInput(SRS_WKT_WGS84_LAT_LONG);
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                            ~LOSLASDataset()                          */
/************************************************************************/

LOSLASDataset::~LOSLASDataset()

{
    LOSLASDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr LOSLASDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (LOSLASDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        if (fpImage)
        {
            if (VSIFCloseL(fpImage) != 0)
            {
                CPLError(CE_Failure, CPLE_FileIO, "I/O error");
                eErr = CE_Failure;
            }
        }

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int LOSLASDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    if (poOpenInfo->nHeaderBytes < 64)
        return FALSE;

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    const char *pszExt = poOpenInfo->osExtension.c_str();
    if (!EQUAL(pszExt, "las") && !EQUAL(pszExt, "los") && !EQUAL(pszExt, "geo"))
        return FALSE;
#endif

    if (!STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader) +
                            56,
                        "NADGRD") &&
        !STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader) +
                            56,
                        "GEOGRD"))
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *LOSLASDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!Identify(poOpenInfo) || poOpenInfo->fpL == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("LOSLAS");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<LOSLASDataset>();
    std::swap(poDS->fpImage, poOpenInfo->fpL);

    /* -------------------------------------------------------------------- */
    /*      Read the header.                                                */
    /* -------------------------------------------------------------------- */
    CPL_IGNORE_RET_VAL(VSIFSeekL(poDS->fpImage, 64, SEEK_SET));

    CPL_IGNORE_RET_VAL(VSIFReadL(&(poDS->nRasterXSize), 4, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&(poDS->nRasterYSize), 4, 1, poDS->fpImage));

    CPL_LSBPTR32(&(poDS->nRasterXSize));
    CPL_LSBPTR32(&(poDS->nRasterYSize));

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize) ||
        poDS->nRasterXSize > (INT_MAX - 4) / 4)
    {
        return nullptr;
    }

    CPL_IGNORE_RET_VAL(VSIFSeekL(poDS->fpImage, 76, SEEK_SET));

    float min_lon, min_lat, delta_lon, delta_lat;

    CPL_IGNORE_RET_VAL(VSIFReadL(&min_lon, 4, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&delta_lon, 4, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&min_lat, 4, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&delta_lat, 4, 1, poDS->fpImage));

    CPL_LSBPTR32(&min_lon);
    CPL_LSBPTR32(&delta_lon);
    CPL_LSBPTR32(&min_lat);
    CPL_LSBPTR32(&delta_lat);

    poDS->nRecordLength = poDS->nRasterXSize * 4 + 4;

    /* -------------------------------------------------------------------- */
    /*      Create band information object.                                 */
    /*                                                                      */
    /*      Note we are setting up to read from the last image record to    */
    /*      the first since the data comes with the southern most record    */
    /*      first, not the northernmost like we would want.                 */
    /* -------------------------------------------------------------------- */
    auto poBand = RawRasterBand::Create(
        poDS.get(), 1, poDS->fpImage,
        static_cast<vsi_l_offset>(poDS->nRasterYSize) * poDS->nRecordLength + 4,
        4, -1 * poDS->nRecordLength, GDT_Float32,
        RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
        RawRasterBand::OwnFP::NO);
    if (!poBand)
        return nullptr;
    poDS->SetBand(1, std::move(poBand));

    if (poOpenInfo->IsExtensionEqualToCI("las"))
    {
        poDS->GetRasterBand(1)->SetDescription("Latitude Offset (arc seconds)");
    }
    else if (poOpenInfo->IsExtensionEqualToCI("los"))
    {
        poDS->GetRasterBand(1)->SetDescription(
            "Longitude Offset (arc seconds)");
        poDS->GetRasterBand(1)->SetMetadataItem("positive_value", "west");
    }
    else if (poOpenInfo->IsExtensionEqualToCI("geo"))
    {
        poDS->GetRasterBand(1)->SetDescription("Geoid undulation (meters)");
    }

    /* -------------------------------------------------------------------- */
    /*      Setup georeferencing.                                           */
    /* -------------------------------------------------------------------- */
    poDS->m_gt[0] = min_lon - delta_lon * 0.5;
    poDS->m_gt[1] = delta_lon;
    poDS->m_gt[2] = 0.0;
    poDS->m_gt[3] = min_lat + (poDS->nRasterYSize - 0.5) * delta_lat;
    poDS->m_gt[4] = 0.0;
    poDS->m_gt[5] = -1.0 * delta_lat;

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    return poDS.release();
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr LOSLASDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                        GDALRegister_LOSLAS()                         */
/************************************************************************/

void GDALRegister_LOSLAS()

{
    if (GDALGetDriverByName("LOSLAS") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("LOSLAS");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "NADCON .los/.las Datum Grid Shift");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = LOSLASDataset::Open;
    poDriver->pfnIdentify = LOSLASDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
