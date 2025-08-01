/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Implements Arc/Info ASCII Grid Format.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2001, Frank Warmerdam (warmerdam@pobox.com)
 * Copyright (c) 2007-2012, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2014, Kyle Shannon <kyle at pobox dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

// We need cpl_port as first include to avoid VSIStatBufL being not
// defined on i586-mingw32msvc.
#include "cpl_port.h"
#include "aaigriddataset.h"
#include "gdal_frmts.h"

#include <cassert>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <limits>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_pam.h"
#include "gdal_priv.h"
#include "ogr_core.h"
#include "ogr_spatialref.h"

namespace
{

float DoubleToFloatClamp(double dfValue)
{
    if (dfValue <= std::numeric_limits<float>::lowest())
        return std::numeric_limits<float>::lowest();
    if (dfValue >= std::numeric_limits<float>::max())
        return std::numeric_limits<float>::max();
    return static_cast<float>(dfValue);
}

// Cast to float and back for make sure the NoData value matches
// that expressed by a float value.  Clamps to the range of a float
// if the value is too large.  Preserves +/-inf and NaN.
// TODO(schwehr): This should probably be moved to port as it is likely
// to be needed for other formats.
double MapNoDataToFloat(double dfNoDataValue)
{
    if (std::isinf(dfNoDataValue) || std::isnan(dfNoDataValue))
        return dfNoDataValue;

    if (dfNoDataValue >= std::numeric_limits<float>::max())
        return std::numeric_limits<float>::max();

    if (dfNoDataValue <= -std::numeric_limits<float>::max())
        return -std::numeric_limits<float>::max();

    return static_cast<double>(static_cast<float>(dfNoDataValue));
}

}  // namespace

static CPLString OSR_GDS(char **papszNV, const char *pszField,
                         const char *pszDefaultValue);

/************************************************************************/
/*                           AAIGRasterBand()                           */
/************************************************************************/

AAIGRasterBand::AAIGRasterBand(AAIGDataset *poDSIn, int nDataStart)
    : panLineOffset(nullptr)
{
    poDS = poDSIn;

    nBand = 1;
    eDataType = poDSIn->eDataType;

    nBlockXSize = poDSIn->nRasterXSize;
    nBlockYSize = 1;

    panLineOffset = static_cast<GUIntBig *>(
        VSI_CALLOC_VERBOSE(poDSIn->nRasterYSize, sizeof(GUIntBig)));
    if (panLineOffset == nullptr)
    {
        return;
    }
    panLineOffset[0] = nDataStart;
}

/************************************************************************/
/*                          ~AAIGRasterBand()                           */
/************************************************************************/

AAIGRasterBand::~AAIGRasterBand()
{
    CPLFree(panLineOffset);
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr AAIGRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)

{
    AAIGDataset *poODS = static_cast<AAIGDataset *>(poDS);

    if (nBlockYOff < 0 || nBlockYOff > poODS->nRasterYSize - 1 ||
        nBlockXOff != 0 || panLineOffset == nullptr || poODS->fp == nullptr)
        return CE_Failure;

    if (panLineOffset[nBlockYOff] == 0)
    {
        for (int iPrevLine = 1; iPrevLine <= nBlockYOff; iPrevLine++)
            if (panLineOffset[iPrevLine] == 0)
                IReadBlock(nBlockXOff, iPrevLine - 1, nullptr);
    }

    if (panLineOffset[nBlockYOff] == 0)
        return CE_Failure;

    if (poODS->Seek(panLineOffset[nBlockYOff]) != 0)
    {
        ReportError(CE_Failure, CPLE_FileIO,
                    "Can't seek to offset %lu in input file to read data.",
                    static_cast<long unsigned int>(panLineOffset[nBlockYOff]));
        return CE_Failure;
    }

    for (int iPixel = 0; iPixel < poODS->nRasterXSize;)
    {
        // Suck up any pre-white space.
        char chNext = '\0';
        do
        {
            chNext = poODS->Getc();
        } while (isspace(static_cast<unsigned char>(chNext)));

        char szToken[500] = {'\0'};
        int iTokenChar = 0;
        while (chNext != '\0' && !isspace(static_cast<unsigned char>(chNext)))
        {
            if (iTokenChar == sizeof(szToken) - 2)
            {
                ReportError(CE_Failure, CPLE_FileIO,
                            "Token too long at scanline %d.", nBlockYOff);
                return CE_Failure;
            }

            szToken[iTokenChar++] = chNext;
            chNext = poODS->Getc();
        }

        if (chNext == '\0' && (iPixel != poODS->nRasterXSize - 1 ||
                               nBlockYOff != poODS->nRasterYSize - 1))
        {
            ReportError(CE_Failure, CPLE_FileIO,
                        "File short, can't read line %d.", nBlockYOff);
            return CE_Failure;
        }

        szToken[iTokenChar] = '\0';

        if (pImage != nullptr)
        {
            // "null" seems to be specific of D12 software
            // See https://github.com/OSGeo/gdal/issues/5095
            if (eDataType == GDT_Float64)
            {
                if (strcmp(szToken, "null") == 0)
                    reinterpret_cast<double *>(pImage)[iPixel] =
                        -std::numeric_limits<double>::max();
                else
                    reinterpret_cast<double *>(pImage)[iPixel] =
                        CPLAtofM(szToken);
            }
            else if (eDataType == GDT_Float32)
            {
                if (strcmp(szToken, "null") == 0)
                    reinterpret_cast<float *>(pImage)[iPixel] =
                        -std::numeric_limits<float>::max();
                else
                    reinterpret_cast<float *>(pImage)[iPixel] =
                        DoubleToFloatClamp(CPLAtofM(szToken));
            }
            else
                reinterpret_cast<GInt32 *>(pImage)[iPixel] =
                    static_cast<GInt32>(atoi(szToken));
        }

        iPixel++;
    }

    if (nBlockYOff < poODS->nRasterYSize - 1)
        panLineOffset[nBlockYOff + 1] = poODS->Tell();

    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double AAIGRasterBand::GetNoDataValue(int *pbSuccess)

{
    AAIGDataset *poODS = static_cast<AAIGDataset *>(poDS);

    if (pbSuccess)
        *pbSuccess = poODS->bNoDataSet;

    return poODS->dfNoDataValue;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr AAIGRasterBand::SetNoDataValue(double dfNoData)

{
    AAIGDataset *poODS = static_cast<AAIGDataset *>(poDS);

    poODS->bNoDataSet = true;
    poODS->dfNoDataValue = dfNoData;

    return CE_None;
}

/************************************************************************/
/* ==================================================================== */
/*                            AAIGDataset                               */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            AAIGDataset()                            */
/************************************************************************/

AAIGDataset::AAIGDataset()
    : fp(nullptr), papszPrj(nullptr), nBufferOffset(0), nOffsetInBuffer(256),
      eDataType(GDT_Int32), bNoDataSet(false), dfNoDataValue(-9999.0)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    memset(achReadBuf, 0, sizeof(achReadBuf));
}

/************************************************************************/
/*                           ~AAIGDataset()                            */
/************************************************************************/

AAIGDataset::~AAIGDataset()

{
    FlushCache(true);

    if (fp != nullptr)
    {
        if (VSIFCloseL(fp) != 0)
        {
            ReportError(CE_Failure, CPLE_FileIO, "I/O error");
        }
    }

    CSLDestroy(papszPrj);
}

/************************************************************************/
/*                                Tell()                                */
/************************************************************************/

GUIntBig AAIGDataset::Tell() const
{
    return nBufferOffset + nOffsetInBuffer;
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int AAIGDataset::Seek(GUIntBig nNewOffset)

{
    nOffsetInBuffer = sizeof(achReadBuf);
    return VSIFSeekL(fp, nNewOffset, SEEK_SET);
}

/************************************************************************/
/*                                Getc()                                */
/*                                                                      */
/*      Read a single character from the input file (efficiently we     */
/*      hope).                                                          */
/************************************************************************/

char AAIGDataset::Getc()

{
    if (nOffsetInBuffer < static_cast<int>(sizeof(achReadBuf)))
        return achReadBuf[nOffsetInBuffer++];

    nBufferOffset = VSIFTellL(fp);
    const int nRead =
        static_cast<int>(VSIFReadL(achReadBuf, 1, sizeof(achReadBuf), fp));
    for (unsigned int i = nRead; i < sizeof(achReadBuf); i++)
        achReadBuf[i] = '\0';

    nOffsetInBuffer = 0;

    return achReadBuf[nOffsetInBuffer++];
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **AAIGDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    if (papszPrj != nullptr)
        papszFileList = CSLAddString(papszFileList, osPrjFilename);

    return papszFileList;
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int AAIGDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    // Does this look like an AI grid file?
    if (poOpenInfo->nHeaderBytes < 40 ||
        !(STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "ncols") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "nrows") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "xllcorner") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "yllcorner") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "xllcenter") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "yllcenter") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "dx") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "dy") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "cellsize")))
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int GRASSASCIIDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    // Does this look like a GRASS ASCII grid file?
    if (poOpenInfo->nHeaderBytes < 40 ||
        !(STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "north:") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "south:") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "east:") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "west:") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "rows:") ||
          STARTS_WITH_CI(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                         "cols:")))
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int ISGDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    // Does this look like a ISG grid file?
    if (poOpenInfo->nHeaderBytes < 40 ||
        !strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                "model name"))
    {
        return FALSE;
    }
    for (int i = 0; i < 2; ++i)
    {
        if (strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "lat min") != nullptr &&
            strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "lat max") != nullptr &&
            strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "lon min") != nullptr &&
            strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "lon max") != nullptr &&
            strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "nrows") != nullptr &&
            strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "ncols") != nullptr)
        {
            return TRUE;
        }
        // Some files like https://isgeoid.polimi.it/Geoid/Europe/Slovenia/public/Slovenia_2016_SLO_VRP2016_Koper_hybrQ_20221122.isg
        // have initial comment lines, so we may need to ingest more bytes
        if (i == 0)
        {
            if (poOpenInfo->nHeaderBytes >= 8192)
                break;
            poOpenInfo->TryToIngest(8192);
        }
    }

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *AAIGDataset::Open(GDALOpenInfo *poOpenInfo)
{
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    // During fuzzing, do not use Identify to reject crazy content.
    if (!Identify(poOpenInfo))
        return nullptr;
#endif

    return CommonOpen(poOpenInfo, FORMAT_AAIG);
}

/************************************************************************/
/*                          ParseHeader()                               */
/************************************************************************/

bool AAIGDataset::ParseHeader(const char *pszHeader, const char *pszDataType)
{
    const CPLStringList aosTokens(CSLTokenizeString2(pszHeader, " \n\r\t", 0));
    const int nTokens = aosTokens.size();

    int i = 0;
    if ((i = aosTokens.FindString("ncols")) < 0 || i + 1 >= nTokens)
    {
        return false;
    }
    nRasterXSize = atoi(aosTokens[i + 1]);
    if ((i = aosTokens.FindString("nrows")) < 0 || i + 1 >= nTokens)
    {
        return false;
    }
    nRasterYSize = atoi(aosTokens[i + 1]);

    if (!GDALCheckDatasetDimensions(nRasterXSize, nRasterYSize))
    {
        return false;
    }

    double dfCellDX = 0.0;
    double dfCellDY = 0.0;
    if ((i = aosTokens.FindString("cellsize")) < 0)
    {
        int iDX, iDY;
        if ((iDX = aosTokens.FindString("dx")) < 0 ||
            (iDY = aosTokens.FindString("dy")) < 0 || iDX + 1 >= nTokens ||
            iDY + 1 >= nTokens)
        {
            return false;
        }

        dfCellDX = CPLAtofM(aosTokens[iDX + 1]);
        dfCellDY = CPLAtofM(aosTokens[iDY + 1]);
    }
    else
    {
        if (i + 1 >= nTokens)
        {
            return false;
        }
        dfCellDY = CPLAtofM(aosTokens[i + 1]);
        dfCellDX = dfCellDY;
    }

    int j = 0;
    if ((i = aosTokens.FindString("xllcorner")) >= 0 &&
        (j = aosTokens.FindString("yllcorner")) >= 0 && i + 1 < nTokens &&
        j + 1 < nTokens)
    {
        m_gt[0] = CPLAtofM(aosTokens[i + 1]);

        // Small hack to compensate from insufficient precision in cellsize
        // parameter in datasets of
        // http://ccafs-climate.org/data/A2a_2020s/hccpr_hadcm3
        if ((nRasterXSize % 360) == 0 && fabs(m_gt[0] - (-180.0)) < 1e-12 &&
            dfCellDX == dfCellDY &&
            fabs(dfCellDX - (360.0 / nRasterXSize)) < 1e-9)
        {
            dfCellDY = 360.0 / nRasterXSize;
            dfCellDX = dfCellDY;
        }

        m_gt[1] = dfCellDX;
        m_gt[2] = 0.0;
        m_gt[3] = CPLAtofM(aosTokens[j + 1]) + nRasterYSize * dfCellDY;
        m_gt[4] = 0.0;
        m_gt[5] = -dfCellDY;
    }
    else if ((i = aosTokens.FindString("xllcenter")) >= 0 &&
             (j = aosTokens.FindString("yllcenter")) >= 0 && i + 1 < nTokens &&
             j + 1 < nTokens)
    {
        SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT);

        m_gt[0] = CPLAtofM(aosTokens[i + 1]) - 0.5 * dfCellDX;
        m_gt[1] = dfCellDX;
        m_gt[2] = 0.0;
        m_gt[3] = CPLAtofM(aosTokens[j + 1]) - 0.5 * dfCellDY +
                  nRasterYSize * dfCellDY;
        m_gt[4] = 0.0;
        m_gt[5] = -dfCellDY;
    }
    else
    {
        m_gt[0] = 0.0;
        m_gt[1] = dfCellDX;
        m_gt[2] = 0.0;
        m_gt[3] = 0.0;
        m_gt[4] = 0.0;
        m_gt[5] = -dfCellDY;
    }

    if ((i = aosTokens.FindString("NODATA_value")) >= 0 && i + 1 < nTokens)
    {
        const char *pszNoData = aosTokens[i + 1];

        bNoDataSet = true;
        if (strcmp(pszNoData, "null") == 0)
        {
            // "null" seems to be specific of D12 software
            // See https://github.com/OSGeo/gdal/issues/5095
            if (pszDataType == nullptr || eDataType == GDT_Float32)
            {
                dfNoDataValue = -std::numeric_limits<float>::max();
                eDataType = GDT_Float32;
            }
            else
            {
                dfNoDataValue = -std::numeric_limits<double>::max();
                eDataType = GDT_Float64;
            }
        }
        else
        {
            dfNoDataValue = CPLAtofM(pszNoData);
            if (pszDataType == nullptr &&
                (strchr(pszNoData, '.') != nullptr ||
                 strchr(pszNoData, ',') != nullptr ||
                 std::isnan(dfNoDataValue) ||
                 std::numeric_limits<int>::min() > dfNoDataValue ||
                 dfNoDataValue > std::numeric_limits<int>::max()))
            {
                eDataType = GDT_Float32;
                if (!std::isinf(dfNoDataValue) &&
                    (fabs(dfNoDataValue) < std::numeric_limits<float>::min() ||
                     fabs(dfNoDataValue) > std::numeric_limits<float>::max()))
                {
                    eDataType = GDT_Float64;
                }
            }
            if (eDataType == GDT_Float32)
            {
                dfNoDataValue = MapNoDataToFloat(dfNoDataValue);
            }
        }
    }

    return true;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *GRASSASCIIDataset::Open(GDALOpenInfo *poOpenInfo)
{
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    // During fuzzing, do not use Identify to reject crazy content.
    if (!Identify(poOpenInfo))
        return nullptr;
#endif

    return CommonOpen(poOpenInfo, FORMAT_GRASSASCII);
}

/************************************************************************/
/*                          ParseHeader()                               */
/************************************************************************/

bool GRASSASCIIDataset::ParseHeader(const char *pszHeader,
                                    const char *pszDataType)
{
    const CPLStringList aosTokens(CSLTokenizeString2(pszHeader, " \n\r\t:", 0));
    const int nTokens = aosTokens.size();
    int i = 0;
    if ((i = aosTokens.FindString("cols")) < 0 || i + 1 >= nTokens)
    {
        return false;
    }
    nRasterXSize = atoi(aosTokens[i + 1]);
    if ((i = aosTokens.FindString("rows")) < 0 || i + 1 >= nTokens)
    {
        return false;
    }
    nRasterYSize = atoi(aosTokens[i + 1]);

    if (!GDALCheckDatasetDimensions(nRasterXSize, nRasterYSize))
    {
        return false;
    }

    const int iNorth = aosTokens.FindString("north");
    const int iSouth = aosTokens.FindString("south");
    const int iEast = aosTokens.FindString("east");
    const int iWest = aosTokens.FindString("west");

    if (iNorth == -1 || iSouth == -1 || iEast == -1 || iWest == -1 ||
        std::max(std::max(iNorth, iSouth), std::max(iEast, iWest)) + 1 >=
            nTokens)
    {
        return false;
    }

    const double dfNorth = CPLAtofM(aosTokens[iNorth + 1]);
    const double dfSouth = CPLAtofM(aosTokens[iSouth + 1]);
    const double dfEast = CPLAtofM(aosTokens[iEast + 1]);
    const double dfWest = CPLAtofM(aosTokens[iWest + 1]);
    const double dfPixelXSize = (dfEast - dfWest) / nRasterXSize;
    const double dfPixelYSize = (dfNorth - dfSouth) / nRasterYSize;

    m_gt[0] = dfWest;
    m_gt[1] = dfPixelXSize;
    m_gt[2] = 0.0;
    m_gt[3] = dfNorth;
    m_gt[4] = 0.0;
    m_gt[5] = -dfPixelYSize;

    if ((i = aosTokens.FindString("null")) >= 0 && i + 1 < nTokens)
    {
        const char *pszNoData = aosTokens[i + 1];

        bNoDataSet = true;
        dfNoDataValue = CPLAtofM(pszNoData);
        if (pszDataType == nullptr &&
            (strchr(pszNoData, '.') != nullptr ||
             strchr(pszNoData, ',') != nullptr || std::isnan(dfNoDataValue) ||
             std::numeric_limits<int>::min() > dfNoDataValue ||
             dfNoDataValue > std::numeric_limits<int>::max()))
        {
            eDataType = GDT_Float32;
        }
        if (eDataType == GDT_Float32)
        {
            dfNoDataValue = MapNoDataToFloat(dfNoDataValue);
        }
    }

    if ((i = aosTokens.FindString("type")) >= 0 && i + 1 < nTokens)
    {
        const char *pszType = aosTokens[i + 1];
        if (EQUAL(pszType, "int"))
            eDataType = GDT_Int32;
        else if (EQUAL(pszType, "float"))
            eDataType = GDT_Float32;
        else if (EQUAL(pszType, "double"))
            eDataType = GDT_Float64;
        else
        {
            ReportError(CE_Warning, CPLE_AppDefined,
                        "Invalid value for type parameter : %s", pszType);
        }
    }

    return true;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *ISGDataset::Open(GDALOpenInfo *poOpenInfo)
{
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    // During fuzzing, do not use Identify to reject crazy content.
    if (!Identify(poOpenInfo))
        return nullptr;
#endif

    return CommonOpen(poOpenInfo, FORMAT_ISG);
}

/************************************************************************/
/*                          ParseHeader()                               */
/************************************************************************/

bool ISGDataset::ParseHeader(const char *pszHeader, const char *)
{
    // See https://www.isgeoid.polimi.it/Geoid/ISG_format_v10_20160121.pdf
    //     https://www.isgeoid.polimi.it/Geoid/ISG_format_v101_20180915.pdf
    //     https://www.isgeoid.polimi.it/Geoid/ISG_format_v20_20200625.pdf

    const CPLStringList aosLines(CSLTokenizeString2(pszHeader, "\n\r", 0));
    CPLString osLatMin;
    CPLString osLatMax;
    CPLString osLonMin;
    CPLString osLonMax;
    CPLString osDeltaLat;
    CPLString osDeltaLon;
    CPLString osRows;
    CPLString osCols;
    CPLString osNodata;
    std::string osISGFormat;
    std::string osDataFormat;    // ISG 2.0
    std::string osDataOrdering;  // ISG 2.0
    std::string osCoordType;     // ISG 2.0
    std::string osCoordUnits;    // ISG 2.0
    for (int iLine = 0; iLine < aosLines.size(); iLine++)
    {
        const CPLStringList aosTokens(
            CSLTokenizeString2(aosLines[iLine], ":=", 0));
        if (aosTokens.size() == 2)
        {
            const CPLString osLeft(CPLString(aosTokens[0]).Trim());
            CPLString osRight(CPLString(aosTokens[1]).Trim());
            if (osLeft == "lat min")
                osLatMin = std::move(osRight);
            else if (osLeft == "lat max")
                osLatMax = std::move(osRight);
            else if (osLeft == "lon min")
                osLonMin = std::move(osRight);
            else if (osLeft == "lon max")
                osLonMax = std::move(osRight);
            else if (osLeft == "delta lat")
                osDeltaLat = std::move(osRight);
            else if (osLeft == "delta lon")
                osDeltaLon = std::move(osRight);
            else if (osLeft == "nrows")
                osRows = std::move(osRight);
            else if (osLeft == "ncols")
                osCols = std::move(osRight);
            else if (osLeft == "nodata")
                osNodata = std::move(osRight);
            else if (osLeft == "model name")
                SetMetadataItem("MODEL_NAME", osRight);
            else if (osLeft == "model type")
                SetMetadataItem("MODEL_TYPE", osRight);
            else if (osLeft == "units" || osLeft == "data units")
                osUnits = std::move(osRight);
            else if (osLeft == "ISG format")
                osISGFormat = std::move(osRight);
            else if (osLeft == "data format")
                osDataFormat = std::move(osRight);
            else if (osLeft == "data ordering")
                osDataOrdering = std::move(osRight);
            else if (osLeft == "coord type")
                osCoordType = std::move(osRight);
            else if (osLeft == "coord units")
                osCoordUnits = std::move(osRight);
        }
    }
    const double dfVersion =
        osISGFormat.empty() ? 0.0 : CPLAtof(osISGFormat.c_str());
    if (osLatMin.empty() || osLatMax.empty() || osLonMin.empty() ||
        osLonMax.empty() || osDeltaLat.empty() || osDeltaLon.empty() ||
        osRows.empty() || osCols.empty())
    {
        return false;
    }
    if (!osDataFormat.empty() && osDataFormat != "grid")
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "ISG: data format = %s not supported", osDataFormat.c_str());
        return false;
    }
    if (!osDataOrdering.empty() && osDataOrdering != "N-to-S, W-to-E")
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "ISG: data ordering = %s not supported",
                 osDataOrdering.c_str());
        return false;
    }
    if (!osCoordType.empty() && osCoordType != "geodetic")
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "ISG: coord type = %s not supported", osCoordType.c_str());
        return false;
    }

    const auto parseDMS = [](CPLString &str)
    {
        const std::string degreeSymbol{"\xc2\xb0"};
        str.replaceAll(degreeSymbol, "D");
        return CPLDMSToDec(str);
    };

    bool useDMS = false;
    if (!osCoordUnits.empty())
    {
        if (osCoordUnits == "dms")
        {
            // CPLDMSToDec does not support the non ascii char for degree used in ISG.
            // just replace it with "D" to make it compatible.
            useDMS = true;
        }
        else if (osCoordUnits != "deg")
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "ISG: coord units = %s not supported",
                     osCoordUnits.c_str());
            return false;
        }
    }
    double dfLatMin = useDMS ? parseDMS(osLatMin) : CPLAtof(osLatMin);
    double dfLatMax = useDMS ? parseDMS(osLatMax) : CPLAtof(osLatMax);
    double dfLonMin = useDMS ? parseDMS(osLonMin) : CPLAtof(osLonMin);
    double dfLonMax = useDMS ? parseDMS(osLonMax) : CPLAtof(osLonMax);
    double dfDeltaLon = useDMS ? parseDMS(osDeltaLon) : CPLAtof(osDeltaLon);
    double dfDeltaLat = useDMS ? parseDMS(osDeltaLat) : CPLAtof(osDeltaLat);
    if (dfVersion >= 2.0)
    {
        dfLatMin -= dfDeltaLat / 2.0;
        dfLatMax += dfDeltaLat / 2.0;
        dfLonMin -= dfDeltaLon / 2.0;
        dfLonMax += dfDeltaLon / 2.0;
    }
    const int nRows = atoi(osRows);
    const int nCols = atoi(osCols);
    if (nRows <= 0 || nCols <= 0 ||
        !(dfDeltaLat > 0 && dfDeltaLon > 0 && dfDeltaLat < 180 &&
          dfDeltaLon < 360))
    {
        return false;
    }

    if (!GDALCheckDatasetDimensions(nRows, nCols))
    {
        return false;
    }

    // Correct rounding errors.

    const auto TryRoundTo = [](double &dfDelta, double dfRoundedDelta,
                               double &dfMin, double &dfMax, int nVals,
                               double dfRelTol)
    {
        double dfMinTry = dfMin;
        double dfMaxTry = dfMax;
        double dfDeltaTry = dfDelta;
        if (dfRoundedDelta != dfDelta &&
            fabs(fabs(dfMin / dfRoundedDelta) -
                 (floor(fabs(dfMin / dfRoundedDelta)) + 0.5)) < dfRelTol &&
            fabs(fabs(dfMax / dfRoundedDelta) -
                 (floor(fabs(dfMax / dfRoundedDelta)) + 0.5)) < dfRelTol)
        {
            {
                double dfVal = (floor(fabs(dfMin / dfRoundedDelta)) + 0.5) *
                               dfRoundedDelta;
                dfMinTry = (dfMin < 0) ? -dfVal : dfVal;
            }
            {
                double dfVal = (floor(fabs(dfMax / dfRoundedDelta)) + 0.5) *
                               dfRoundedDelta;
                dfMaxTry = (dfMax < 0) ? -dfVal : dfVal;
            }
            dfDeltaTry = dfRoundedDelta;
        }
        else if (dfRoundedDelta != dfDelta &&
                 fabs(fabs(dfMin / dfRoundedDelta) -
                      (floor(fabs(dfMin / dfRoundedDelta) + 0.5) + 0.)) <
                     dfRelTol &&
                 fabs(fabs(dfMax / dfRoundedDelta) -
                      (floor(fabs(dfMax / dfRoundedDelta) + 0.5) + 0.)) <
                     dfRelTol)
        {
            {
                double dfVal =
                    (floor(fabs(dfMin / dfRoundedDelta) + 0.5) + 0.) *
                    dfRoundedDelta;
                dfMinTry = (dfMin < 0) ? -dfVal : dfVal;
            }
            {
                double dfVal =
                    (floor(fabs(dfMax / dfRoundedDelta) + 0.5) + 0.) *
                    dfRoundedDelta;
                dfMaxTry = (dfMax < 0) ? -dfVal : dfVal;
            }
            dfDeltaTry = dfRoundedDelta;
        }
        if (fabs(dfMinTry + dfDeltaTry * nVals - dfMaxTry) <
            dfRelTol * dfDeltaTry)
        {
            dfMin = dfMinTry;
            dfMax = dfMaxTry;
            dfDelta = dfDeltaTry;
            return true;
        }
        return false;
    };

    const double dfRoundedDeltaLon =
        (osDeltaLon == "0.0167" ||
         (dfDeltaLon < 1 &&
          fabs(1. / dfDeltaLon - floor(1. / dfDeltaLon + 0.5)) < 0.06))
            ? 1. / floor(1. / dfDeltaLon + 0.5)
            : dfDeltaLon;

    const double dfRoundedDeltaLat =
        (osDeltaLat == "0.0167" ||
         (dfDeltaLat < 1 &&
          fabs(1. / dfDeltaLat - floor(1. / dfDeltaLat + 0.5)) < 0.06))
            ? 1. / floor(1. / dfDeltaLat + 0.5)
            : dfDeltaLat;

    bool bOK = TryRoundTo(dfDeltaLon, dfRoundedDeltaLon, dfLonMin, dfLonMax,
                          nCols, 1e-2) &&
               TryRoundTo(dfDeltaLat, dfRoundedDeltaLat, dfLatMin, dfLatMax,
                          nRows, 1e-2);
    if (!bOK && osDeltaLon == "0.0167" && osDeltaLat == "0.0167")
    {
        // For https://www.isgeoid.polimi.it/Geoid/America/Argentina/public/GEOIDEAR16_20160419.isg
        bOK =
            TryRoundTo(dfDeltaLon, 0.016667, dfLonMin, dfLonMax, nCols, 1e-1) &&
            TryRoundTo(dfDeltaLat, 0.016667, dfLatMin, dfLatMax, nRows, 1e-1);
    }
    if (!bOK)
    {
        // 0.005 is what would be needed for the above GEOIDEAR16_20160419.isg
        // file without the specific fine tuning done.
        if ((fabs((dfLonMax - dfLonMin) / nCols - dfDeltaLon) <
                 0.005 * dfDeltaLon &&
             fabs((dfLatMax - dfLatMin) / nRows - dfDeltaLat) <
                 0.005 * dfDeltaLat) ||
            CPLTestBool(
                CPLGetConfigOption("ISG_SKIP_GEOREF_CONSISTENCY_CHECK", "NO")))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Georeference might be slightly approximate due to "
                     "rounding of coordinates and resolution in file header.");
            dfDeltaLon = (dfLonMax - dfLonMin) / nCols;
            dfDeltaLat = (dfLatMax - dfLatMin) / nRows;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Inconsistent extent/resolution/raster dimension, or "
                     "rounding of coordinates and resolution in file header "
                     "higher than accepted. You may skip this consistency "
                     "check by setting the ISG_SKIP_GEOREF_CONSISTENCY_CHECK "
                     "configuration option to YES.");
            return false;
        }
    }
    nRasterXSize = nCols;
    nRasterYSize = nRows;
    m_gt[0] = dfLonMin;
    m_gt[1] = dfDeltaLon;
    m_gt[2] = 0.0;
    m_gt[3] = dfLatMax;
    m_gt[4] = 0.0;
    m_gt[5] = -dfDeltaLat;
    if (!osNodata.empty())
    {
        bNoDataSet = true;
        dfNoDataValue = MapNoDataToFloat(CPLAtof(osNodata));
    }
    return true;
}

/************************************************************************/
/*                           CommonOpen()                               */
/************************************************************************/

GDALDataset *AAIGDataset::CommonOpen(GDALOpenInfo *poOpenInfo,
                                     GridFormat eFormat)
{
    if (poOpenInfo->fpL == nullptr)
        return nullptr;

    // Create a corresponding GDALDataset.
    std::unique_ptr<AAIGDataset> poDS;

    if (eFormat == FORMAT_AAIG)
        poDS = std::make_unique<AAIGDataset>();
    else if (eFormat == FORMAT_GRASSASCII)
        poDS = std::make_unique<GRASSASCIIDataset>();
    else
    {
        poDS = std::make_unique<ISGDataset>();
        poDS->eDataType = GDT_Float32;
    }

    const char *pszDataTypeOption = eFormat == FORMAT_AAIG ? "AAIGRID_DATATYPE"
                                    : eFormat == FORMAT_GRASSASCII
                                        ? "GRASSASCIIGRID_DATATYPE"
                                        : nullptr;

    const char *pszDataType =
        pszDataTypeOption ? CPLGetConfigOption(pszDataTypeOption, nullptr)
                          : nullptr;
    if (pszDataType == nullptr)
    {
        pszDataType =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "DATATYPE");
    }
    if (pszDataType != nullptr)
    {
        poDS->eDataType = GDALGetDataTypeByName(pszDataType);
        if (!(poDS->eDataType == GDT_Int32 || poDS->eDataType == GDT_Float32 ||
              poDS->eDataType == GDT_Float64))
        {
            ReportError(poOpenInfo->pszFilename, CE_Warning, CPLE_NotSupported,
                        "Unsupported value for %s : %s", pszDataTypeOption,
                        pszDataType);
            poDS->eDataType = GDT_Int32;
            pszDataType = nullptr;
        }
    }

    // Parse the header.
    if (!poDS->ParseHeader(
            reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
            pszDataType))
    {
        return nullptr;
    }

    poDS->fp = poOpenInfo->fpL;
    poOpenInfo->fpL = nullptr;

    // Sanity check in particular to avoid allocating a too large
    // AAIGRasterBand::panLineOffset array
    if (poDS->nRasterXSize > 10 * 1000 * 1000 ||
        poDS->nRasterYSize > 10 * 1000 * 1000 ||
        static_cast<int64_t>(poDS->nRasterXSize) * poDS->nRasterYSize >
            1000 * 1000 * 1000)
    {
        // We need at least 2 bytes for each pixel: one for the character for
        // its value and one for the space separator
        constexpr int MIN_BYTE_COUNT_PER_PIXEL = 2;
        if (VSIFSeekL(poDS->fp, 0, SEEK_END) != 0 ||
            VSIFTellL(poDS->fp) <
                static_cast<vsi_l_offset>(poDS->nRasterXSize) *
                    poDS->nRasterYSize * MIN_BYTE_COUNT_PER_PIXEL)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Too large raster dimension %d x %d compared to file size "
                     "(%" PRIu64 " bytes)",
                     poDS->nRasterXSize, poDS->nRasterYSize,
                     static_cast<uint64_t>(VSIFTellL(poDS->fp)));
            return nullptr;
        }
        VSIFSeekL(poDS->fp, 0, SEEK_SET);
    }

    // Find the start of real data.
    int nStartOfData = 0;

    if (eFormat == FORMAT_ISG)
    {
        const char *pszEOH =
            strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                   "end_of_head");
        if (pszEOH == nullptr)
        {
            return nullptr;
        }
        for (int i = 0; pszEOH[i]; i++)
        {
            if (pszEOH[i] == '\n' || pszEOH[i] == '\r')
            {
                nStartOfData =
                    static_cast<int>(pszEOH - reinterpret_cast<const char *>(
                                                  poOpenInfo->pabyHeader)) +
                    i;
                break;
            }
        }
        if (nStartOfData == 0)
        {
            return nullptr;
        }
        if (poOpenInfo->pabyHeader[nStartOfData] == '\n' ||
            poOpenInfo->pabyHeader[nStartOfData] == '\r')
        {
            nStartOfData++;
        }

        poDS->m_oSRS.importFromWkt(SRS_WKT_WGS84_LAT_LONG);
    }
    else
    {
        for (int i = 2; true; i++)
        {
            if (poOpenInfo->pabyHeader[i] == '\0')
            {
                ReportError(poOpenInfo->pszFilename, CE_Failure,
                            CPLE_AppDefined,
                            "Couldn't find data values in ASCII Grid file.");
                return nullptr;
            }

            if (poOpenInfo->pabyHeader[i - 1] == '\n' ||
                poOpenInfo->pabyHeader[i - 2] == '\n' ||
                poOpenInfo->pabyHeader[i - 1] == '\r' ||
                poOpenInfo->pabyHeader[i - 2] == '\r')
            {
                if ((!isalpha(static_cast<unsigned char>(
                         poOpenInfo->pabyHeader[i])) ||
                     // null seems to be specific of D12 software
                     // See https://github.com/OSGeo/gdal/issues/5095
                     (i + 5 < poOpenInfo->nHeaderBytes &&
                      memcmp(poOpenInfo->pabyHeader + i, "null ", 5) == 0) ||
                     (i + 4 < poOpenInfo->nHeaderBytes &&
                      EQUALN(reinterpret_cast<const char *>(
                                 poOpenInfo->pabyHeader + i),
                             "nan ", 4))) &&
                    poOpenInfo->pabyHeader[i] != '\n' &&
                    poOpenInfo->pabyHeader[i] != '\r')
                {
                    nStartOfData = i;

                    // Beginning of real data found.
                    break;
                }
            }
        }
    }

    // Recognize the type of data.
    CPLAssert(nullptr != poDS->fp);

    if (pszDataType == nullptr && poDS->eDataType != GDT_Float32 &&
        poDS->eDataType != GDT_Float64)
    {
        // Allocate 100K chunk + 1 extra byte for NULL character.
        constexpr size_t nChunkSize = 1024 * 100;
        std::unique_ptr<GByte, VSIFreeReleaser> pabyChunk(static_cast<GByte *>(
            VSI_CALLOC_VERBOSE(nChunkSize + 1, sizeof(GByte))));
        if (pabyChunk == nullptr)
        {
            return nullptr;
        }
        (pabyChunk.get())[nChunkSize] = '\0';

        if (VSIFSeekL(poDS->fp, nStartOfData, SEEK_SET) < 0)
        {
            return nullptr;
        }

        // Scan for dot in subsequent chunks of data.
        while (!VSIFEofL(poDS->fp))
        {
            const size_t nLen =
                VSIFReadL(pabyChunk.get(), 1, nChunkSize, poDS->fp);

            for (size_t i = 0; i < nLen; i++)
            {
                const GByte ch = (pabyChunk.get())[i];
                if (ch == '.' || ch == ',' || ch == 'e' || ch == 'E')
                {
                    poDS->eDataType = GDT_Float32;
                    break;
                }
            }
        }
    }

    // Create band information objects.
    AAIGRasterBand *band = new AAIGRasterBand(poDS.get(), nStartOfData);
    poDS->SetBand(1, band);
    if (band->panLineOffset == nullptr)
    {
        return nullptr;
    }
    if (!poDS->osUnits.empty())
    {
        poDS->GetRasterBand(1)->SetUnitType(poDS->osUnits);
    }

    // Try to read projection file.
    const std::string osDirname = CPLGetPathSafe(poOpenInfo->pszFilename);
    const std::string osBasename = CPLGetBasenameSafe(poOpenInfo->pszFilename);

    poDS->osPrjFilename =
        CPLFormFilenameSafe(osDirname.c_str(), osBasename.c_str(), "prj");
    int nRet = 0;
    {
        VSIStatBufL sStatBuf;
        nRet = VSIStatL(poDS->osPrjFilename, &sStatBuf);
    }
    if (nRet != 0 && VSIIsCaseSensitiveFS(poDS->osPrjFilename))
    {
        poDS->osPrjFilename =
            CPLFormFilenameSafe(osDirname.c_str(), osBasename.c_str(), "PRJ");

        VSIStatBufL sStatBuf;
        nRet = VSIStatL(poDS->osPrjFilename, &sStatBuf);
    }

    if (nRet == 0)
    {
        poDS->papszPrj = CSLLoad(poDS->osPrjFilename);

        CPLDebug("AAIGrid", "Loaded SRS from %s", poDS->osPrjFilename.c_str());

        OGRSpatialReference oSRS;
        oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (oSRS.importFromESRI(poDS->papszPrj) == OGRERR_NONE)
        {
            // If geographic values are in seconds, we must transform.
            // Is there a code for minutes too?
            if (oSRS.IsGeographic() &&
                EQUAL(OSR_GDS(poDS->papszPrj, "Units", ""), "DS"))
            {
                poDS->m_gt[0] /= 3600.0;
                poDS->m_gt[1] /= 3600.0;
                poDS->m_gt[2] /= 3600.0;
                poDS->m_gt[3] /= 3600.0;
                poDS->m_gt[4] /= 3600.0;
                poDS->m_gt[5] /= 3600.0;
            }

            poDS->m_oSRS = std::move(oSRS);
        }
    }

    // Initialize any PAM information.
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    // Check for external overviews.
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename,
                                poOpenInfo->GetSiblingFiles());

    return poDS.release();
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr AAIGDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *AAIGDataset::GetSpatialRef() const
{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                          CreateCopy()                                */
/************************************************************************/

GDALDataset *AAIGDataset::CreateCopy(const char *pszFilename,
                                     GDALDataset *poSrcDS, int /* bStrict */,
                                     char **papszOptions,
                                     GDALProgressFunc pfnProgress,
                                     void *pProgressData)
{
    const int nBands = poSrcDS->GetRasterCount();
    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();

    // Some rudimentary checks.
    if (nBands != 1)
    {
        ReportError(pszFilename, CE_Failure, CPLE_NotSupported,
                    "AAIG driver doesn't support %d bands.  Must be 1 band.",
                    nBands);

        return nullptr;
    }

    if (!pfnProgress(0.0, nullptr, pProgressData))
        return nullptr;

    // Create the dataset.
    VSILFILE *fpImage = VSIFOpenL(pszFilename, "wt");
    if (fpImage == nullptr)
    {
        ReportError(pszFilename, CE_Failure, CPLE_OpenFailed,
                    "Unable to create file.");
        return nullptr;
    }

    // Write ASCII Grid file header.
    GDALGeoTransform gt;
    char szHeader[2000] = {};
    const char *pszForceCellsize =
        CSLFetchNameValue(papszOptions, "FORCE_CELLSIZE");

    poSrcDS->GetGeoTransform(gt);

    const double dfYLLCorner = gt[5] < 0 ? gt[3] + nYSize * gt[5] : gt[3];
    if (std::abs(gt[1] + gt[5]) < 0.0000001 ||
        std::abs(gt[1] - gt[5]) < 0.0000001 ||
        (pszForceCellsize && CPLTestBool(pszForceCellsize)))
    {
        CPLsnprintf(szHeader, sizeof(szHeader),
                    "ncols        %d\n"
                    "nrows        %d\n"
                    "xllcorner    %.12f\n"
                    "yllcorner    %.12f\n"
                    "cellsize     %.12f\n",
                    nXSize, nYSize, gt[0], dfYLLCorner, gt[1]);
    }
    else
    {
        if (pszForceCellsize == nullptr)
            ReportError(pszFilename, CE_Warning, CPLE_AppDefined,
                        "Producing a Golden Surfer style file with DX and DY "
                        "instead of CELLSIZE since the input pixels are "
                        "non-square.  Use the FORCE_CELLSIZE=TRUE creation "
                        "option to force use of DX for even though this will "
                        "be distorted.  Most ASCII Grid readers (ArcGIS "
                        "included) do not support the DX and DY parameters.");
        CPLsnprintf(szHeader, sizeof(szHeader),
                    "ncols        %d\n"
                    "nrows        %d\n"
                    "xllcorner    %.12f\n"
                    "yllcorner    %.12f\n"
                    "dx           %.12f\n"
                    "dy           %.12f\n",
                    nXSize, nYSize, gt[0], dfYLLCorner, gt[1], fabs(gt[5]));
    }

    // Builds the format string used for printing float values.
    char szFormatFloat[32] = {'\0'};
    strcpy(szFormatFloat, "%.20g");
    const char *pszDecimalPrecision =
        CSLFetchNameValue(papszOptions, "DECIMAL_PRECISION");
    const char *pszSignificantDigits =
        CSLFetchNameValue(papszOptions, "SIGNIFICANT_DIGITS");
    bool bIgnoreSigDigits = false;
    if (pszDecimalPrecision && pszSignificantDigits)
    {
        ReportError(pszFilename, CE_Warning, CPLE_AppDefined,
                    "Conflicting precision arguments, using DECIMAL_PRECISION");
        bIgnoreSigDigits = true;
    }
    int nPrecision;
    if (pszSignificantDigits && !bIgnoreSigDigits)
    {
        nPrecision = atoi(pszSignificantDigits);
        if (nPrecision >= 0)
            snprintf(szFormatFloat, sizeof(szFormatFloat), "%%.%dg",
                     nPrecision);
        CPLDebug("AAIGrid", "Setting precision format: %s", szFormatFloat);
    }
    else if (pszDecimalPrecision)
    {
        nPrecision = atoi(pszDecimalPrecision);
        if (nPrecision >= 0)
            snprintf(szFormatFloat, sizeof(szFormatFloat), "%%.%df",
                     nPrecision);
        CPLDebug("AAIGrid", "Setting precision format: %s", szFormatFloat);
    }

    // Handle nodata (optionally).
    GDALRasterBand *poBand = poSrcDS->GetRasterBand(1);
    const bool bReadAsInt = poBand->GetRasterDataType() == GDT_Byte ||
                            poBand->GetRasterDataType() == GDT_Int16 ||
                            poBand->GetRasterDataType() == GDT_UInt16 ||
                            poBand->GetRasterDataType() == GDT_Int32;

    // Write `nodata' value to header if it is exists in source dataset
    int bSuccess = FALSE;
    const double dfNoData = poBand->GetNoDataValue(&bSuccess);
    if (bSuccess)
    {
        snprintf(szHeader + strlen(szHeader),
                 sizeof(szHeader) - strlen(szHeader), "%s", "NODATA_value ");
        if (bReadAsInt)
            snprintf(szHeader + strlen(szHeader),
                     sizeof(szHeader) - strlen(szHeader), "%d",
                     static_cast<int>(dfNoData));
        else
            CPLsnprintf(szHeader + strlen(szHeader),
                        sizeof(szHeader) - strlen(szHeader), szFormatFloat,
                        dfNoData);
        snprintf(szHeader + strlen(szHeader),
                 sizeof(szHeader) - strlen(szHeader), "%s", "\n");
    }

    if (VSIFWriteL(szHeader, strlen(szHeader), 1, fpImage) != 1)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fpImage));
        return nullptr;
    }

    // Loop over image, copying image data.

    // Write scanlines to output file
    int *panScanline = bReadAsInt
                           ? static_cast<int *>(CPLMalloc(sizeof(int) * nXSize))
                           : nullptr;

    double *padfScanline =
        bReadAsInt ? nullptr
                   : static_cast<double *>(CPLMalloc(sizeof(double) * nXSize));

    CPLErr eErr = CE_None;

    bool bHasOutputDecimalDot = false;
    for (int iLine = 0; eErr == CE_None && iLine < nYSize; iLine++)
    {
        CPLString osBuf;
        const int iSrcLine = gt[5] < 0 ? iLine : nYSize - 1 - iLine;
        eErr = poBand->RasterIO(GF_Read, 0, iSrcLine, nXSize, 1,
                                bReadAsInt ? static_cast<void *>(panScanline)
                                           : static_cast<void *>(padfScanline),
                                nXSize, 1, bReadAsInt ? GDT_Int32 : GDT_Float64,
                                0, 0, nullptr);

        if (bReadAsInt)
        {
            for (int iPixel = 0; iPixel < nXSize; iPixel++)
            {
                snprintf(szHeader, sizeof(szHeader), "%d", panScanline[iPixel]);
                osBuf += szHeader;
                osBuf += ' ';
                if ((iPixel > 0 && (iPixel % 1024) == 0) ||
                    iPixel == nXSize - 1)
                {
                    if (VSIFWriteL(osBuf, static_cast<int>(osBuf.size()), 1,
                                   fpImage) != 1)
                    {
                        eErr = CE_Failure;
                        ReportError(pszFilename, CE_Failure, CPLE_AppDefined,
                                    "Write failed, disk full?");
                        break;
                    }
                    osBuf = "";
                }
            }
        }
        else
        {
            assert(padfScanline);

            for (int iPixel = 0; iPixel < nXSize; iPixel++)
            {
                CPLsnprintf(szHeader, sizeof(szHeader), szFormatFloat,
                            padfScanline[iPixel]);

                // Make sure that as least one value has a decimal point (#6060)
                if (!bHasOutputDecimalDot)
                {
                    if (strchr(szHeader, '.') || strchr(szHeader, 'e') ||
                        strchr(szHeader, 'E'))
                    {
                        bHasOutputDecimalDot = true;
                    }
                    else if (!std::isinf(padfScanline[iPixel]) &&
                             !std::isnan(padfScanline[iPixel]))
                    {
                        strcat(szHeader, ".0");
                        bHasOutputDecimalDot = true;
                    }
                }

                osBuf += szHeader;
                osBuf += ' ';
                if ((iPixel > 0 && (iPixel % 1024) == 0) ||
                    iPixel == nXSize - 1)
                {
                    if (VSIFWriteL(osBuf, static_cast<int>(osBuf.size()), 1,
                                   fpImage) != 1)
                    {
                        eErr = CE_Failure;
                        ReportError(pszFilename, CE_Failure, CPLE_AppDefined,
                                    "Write failed, disk full?");
                        break;
                    }
                    osBuf = "";
                }
            }
        }
        if (VSIFWriteL("\n", 1, 1, fpImage) != 1)
            eErr = CE_Failure;

        if (eErr == CE_None &&
            !pfnProgress((iLine + 1) / static_cast<double>(nYSize), nullptr,
                         pProgressData))
        {
            eErr = CE_Failure;
            ReportError(pszFilename, CE_Failure, CPLE_UserInterrupt,
                        "User terminated CreateCopy()");
        }
    }

    CPLFree(panScanline);
    CPLFree(padfScanline);
    if (VSIFCloseL(fpImage) != 0)
        eErr = CE_Failure;

    if (eErr != CE_None)
        return nullptr;

    // Try to write projection file.
    const char *pszOriginalProjection = poSrcDS->GetProjectionRef();
    if (!EQUAL(pszOriginalProjection, ""))
    {
        char *pszDirname = CPLStrdup(CPLGetPathSafe(pszFilename).c_str());
        char *pszBasename = CPLStrdup(CPLGetBasenameSafe(pszFilename).c_str());
        char *pszPrjFilename = CPLStrdup(
            CPLFormFilenameSafe(pszDirname, pszBasename, "prj").c_str());
        VSILFILE *fp = VSIFOpenL(pszPrjFilename, "wt");
        if (fp != nullptr)
        {
            OGRSpatialReference oSRS;
            oSRS.importFromWkt(pszOriginalProjection);
            oSRS.morphToESRI();
            char *pszESRIProjection = nullptr;
            oSRS.exportToWkt(&pszESRIProjection);
            CPL_IGNORE_RET_VAL(VSIFWriteL(pszESRIProjection, 1,
                                          strlen(pszESRIProjection), fp));

            CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
            CPLFree(pszESRIProjection);
        }
        else
        {
            ReportError(pszFilename, CE_Failure, CPLE_FileIO,
                        "Unable to create file %s.", pszPrjFilename);
        }
        CPLFree(pszDirname);
        CPLFree(pszBasename);
        CPLFree(pszPrjFilename);
    }

    // Re-open dataset, and copy any auxiliary pam information.

    // If writing to stdout, we can't reopen it, so return
    // a fake dataset to make the caller happy.
    CPLPushErrorHandler(CPLQuietErrorHandler);
    GDALPamDataset *poDS =
        cpl::down_cast<GDALPamDataset *>(GDALDataset::Open(pszFilename));
    CPLPopErrorHandler();
    if (poDS)
    {
        poDS->CloneInfo(poSrcDS, GCIF_PAM_DEFAULT);
        return poDS;
    }

    CPLErrorReset();

    AAIGDataset *poAAIG_DS = new AAIGDataset();
    poAAIG_DS->nRasterXSize = nXSize;
    poAAIG_DS->nRasterYSize = nYSize;
    poAAIG_DS->nBands = 1;
    poAAIG_DS->SetBand(1, new AAIGRasterBand(poAAIG_DS, 1));
    return poAAIG_DS;
}

/************************************************************************/
/*                              OSR_GDS()                               */
/************************************************************************/

static CPLString OSR_GDS(char **papszNV, const char *pszField,
                         const char *pszDefaultValue)

{
    if (papszNV == nullptr || papszNV[0] == nullptr)
        return pszDefaultValue;

    int iLine = 0;  // Used after for.
    for (; papszNV[iLine] != nullptr &&
           !EQUALN(papszNV[iLine], pszField, strlen(pszField));
         iLine++)
    {
    }

    if (papszNV[iLine] == nullptr)
        return pszDefaultValue;
    else
    {
        char **papszTokens = CSLTokenizeString(papszNV[iLine]);

        CPLString osResult;
        if (CSLCount(papszTokens) > 1)
            osResult = papszTokens[1];
        else
            osResult = pszDefaultValue;

        CSLDestroy(papszTokens);
        return osResult;
    }
}

/************************************************************************/
/*                        GDALRegister_AAIGrid()                        */
/************************************************************************/

void GDALRegister_AAIGrid()

{
    if (GDALGetDriverByName("AAIGrid") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("AAIGrid");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Arc/Info ASCII Grid");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC,
                              "drivers/raster/aaigrid.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "asc");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES,
                              "Byte UInt16 Int16 Int32 Float32");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>\n"
        "   <Option name='FORCE_CELLSIZE' type='boolean' description='Force "
        "use of CELLSIZE, default is FALSE.'/>\n"
        "   <Option name='DECIMAL_PRECISION' type='int' description='Number of "
        "decimal when writing floating-point numbers(%f).'/>\n"
        "   <Option name='SIGNIFICANT_DIGITS' type='int' description='Number "
        "of significant digits when writing floating-point numbers(%g).'/>\n"
        "</CreationOptionList>\n");
    poDriver->SetMetadataItem(GDAL_DMD_OPENOPTIONLIST,
                              "<OpenOptionList>\n"
                              "   <Option name='DATATYPE' type='string-select' "
                              "description='Data type to be used.'>\n"
                              "       <Value>Int32</Value>\n"
                              "       <Value>Float32</Value>\n"
                              "       <Value>Float64</Value>\n"
                              "   </Option>\n"
                              "</OpenOptionList>\n");

    poDriver->pfnOpen = AAIGDataset::Open;
    poDriver->pfnIdentify = AAIGDataset::Identify;
    poDriver->pfnCreateCopy = AAIGDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}

/************************************************************************/
/*                   GDALRegister_GRASSASCIIGrid()                      */
/************************************************************************/

void GDALRegister_GRASSASCIIGrid()

{
    if (GDALGetDriverByName("GRASSASCIIGrid") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("GRASSASCIIGrid");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "GRASS ASCII Grid");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC,
                              "drivers/raster/grassasciigrid.html");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = GRASSASCIIDataset::Open;
    poDriver->pfnIdentify = GRASSASCIIDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}

/************************************************************************/
/*                       GDALRegister_ISG()                             */
/************************************************************************/

void GDALRegister_ISG()

{
    if (GDALGetDriverByName("ISG") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("ISG");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "International Service for the Geoid");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/isg.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "isg");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = ISGDataset::Open;
    poDriver->pfnIdentify = ISGDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
