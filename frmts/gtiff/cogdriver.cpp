/******************************************************************************
 *
 * Project:  COG Driver
 * Purpose:  Cloud optimized GeoTIFF write support.
 * Author:   Even Rouault <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2019, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"

#include "gdal_priv.h"
#include "gtiff.h"
#include "gt_overview.h"
#include "gdal_utils.h"
#include "gdalwarper.h"
#include "cogdriver.h"
#include "geotiff.h"

#include "tilematrixset.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

static bool gbHasLZW = false;

extern "C" CPL_DLL void GDALRegister_COG();

/************************************************************************/
/*                        HasZSTDCompression()                          */
/************************************************************************/

static bool HasZSTDCompression()
{
    TIFFCodec *codecs = TIFFGetConfiguredCODECs();
    bool bHasZSTD = false;
    for (TIFFCodec *c = codecs; c->name; ++c)
    {
        if (c->scheme == COMPRESSION_ZSTD)
        {
            bHasZSTD = true;
            break;
        }
    }
    _TIFFfree(codecs);
    return bHasZSTD;
}

/************************************************************************/
/*                           GetTmpFilename()                           */
/************************************************************************/

static CPLString GetTmpFilename(const char *pszFilename, const char *pszExt)
{
    const bool bSupportsRandomWrite =
        VSISupportsRandomWrite(pszFilename, false);
    CPLString osTmpFilename;
    if (!bSupportsRandomWrite ||
        CPLGetConfigOption("CPL_TMPDIR", nullptr) != nullptr)
    {
        osTmpFilename = CPLGenerateTempFilenameSafe(
            CPLGetBasenameSafe(pszFilename).c_str());
    }
    else
        osTmpFilename = pszFilename;
    osTmpFilename += '.';
    osTmpFilename += pszExt;
    VSIUnlink(osTmpFilename);
    return osTmpFilename;
}

/************************************************************************/
/*                             GetResampling()                          */
/************************************************************************/

static const char *GetResampling(GDALDataset *poSrcDS)
{
    return poSrcDS->GetRasterBand(1)->GetColorTable() ? "NEAREST" : "CUBIC";
}

/************************************************************************/
/*                             GetPredictor()                          */
/************************************************************************/
static const char *GetPredictor(GDALDataset *poSrcDS, const char *pszPredictor)
{
    if (pszPredictor == nullptr)
        return nullptr;

    if (EQUAL(pszPredictor, "YES") || EQUAL(pszPredictor, "ON") ||
        EQUAL(pszPredictor, "TRUE"))
    {
        if (GDALDataTypeIsFloating(
                poSrcDS->GetRasterBand(1)->GetRasterDataType()))
            return "3";
        else
            return "2";
    }
    else if (EQUAL(pszPredictor, "STANDARD") || EQUAL(pszPredictor, "2"))
    {
        return "2";
    }
    else if (EQUAL(pszPredictor, "FLOATING_POINT") || EQUAL(pszPredictor, "3"))
    {
        return "3";
    }
    return nullptr;
}

/************************************************************************/
/*                            COGGetTargetSRS()                         */
/************************************************************************/

static bool COGGetTargetSRS(const char *const *papszOptions,
                            CPLString &osTargetSRS,
                            std::unique_ptr<gdal::TileMatrixSet> &poTM)
{
    osTargetSRS = CSLFetchNameValueDef(papszOptions, "TARGET_SRS", "");
    CPLString osTilingScheme(
        CSLFetchNameValueDef(papszOptions, "TILING_SCHEME", "CUSTOM"));
    if (EQUAL(osTargetSRS, "") && EQUAL(osTilingScheme, "CUSTOM"))
        return false;

    if (!EQUAL(osTilingScheme, "CUSTOM"))
    {
        poTM = gdal::TileMatrixSet::parse(osTilingScheme);
        if (poTM == nullptr)
            return false;
        if (!poTM->haveAllLevelsSameTopLeft())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unsupported tiling scheme: not all zoom levels have same "
                     "top left corner");
            return false;
        }
        if (!poTM->haveAllLevelsSameTileSize())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unsupported tiling scheme: not all zoom levels have same "
                     "tile size");
            return false;
        }
        if (poTM->hasVariableMatrixWidth())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unsupported tiling scheme: some levels have variable "
                     "matrix width");
            return false;
        }
        if (!osTargetSRS.empty())
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Ignoring TARGET_SRS option");
        }
        osTargetSRS = poTM->crs();

        // "Normalize" SRS as AUTH:CODE
        OGRSpatialReference oTargetSRS;
        oTargetSRS.SetFromUserInput(
            osTargetSRS,
            OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
        const char *pszAuthCode = oTargetSRS.GetAuthorityCode(nullptr);
        const char *pszAuthName = oTargetSRS.GetAuthorityName(nullptr);
        if (pszAuthName && pszAuthCode)
        {
            osTargetSRS = pszAuthName;
            osTargetSRS += ':';
            osTargetSRS += pszAuthCode;
        }
    }

    return true;
}

// Used by gdalwarp
bool COGGetTargetSRS(const char *const *papszOptions, CPLString &osTargetSRS)
{
    std::unique_ptr<gdal::TileMatrixSet> poTM;
    return COGGetTargetSRS(papszOptions, osTargetSRS, poTM);
}

// Used by gdalwarp
std::string COGGetResampling(GDALDataset *poSrcDS,
                             const char *const *papszOptions)
{
    return CSLFetchNameValueDef(papszOptions, "WARP_RESAMPLING",
                                CSLFetchNameValueDef(papszOptions, "RESAMPLING",
                                                     GetResampling(poSrcDS)));
}

/************************************************************************/
/*                     COGGetWarpingCharacteristics()                   */
/************************************************************************/

static bool COGGetWarpingCharacteristics(
    GDALDataset *poSrcDS, const char *const *papszOptions,
    CPLString &osResampling, CPLString &osTargetSRS, int &nXSize, int &nYSize,
    double &dfMinX, double &dfMinY, double &dfMaxX, double &dfMaxY,
    double &dfRes, std::unique_ptr<gdal::TileMatrixSet> &poTM, int &nZoomLevel,
    int &nAlignedLevels)
{
    if (!COGGetTargetSRS(papszOptions, osTargetSRS, poTM))
        return false;

    CPLStringList aosTO;
    aosTO.SetNameValue("DST_SRS", osTargetSRS);
    void *hTransformArg = nullptr;

    OGRSpatialReference oTargetSRS;
    oTargetSRS.SetFromUserInput(
        osTargetSRS,
        OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
    const char *pszAuthCode = oTargetSRS.GetAuthorityCode(nullptr);
    const int nEPSGCode = pszAuthCode ? atoi(pszAuthCode) : 0;

    // Hack to compensate for GDALSuggestedWarpOutput2() failure (or not
    // ideal suggestion with PROJ 8) when reprojecting latitude = +/- 90 to
    // EPSG:3857.
    GDALGeoTransform srcGT;
    std::unique_ptr<GDALDataset> poTmpDS;
    if (nEPSGCode == 3857 && poSrcDS->GetGeoTransform(srcGT) == CE_None &&
        srcGT[2] == 0 && srcGT[4] == 0 && srcGT[5] < 0)
    {
        const auto poSrcSRS = poSrcDS->GetSpatialRef();
        if (poSrcSRS && poSrcSRS->IsGeographic() &&
            !poSrcSRS->IsDerivedGeographic())
        {
            double maxLat = srcGT[3];
            double minLat = srcGT[3] + poSrcDS->GetRasterYSize() * srcGT[5];
            // Corresponds to the latitude of below MAX_GM
            constexpr double MAX_LAT = 85.0511287798066;
            bool bModified = false;
            if (maxLat > MAX_LAT)
            {
                maxLat = MAX_LAT;
                bModified = true;
            }
            if (minLat < -MAX_LAT)
            {
                minLat = -MAX_LAT;
                bModified = true;
            }
            if (bModified)
            {
                CPLStringList aosOptions;
                aosOptions.AddString("-of");
                aosOptions.AddString("VRT");
                aosOptions.AddString("-projwin");
                aosOptions.AddString(CPLSPrintf("%.17g", srcGT[0]));
                aosOptions.AddString(CPLSPrintf("%.17g", maxLat));
                aosOptions.AddString(CPLSPrintf(
                    "%.17g", srcGT[0] + poSrcDS->GetRasterXSize() * srcGT[1]));
                aosOptions.AddString(CPLSPrintf("%.17g", minLat));
                auto psOptions =
                    GDALTranslateOptionsNew(aosOptions.List(), nullptr);
                poTmpDS.reset(GDALDataset::FromHandle(GDALTranslate(
                    "", GDALDataset::ToHandle(poSrcDS), psOptions, nullptr)));
                GDALTranslateOptionsFree(psOptions);
                if (poTmpDS)
                {
                    hTransformArg = GDALCreateGenImgProjTransformer2(
                        GDALDataset::FromHandle(poTmpDS.get()), nullptr,
                        aosTO.List());
                    if (hTransformArg == nullptr)
                    {
                        return false;
                    }
                }
            }
        }
    }
    if (hTransformArg == nullptr)
    {
        hTransformArg =
            GDALCreateGenImgProjTransformer2(poSrcDS, nullptr, aosTO.List());
        if (hTransformArg == nullptr)
        {
            return false;
        }
    }

    GDALTransformerInfo *psInfo =
        static_cast<GDALTransformerInfo *>(hTransformArg);
    double adfGeoTransform[6];
    double adfExtent[4];

    if (GDALSuggestedWarpOutput2(poTmpDS ? poTmpDS.get() : poSrcDS,
                                 psInfo->pfnTransform, hTransformArg,
                                 adfGeoTransform, &nXSize, &nYSize, adfExtent,
                                 0) != CE_None)
    {
        GDALDestroyGenImgProjTransformer(hTransformArg);
        return false;
    }

    GDALDestroyGenImgProjTransformer(hTransformArg);
    hTransformArg = nullptr;
    poTmpDS.reset();

    dfMinX = adfExtent[0];
    dfMinY = adfExtent[1];
    dfMaxX = adfExtent[2];
    dfMaxY = adfExtent[3];
    dfRes = adfGeoTransform[1];

    const CPLString osExtent(CSLFetchNameValueDef(papszOptions, "EXTENT", ""));
    const CPLString osRes(CSLFetchNameValueDef(papszOptions, "RES", ""));
    if (poTM)
    {
        if (!osExtent.empty())
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Ignoring EXTENT option");
        }
        if (!osRes.empty())
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Ignoring RES option");
        }
        const bool bInvertAxis =
            oTargetSRS.EPSGTreatsAsLatLong() != FALSE ||
            oTargetSRS.EPSGTreatsAsNorthingEasting() != FALSE;

        const auto &bbox = poTM->bbox();
        if (bbox.mCrs == poTM->crs())
        {
            if (dfMaxX <
                    (bInvertAxis ? bbox.mLowerCornerY : bbox.mLowerCornerX) ||
                dfMinX >
                    (bInvertAxis ? bbox.mUpperCornerY : bbox.mUpperCornerX) ||
                dfMaxY <
                    (bInvertAxis ? bbox.mLowerCornerX : bbox.mLowerCornerY) ||
                dfMinY >
                    (bInvertAxis ? bbox.mUpperCornerX : bbox.mUpperCornerY))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Raster extent completely outside of tile matrix set "
                         "bounding box");
                return false;
            }
        }

        const auto &tmList = poTM->tileMatrixList();
        const int nBlockSize = atoi(CSLFetchNameValueDef(
            papszOptions, "BLOCKSIZE", CPLSPrintf("%d", tmList[0].mTileWidth)));
        dfRes = 0.0;

        const char *pszZoomLevel =
            CSLFetchNameValue(papszOptions, "ZOOM_LEVEL");
        if (pszZoomLevel)
        {
            nZoomLevel = atoi(pszZoomLevel);
            if (nZoomLevel < 0 || nZoomLevel >= static_cast<int>(tmList.size()))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid zoom level: should be in [0,%d]",
                         static_cast<int>(tmList.size()) - 1);
                return false;
            }
        }
        else
        {
            double dfComputedRes = adfGeoTransform[1];
            double dfPrevRes = 0.0;
            for (; nZoomLevel < static_cast<int>(tmList.size()); nZoomLevel++)
            {
                dfRes = tmList[nZoomLevel].mResX * tmList[0].mTileWidth /
                        nBlockSize;
                if (dfComputedRes > dfRes ||
                    fabs(dfComputedRes - dfRes) / dfRes <= 1e-8)
                    break;
                dfPrevRes = dfRes;
            }
            if (nZoomLevel == static_cast<int>(tmList.size()))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Could not find an appropriate zoom level");
                return false;
            }

            if (nZoomLevel > 0 && fabs(dfComputedRes - dfRes) / dfRes > 1e-8)
            {
                const char *pszZoomLevelStrategy = CSLFetchNameValueDef(
                    papszOptions, "ZOOM_LEVEL_STRATEGY", "AUTO");
                if (EQUAL(pszZoomLevelStrategy, "LOWER"))
                {
                    nZoomLevel--;
                }
                else if (EQUAL(pszZoomLevelStrategy, "UPPER"))
                {
                    /* do nothing */
                }
                else
                {
                    if (dfPrevRes / dfComputedRes < dfComputedRes / dfRes)
                        nZoomLevel--;
                }
            }
        }
        CPLDebug("COG", "Using ZOOM_LEVEL %d", nZoomLevel);
        dfRes = tmList[nZoomLevel].mResX * tmList[0].mTileWidth / nBlockSize;

        const double dfOriX =
            bInvertAxis ? tmList[0].mTopLeftY : tmList[0].mTopLeftX;
        const double dfOriY =
            bInvertAxis ? tmList[0].mTopLeftX : tmList[0].mTopLeftY;
        const double dfTileExtent = dfRes * nBlockSize;
        constexpr double TOLERANCE_IN_PIXEL = 0.499;
        const double dfEps = TOLERANCE_IN_PIXEL * dfRes;
        int nTLTileX = static_cast<int>(
            std::floor((dfMinX - dfOriX + dfEps) / dfTileExtent));
        int nTLTileY = static_cast<int>(
            std::floor((dfOriY - dfMaxY + dfEps) / dfTileExtent));
        int nBRTileX = static_cast<int>(
            std::ceil((dfMaxX - dfOriX - dfEps) / dfTileExtent));
        int nBRTileY = static_cast<int>(
            std::ceil((dfOriY - dfMinY - dfEps) / dfTileExtent));

        nAlignedLevels =
            std::min(std::min(10, atoi(CSLFetchNameValueDef(
                                      papszOptions, "ALIGNED_LEVELS", "0"))),
                     nZoomLevel);
        int nAccDivisor = 1;
        for (int i = 0; i < nAlignedLevels - 1; i++)
        {
            const int nCurLevel = nZoomLevel - i;
            const double dfResRatio =
                tmList[nCurLevel - 1].mResX / tmList[nCurLevel].mResX;
            // Magical number that has a great number of divisors
            // For example if previous scale denom was 50K and current one
            // is 20K, then dfResRatio = 2.5 and dfScaledInvResRatio = 24
            // We must then simplify 60 / 24 as 5 / 2, and make sure to
            // align tile coordinates on multiple of the 5 numerator
            constexpr int MAGICAL = 60;
            const double dfScaledInvResRatio = MAGICAL / dfResRatio;
            if (dfScaledInvResRatio < 1 || dfScaledInvResRatio > 60 ||
                std::abs(std::round(dfScaledInvResRatio) -
                         dfScaledInvResRatio) > 1e-10)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unsupported ratio of resolution for "
                         "ALIGNED_LEVELS between zoom level %d and %d = %g",
                         nCurLevel - 1, nCurLevel, dfResRatio);
                return false;
            }
            const int nScaledInvResRatio =
                static_cast<int>(std::round(dfScaledInvResRatio));
            int nNumerator = 0;
            for (int nDivisor = nScaledInvResRatio; nDivisor >= 2; --nDivisor)
            {
                if ((MAGICAL % nDivisor) == 0 &&
                    (nScaledInvResRatio % nDivisor) == 0)
                {
                    nNumerator = MAGICAL / nDivisor;
                    break;
                }
            }
            if (nNumerator == 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unsupported ratio of resolution for "
                         "ALIGNED_LEVELS between zoom level %d and %d = %g",
                         nCurLevel - 1, nCurLevel, dfResRatio);
                return false;
            }
            nAccDivisor *= nNumerator;
        }
        if (nAccDivisor > 1)
        {
            nTLTileX = (nTLTileX / nAccDivisor) * nAccDivisor;
            nTLTileY = (nTLTileY / nAccDivisor) * nAccDivisor;
            nBRTileY = DIV_ROUND_UP(nBRTileY, nAccDivisor) * nAccDivisor;
            nBRTileX = DIV_ROUND_UP(nBRTileX, nAccDivisor) * nAccDivisor;
        }

        if (nTLTileX < 0 || nTLTileY < 0 ||
            nBRTileX > tmList[nZoomLevel].mMatrixWidth ||
            nBRTileY > tmList[nZoomLevel].mMatrixHeight)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Raster extent partially outside of tile matrix "
                     "bounding box. Clamping it to it");
        }
        nTLTileX = std::max(0, nTLTileX);
        nTLTileY = std::max(0, nTLTileY);
        nBRTileX = std::min(tmList[nZoomLevel].mMatrixWidth, nBRTileX);
        nBRTileY = std::min(tmList[nZoomLevel].mMatrixHeight, nBRTileY);

        dfMinX = dfOriX + nTLTileX * dfTileExtent;
        dfMinY = dfOriY - nBRTileY * dfTileExtent;
        dfMaxX = dfOriX + nBRTileX * dfTileExtent;
        dfMaxY = dfOriY - nTLTileY * dfTileExtent;
    }
    else if (!osExtent.empty() || !osRes.empty())
    {
        CPLStringList aosTokens(CSLTokenizeString2(osExtent, ",", 0));
        if (aosTokens.size() != 4)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid value for EXTENT");
            return false;
        }
        dfMinX = CPLAtof(aosTokens[0]);
        dfMinY = CPLAtof(aosTokens[1]);
        dfMaxX = CPLAtof(aosTokens[2]);
        dfMaxY = CPLAtof(aosTokens[3]);
        if (!osRes.empty())
            dfRes = CPLAtof(osRes);
    }

    nXSize = static_cast<int>(std::round((dfMaxX - dfMinX) / dfRes));
    nYSize = static_cast<int>(std::round((dfMaxY - dfMinY) / dfRes));

    osResampling = COGGetResampling(poSrcDS, papszOptions);

    return true;
}

// Used by gdalwarp
bool COGGetWarpingCharacteristics(GDALDataset *poSrcDS,
                                  const char *const *papszOptions,
                                  CPLString &osResampling,
                                  CPLString &osTargetSRS, int &nXSize,
                                  int &nYSize, double &dfMinX, double &dfMinY,
                                  double &dfMaxX, double &dfMaxY)
{
    std::unique_ptr<gdal::TileMatrixSet> poTM;
    int nZoomLevel = 0;
    int nAlignedLevels = 0;
    double dfRes;
    return COGGetWarpingCharacteristics(poSrcDS, papszOptions, osResampling,
                                        osTargetSRS, nXSize, nYSize, dfMinX,
                                        dfMinY, dfMaxX, dfMaxY, dfRes, poTM,
                                        nZoomLevel, nAlignedLevels);
}

/************************************************************************/
/*                        COGHasWarpingOptions()                        */
/************************************************************************/

bool COGHasWarpingOptions(CSLConstList papszOptions)
{
    return CSLFetchNameValue(papszOptions, "TARGET_SRS") != nullptr ||
           !EQUAL(CSLFetchNameValueDef(papszOptions, "TILING_SCHEME", "CUSTOM"),
                  "CUSTOM");
}

/************************************************************************/
/*                      COGRemoveWarpingOptions()                       */
/************************************************************************/

void COGRemoveWarpingOptions(CPLStringList &aosOptions)
{
    aosOptions.SetNameValue("TARGET_SRS", nullptr);
    aosOptions.SetNameValue("TILING_SCHEME", nullptr);
    aosOptions.SetNameValue("EXTENT", nullptr);
    aosOptions.SetNameValue("RES", nullptr);
    aosOptions.SetNameValue("ALIGNED_LEVELS", nullptr);
    aosOptions.SetNameValue("ZOOM_LEVEL_STRATEGY", nullptr);
}

/************************************************************************/
/*                        CreateReprojectedDS()                         */
/************************************************************************/

static std::unique_ptr<GDALDataset> CreateReprojectedDS(
    const char *pszDstFilename, GDALDataset *poSrcDS,
    const char *const *papszOptions, const CPLString &osResampling,
    const CPLString &osTargetSRS, const int nXSize, const int nYSize,
    const double dfMinX, const double dfMinY, const double dfMaxX,
    const double dfMaxY, const double dfRes, GDALProgressFunc pfnProgress,
    void *pProgressData, double &dfCurPixels, double &dfTotalPixelsToProcess)
{
    char **papszArg = nullptr;
    // We could have done a warped VRT, but overview building on it might be
    // slow, so materialize as GTiff
    papszArg = CSLAddString(papszArg, "-of");
    papszArg = CSLAddString(papszArg, "GTiff");
    papszArg = CSLAddString(papszArg, "-co");
    papszArg = CSLAddString(papszArg, "TILED=YES");
    papszArg = CSLAddString(papszArg, "-co");
    papszArg = CSLAddString(papszArg, "SPARSE_OK=YES");
    const char *pszBIGTIFF = CSLFetchNameValue(papszOptions, "BIGTIFF");
    if (pszBIGTIFF)
    {
        papszArg = CSLAddString(papszArg, "-co");
        papszArg = CSLAddString(papszArg,
                                (CPLString("BIGTIFF=") + pszBIGTIFF).c_str());
    }
    papszArg = CSLAddString(papszArg, "-co");
    papszArg = CSLAddString(papszArg, HasZSTDCompression() ? "COMPRESS=ZSTD"
                                                           : "COMPRESS=LZW");
    papszArg = CSLAddString(papszArg, "-t_srs");
    papszArg = CSLAddString(papszArg, osTargetSRS);
    papszArg = CSLAddString(papszArg, "-te");
    papszArg = CSLAddString(papszArg, CPLSPrintf("%.17g", dfMinX));
    papszArg = CSLAddString(papszArg, CPLSPrintf("%.17g", dfMinY));
    papszArg = CSLAddString(papszArg, CPLSPrintf("%.17g", dfMaxX));
    papszArg = CSLAddString(papszArg, CPLSPrintf("%.17g", dfMaxY));
    papszArg = CSLAddString(papszArg, "-ts");
    papszArg = CSLAddString(papszArg, CPLSPrintf("%d", nXSize));
    papszArg = CSLAddString(papszArg, CPLSPrintf("%d", nYSize));

    // to be kept in sync with gdalwarp_lib.cpp
    constexpr double RELATIVE_ERROR_RES_SHARED_BY_COG_AND_GDALWARP = 1e-8;
    if (fabs((dfMaxX - dfMinX) / dfRes - nXSize) <=
            RELATIVE_ERROR_RES_SHARED_BY_COG_AND_GDALWARP &&
        fabs((dfMaxY - dfMinY) / dfRes - nYSize) <=
            RELATIVE_ERROR_RES_SHARED_BY_COG_AND_GDALWARP)
    {
        // Try to produce exactly square pixels
        papszArg = CSLAddString(papszArg, "-tr");
        papszArg = CSLAddString(papszArg, CPLSPrintf("%.17g", dfRes));
        papszArg = CSLAddString(papszArg, CPLSPrintf("%.17g", dfRes));
    }
    else
    {
        CPLDebug("COG", "Cannot pass -tr option to GDALWarp() due to extent, "
                        "size and resolution not consistent enough");
    }

    int bHasNoData = FALSE;
    poSrcDS->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    if (!bHasNoData &&
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "ADD_ALPHA", "YES")))
    {
        papszArg = CSLAddString(papszArg, "-dstalpha");
    }
    papszArg = CSLAddString(papszArg, "-r");
    papszArg = CSLAddString(papszArg, osResampling);
    papszArg = CSLAddString(papszArg, "-wo");
    papszArg = CSLAddString(papszArg, "SAMPLE_GRID=YES");
    const char *pszNumThreads = CSLFetchNameValue(papszOptions, "NUM_THREADS");
    if (pszNumThreads)
    {
        papszArg = CSLAddString(papszArg, "-wo");
        papszArg = CSLAddString(
            papszArg, (CPLString("NUM_THREADS=") + pszNumThreads).c_str());
    }

    const auto poFirstBand = poSrcDS->GetRasterBand(1);
    const bool bHasMask = poFirstBand->GetMaskFlags() == GMF_PER_DATASET;

    const int nBands = poSrcDS->GetRasterCount();
    const char *pszOverviews =
        CSLFetchNameValueDef(papszOptions, "OVERVIEWS", "AUTO");
    const bool bUseExistingOrNone = EQUAL(pszOverviews, "FORCE_USE_EXISTING") ||
                                    EQUAL(pszOverviews, "NONE");
    dfTotalPixelsToProcess =
        double(nXSize) * nYSize * (nBands + (bHasMask ? 1 : 0)) +
        ((bHasMask && !bUseExistingOrNone) ? double(nXSize) * nYSize / 3 : 0) +
        (!bUseExistingOrNone ? double(nXSize) * nYSize * nBands / 3 : 0) +
        double(nXSize) * nYSize * (nBands + (bHasMask ? 1 : 0)) * 4. / 3;

    auto psOptions = GDALWarpAppOptionsNew(papszArg, nullptr);
    CSLDestroy(papszArg);
    if (psOptions == nullptr)
        return nullptr;

    const double dfNextPixels =
        double(nXSize) * nYSize * (nBands + (bHasMask ? 1 : 0));
    void *pScaledProgress = GDALCreateScaledProgress(
        dfCurPixels / dfTotalPixelsToProcess,
        dfNextPixels / dfTotalPixelsToProcess, pfnProgress, pProgressData);
    dfCurPixels = dfNextPixels;

    CPLDebug("COG", "Reprojecting source dataset: start");
    GDALWarpAppOptionsSetProgress(psOptions, GDALScaledProgress,
                                  pScaledProgress);
    CPLString osTmpFile(GetTmpFilename(pszDstFilename, "warped.tif.tmp"));
    auto hSrcDS = GDALDataset::ToHandle(poSrcDS);

    std::unique_ptr<CPLConfigOptionSetter> poWarpThreadSetter;
    if (pszNumThreads)
    {
        poWarpThreadSetter.reset(new CPLConfigOptionSetter(
            "GDAL_NUM_THREADS", pszNumThreads, false));
    }

    auto hRet = GDALWarp(osTmpFile, nullptr, 1, &hSrcDS, psOptions, nullptr);
    GDALWarpAppOptionsFree(psOptions);
    CPLDebug("COG", "Reprojecting source dataset: end");

    GDALDestroyScaledProgress(pScaledProgress);

    return std::unique_ptr<GDALDataset>(GDALDataset::FromHandle(hRet));
}

/************************************************************************/
/*                            GDALCOGCreator                            */
/************************************************************************/

struct GDALCOGCreator final
{
    std::unique_ptr<GDALDataset> m_poReprojectedDS{};
    std::unique_ptr<GDALDataset> m_poRGBMaskDS{};
    std::unique_ptr<GDALDataset> m_poVRTWithOrWithoutStats{};
    CPLString m_osTmpOverviewFilename{};
    CPLString m_osTmpMskOverviewFilename{};

    ~GDALCOGCreator();

    GDALDataset *Create(const char *pszFilename, GDALDataset *const poSrcDS,
                        char **papszOptions, GDALProgressFunc pfnProgress,
                        void *pProgressData);
};

/************************************************************************/
/*                    GDALCOGCreator::~GDALCOGCreator()                 */
/************************************************************************/

GDALCOGCreator::~GDALCOGCreator()
{
    // Destroy m_poRGBMaskDS before m_poReprojectedDS since the former
    // may reference the later
    m_poRGBMaskDS.reset();

    // Config option just for testing purposes
    const bool bDeleteTempFiles =
        CPLTestBool(CPLGetConfigOption("COG_DELETE_TEMP_FILES", "YES"));
    if (bDeleteTempFiles)
    {
        if (m_poReprojectedDS)
        {
            CPLString osProjectedDSName(m_poReprojectedDS->GetDescription());
            m_poReprojectedDS.reset();
            VSIUnlink(osProjectedDSName);
        }
        if (!m_osTmpOverviewFilename.empty())
        {
            VSIUnlink(m_osTmpOverviewFilename);
        }
        if (!m_osTmpMskOverviewFilename.empty())
        {
            VSIUnlink(m_osTmpMskOverviewFilename);
        }
    }
}

/************************************************************************/
/*                    GDALCOGCreator::Create()                          */
/************************************************************************/

GDALDataset *GDALCOGCreator::Create(const char *pszFilename,
                                    GDALDataset *const poSrcDS,
                                    char **papszOptions,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData)
{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    if (poSrcDS->GetRasterCount() == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "COG driver does not support 0-band source raster");
        return nullptr;
    }

    const CPLString osCompress = CSLFetchNameValueDef(
        papszOptions, "COMPRESS", gbHasLZW ? "LZW" : "NONE");

    const char *pszInterleave =
        CSLFetchNameValueDef(papszOptions, "INTERLEAVE", "PIXEL");
    if (EQUAL(osCompress, "WEBP"))
    {
        if (!EQUAL(pszInterleave, "PIXEL"))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "COMPRESS=WEBP only supported for INTERLEAVE=PIXEL");
            return nullptr;
        }
    }

    CPLConfigOptionSetter oSetterReportDirtyBlockFlushing(
        "GDAL_REPORT_DIRTY_BLOCK_FLUSHING", "NO", true);

    const char *pszStatistics =
        CSLFetchNameValueDef(papszOptions, "STATISTICS", "AUTO");
    auto poSrcFirstBand = poSrcDS->GetRasterBand(1);
    const bool bSrcHasStatistics =
        poSrcFirstBand->GetMetadataItem("STATISTICS_MINIMUM") &&
        poSrcFirstBand->GetMetadataItem("STATISTICS_MAXIMUM") &&
        poSrcFirstBand->GetMetadataItem("STATISTICS_MEAN") &&
        poSrcFirstBand->GetMetadataItem("STATISTICS_STDDEV");
    bool bNeedStats = false;
    bool bRemoveStats = false;
    bool bWrkHasStatistics = bSrcHasStatistics;
    if (EQUAL(pszStatistics, "AUTO"))
    {
        // nothing
    }
    else if (CPLTestBool(pszStatistics))
    {
        bNeedStats = true;
    }
    else
    {
        bRemoveStats = true;
    }

    double dfCurPixels = 0;
    double dfTotalPixelsToProcess = 0;
    GDALDataset *poCurDS = poSrcDS;

    std::unique_ptr<gdal::TileMatrixSet> poTM;
    int nZoomLevel = 0;
    int nAlignedLevels = 0;
    if (COGHasWarpingOptions(papszOptions))
    {
        CPLString osTargetResampling;
        CPLString osTargetSRS;
        int nTargetXSize = 0;
        int nTargetYSize = 0;
        double dfTargetMinX = 0;
        double dfTargetMinY = 0;
        double dfTargetMaxX = 0;
        double dfTargetMaxY = 0;
        double dfRes = 0;
        if (!COGGetWarpingCharacteristics(
                poCurDS, papszOptions, osTargetResampling, osTargetSRS,
                nTargetXSize, nTargetYSize, dfTargetMinX, dfTargetMinY,
                dfTargetMaxX, dfTargetMaxY, dfRes, poTM, nZoomLevel,
                nAlignedLevels))
        {
            return nullptr;
        }

        // Collect information on source dataset to see if it already
        // matches the warping specifications
        CPLString osSrcSRS;
        const auto poSrcSRS = poCurDS->GetSpatialRef();
        if (poSrcSRS)
        {
            const char *pszAuthName = poSrcSRS->GetAuthorityName(nullptr);
            const char *pszAuthCode = poSrcSRS->GetAuthorityCode(nullptr);
            if (pszAuthName && pszAuthCode)
            {
                osSrcSRS = pszAuthName;
                osSrcSRS += ':';
                osSrcSRS += pszAuthCode;
            }
        }
        double dfSrcMinX = 0;
        double dfSrcMinY = 0;
        double dfSrcMaxX = 0;
        double dfSrcMaxY = 0;
        GDALGeoTransform srcGT;
        const int nSrcXSize = poCurDS->GetRasterXSize();
        const int nSrcYSize = poCurDS->GetRasterYSize();
        if (poCurDS->GetGeoTransform(srcGT) == CE_None)
        {
            dfSrcMinX = srcGT[0];
            dfSrcMaxY = srcGT[3];
            dfSrcMaxX = srcGT[0] + nSrcXSize * srcGT[1];
            dfSrcMinY = srcGT[3] + nSrcYSize * srcGT[5];
        }

        if (nTargetXSize == nSrcXSize && nTargetYSize == nSrcYSize &&
            osTargetSRS == osSrcSRS &&
            fabs(dfSrcMinX - dfTargetMinX) < 1e-10 * fabs(dfSrcMinX) &&
            fabs(dfSrcMinY - dfTargetMinY) < 1e-10 * fabs(dfSrcMinY) &&
            fabs(dfSrcMaxX - dfTargetMaxX) < 1e-10 * fabs(dfSrcMaxX) &&
            fabs(dfSrcMaxY - dfTargetMaxY) < 1e-10 * fabs(dfSrcMaxY))
        {
            CPLDebug("COG",
                     "Skipping reprojection step: "
                     "source dataset matches reprojection specifications");
        }
        else
        {
            m_poReprojectedDS = CreateReprojectedDS(
                pszFilename, poCurDS, papszOptions, osTargetResampling,
                osTargetSRS, nTargetXSize, nTargetYSize, dfTargetMinX,
                dfTargetMinY, dfTargetMaxX, dfTargetMaxY, dfRes, pfnProgress,
                pProgressData, dfCurPixels, dfTotalPixelsToProcess);
            if (!m_poReprojectedDS)
                return nullptr;
            poCurDS = m_poReprojectedDS.get();

            if (bSrcHasStatistics && !bNeedStats && !bRemoveStats)
            {
                bNeedStats = true;
            }
            bWrkHasStatistics = false;
        }
    }

    if (EQUAL(osCompress, "JPEG") && EQUAL(pszInterleave, "PIXEL") &&
        (poCurDS->GetRasterCount() == 2 || poCurDS->GetRasterCount() == 4) &&
        poCurDS->GetRasterBand(poCurDS->GetRasterCount())
                ->GetColorInterpretation() == GCI_AlphaBand)
    {
        char **papszArg = nullptr;
        papszArg = CSLAddString(papszArg, "-of");
        papszArg = CSLAddString(papszArg, "VRT");
        papszArg = CSLAddString(papszArg, "-b");
        papszArg = CSLAddString(papszArg, "1");
        if (poCurDS->GetRasterCount() == 2)
        {
            papszArg = CSLAddString(papszArg, "-mask");
            papszArg = CSLAddString(papszArg, "2");
        }
        else
        {
            CPLAssert(poCurDS->GetRasterCount() == 4);
            papszArg = CSLAddString(papszArg, "-b");
            papszArg = CSLAddString(papszArg, "2");
            papszArg = CSLAddString(papszArg, "-b");
            papszArg = CSLAddString(papszArg, "3");
            papszArg = CSLAddString(papszArg, "-mask");
            papszArg = CSLAddString(papszArg, "4");
        }
        GDALTranslateOptions *psOptions =
            GDALTranslateOptionsNew(papszArg, nullptr);
        CSLDestroy(papszArg);
        GDALDatasetH hRGBMaskDS = GDALTranslate(
            "", GDALDataset::ToHandle(poCurDS), psOptions, nullptr);
        GDALTranslateOptionsFree(psOptions);
        if (!hRGBMaskDS)
        {
            return nullptr;
        }
        m_poRGBMaskDS.reset(GDALDataset::FromHandle(hRGBMaskDS));
        poCurDS = m_poRGBMaskDS.get();

        if (bSrcHasStatistics && !bNeedStats && !bRemoveStats)
        {
            bNeedStats = true;
        }
        else if (bRemoveStats && bWrkHasStatistics)
        {
            poCurDS->ClearStatistics();
            bRemoveStats = false;
        }
    }

    const int nBands = poCurDS->GetRasterCount();
    const int nXSize = poCurDS->GetRasterXSize();
    const int nYSize = poCurDS->GetRasterYSize();

    const auto CreateVRTWithOrWithoutStats = [this, &poCurDS]()
    {
        const char *const apszOptions[] = {"-of", "VRT", nullptr};
        GDALTranslateOptions *psOptions =
            GDALTranslateOptionsNew(const_cast<char **>(apszOptions), nullptr);
        GDALDatasetH hVRTDS = GDALTranslate("", GDALDataset::ToHandle(poCurDS),
                                            psOptions, nullptr);
        GDALTranslateOptionsFree(psOptions);
        if (!hVRTDS)
            return false;
        m_poVRTWithOrWithoutStats.reset(GDALDataset::FromHandle(hVRTDS));
        poCurDS = m_poVRTWithOrWithoutStats.get();
        return true;
    };

    if (bNeedStats)
    {
        if (poSrcDS == poCurDS && !CreateVRTWithOrWithoutStats())
        {
            return nullptr;
        }
        poCurDS->ClearStatistics();

        // Avoid source files to be modified
        CPLConfigOptionSetter enablePamDirtyDisabler(
            "GDAL_PAM_ENABLE_MARK_DIRTY", "NO", true);

        for (int i = 1; i <= nBands; ++i)
        {
            poCurDS->GetRasterBand(i)->ComputeStatistics(
                /*bApproxOK=*/FALSE, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr);
        }
    }
    else if (bRemoveStats && bWrkHasStatistics)
    {
        if (!CreateVRTWithOrWithoutStats())
            return nullptr;

        m_poVRTWithOrWithoutStats->ClearStatistics();
    }

    CPLString osBlockSize(CSLFetchNameValueDef(papszOptions, "BLOCKSIZE", ""));
    if (osBlockSize.empty())
    {
        if (poTM)
        {
            osBlockSize.Printf("%d", poTM->tileMatrixList()[0].mTileWidth);
        }
        else
        {
            osBlockSize = "512";
        }
    }

    const int nOvrThresholdSize = atoi(osBlockSize);

    const auto poFirstBand = poCurDS->GetRasterBand(1);
    const bool bHasMask = poFirstBand->GetMaskFlags() == GMF_PER_DATASET;

    CPLString osOverviews =
        CSLFetchNameValueDef(papszOptions, "OVERVIEWS", "AUTO");
    const bool bUseExistingOrNone =
        EQUAL(osOverviews, "FORCE_USE_EXISTING") || EQUAL(osOverviews, "NONE");

    const int nOverviewCount =
        atoi(CSLFetchNameValueDef(papszOptions, "OVERVIEW_COUNT", "-1"));

    const bool bGenerateMskOvr =
        !bUseExistingOrNone && bHasMask &&
        (nXSize > nOvrThresholdSize || nYSize > nOvrThresholdSize ||
         nOverviewCount > 0) &&
        (EQUAL(osOverviews, "IGNORE_EXISTING") ||
         poFirstBand->GetMaskBand()->GetOverviewCount() == 0);
    const bool bGenerateOvr =
        !bUseExistingOrNone &&
        (nXSize > nOvrThresholdSize || nYSize > nOvrThresholdSize ||
         nOverviewCount > 0) &&
        (EQUAL(osOverviews, "IGNORE_EXISTING") ||
         poFirstBand->GetOverviewCount() == 0);

    std::vector<std::pair<int, int>> asOverviewDims;
    int nTmpXSize = nXSize;
    int nTmpYSize = nYSize;
    if (poTM)
    {
        const auto &tmList = poTM->tileMatrixList();
        int nCurLevel = nZoomLevel;
        while (true)
        {
            if (nOverviewCount < 0)
            {
                if (nTmpXSize <= nOvrThresholdSize &&
                    nTmpYSize <= nOvrThresholdSize)
                    break;
            }
            else if (static_cast<int>(asOverviewDims.size()) ==
                         nOverviewCount ||
                     (nTmpXSize == 1 && nTmpYSize == 1))
            {
                break;
            }
            const double dfResRatio =
                (nCurLevel >= 1)
                    ? tmList[nCurLevel - 1].mResX / tmList[nCurLevel].mResX
                    : 2;
            nTmpXSize = static_cast<int>(nTmpXSize / dfResRatio + 0.5);
            nTmpYSize = static_cast<int>(nTmpYSize / dfResRatio + 0.5);
            if (nTmpXSize == 0)
                nTmpXSize = 1;
            if (nTmpYSize == 0)
                nTmpYSize = 1;
            asOverviewDims.emplace_back(std::pair(nTmpXSize, nTmpYSize));
            nCurLevel--;
        }
    }
    else if (bGenerateMskOvr || bGenerateOvr)
    {
        if (!bGenerateOvr)
        {
            // If generating only .msk.ovr, use the exact overview size as
            // the overviews of the imagery.
            int nIters = poFirstBand->GetOverviewCount();
            if (nOverviewCount >= 0 && nOverviewCount < nIters)
                nIters = nOverviewCount;
            for (int i = 0; i < nIters; i++)
            {
                auto poOvrBand = poFirstBand->GetOverview(i);
                asOverviewDims.emplace_back(
                    std::pair(poOvrBand->GetXSize(), poOvrBand->GetYSize()));
            }
        }
        else
        {
            while (true)
            {
                if (nOverviewCount < 0)
                {
                    if (nTmpXSize <= nOvrThresholdSize &&
                        nTmpYSize <= nOvrThresholdSize)
                        break;
                }
                else if (static_cast<int>(asOverviewDims.size()) ==
                             nOverviewCount ||
                         (nTmpXSize == 1 && nTmpYSize == 1))
                {
                    break;
                }
                nTmpXSize /= 2;
                nTmpYSize /= 2;
                if (nTmpXSize == 0)
                    nTmpXSize = 1;
                if (nTmpYSize == 0)
                    nTmpYSize = 1;
                asOverviewDims.emplace_back(std::pair(nTmpXSize, nTmpYSize));
            }
        }
    }

    if (dfTotalPixelsToProcess == 0.0)
    {
        dfTotalPixelsToProcess =
            (bGenerateMskOvr ? double(nXSize) * nYSize / 3 : 0) +
            (bGenerateOvr ? double(nXSize) * nYSize * nBands / 3 : 0) +
            double(nXSize) * nYSize * (nBands + (bHasMask ? 1 : 0)) * 4. / 3;
    }

    CPLStringList aosOverviewOptions;
    aosOverviewOptions.SetNameValue(
        "COMPRESS",
        CPLGetConfigOption("COG_TMP_COMPRESSION",  // only for debug purposes
                           HasZSTDCompression() ? "ZSTD" : "LZW"));
    aosOverviewOptions.SetNameValue(
        "NUM_THREADS", CSLFetchNameValue(papszOptions, "NUM_THREADS"));
    aosOverviewOptions.SetNameValue("BIGTIFF", "YES");
    aosOverviewOptions.SetNameValue("SPARSE_OK", "YES");

    if (bGenerateMskOvr)
    {
        CPLDebug("COG", "Generating overviews of the mask: start");
        m_osTmpMskOverviewFilename = GetTmpFilename(pszFilename, "msk.ovr.tmp");
        GDALRasterBand *poSrcMask = poFirstBand->GetMaskBand();
        const char *pszResampling = CSLFetchNameValueDef(
            papszOptions, "OVERVIEW_RESAMPLING",
            CSLFetchNameValueDef(papszOptions, "RESAMPLING",
                                 GetResampling(poSrcDS)));

        double dfNextPixels = dfCurPixels + double(nXSize) * nYSize / 3;
        void *pScaledProgress = GDALCreateScaledProgress(
            dfCurPixels / dfTotalPixelsToProcess,
            dfNextPixels / dfTotalPixelsToProcess, pfnProgress, pProgressData);
        dfCurPixels = dfNextPixels;

        CPLErr eErr = GTIFFBuildOverviewsEx(
            m_osTmpMskOverviewFilename, 1, &poSrcMask,
            static_cast<int>(asOverviewDims.size()), nullptr,
            asOverviewDims.data(), pszResampling, aosOverviewOptions.List(),
            GDALScaledProgress, pScaledProgress);
        CPLDebug("COG", "Generating overviews of the mask: end");

        GDALDestroyScaledProgress(pScaledProgress);
        if (eErr != CE_None)
        {
            return nullptr;
        }
    }

    if (bGenerateOvr)
    {
        CPLDebug("COG", "Generating overviews of the imagery: start");
        m_osTmpOverviewFilename = GetTmpFilename(pszFilename, "ovr.tmp");
        std::vector<GDALRasterBand *> apoSrcBands;
        for (int i = 0; i < nBands; i++)
            apoSrcBands.push_back(poCurDS->GetRasterBand(i + 1));
        const char *pszResampling = CSLFetchNameValueDef(
            papszOptions, "OVERVIEW_RESAMPLING",
            CSLFetchNameValueDef(papszOptions, "RESAMPLING",
                                 GetResampling(poSrcDS)));

        double dfNextPixels =
            dfCurPixels + double(nXSize) * nYSize * nBands / 3;
        void *pScaledProgress = GDALCreateScaledProgress(
            dfCurPixels / dfTotalPixelsToProcess,
            dfNextPixels / dfTotalPixelsToProcess, pfnProgress, pProgressData);
        dfCurPixels = dfNextPixels;

        if (nBands > 1)
        {
            aosOverviewOptions.SetNameValue("INTERLEAVE", "PIXEL");
        }
        if (!m_osTmpMskOverviewFilename.empty())
        {
            aosOverviewOptions.SetNameValue("MASK_OVERVIEW_DATASET",
                                            m_osTmpMskOverviewFilename);
        }
        CPLErr eErr = GTIFFBuildOverviewsEx(
            m_osTmpOverviewFilename, nBands, &apoSrcBands[0],
            static_cast<int>(asOverviewDims.size()), nullptr,
            asOverviewDims.data(), pszResampling, aosOverviewOptions.List(),
            GDALScaledProgress, pScaledProgress);
        CPLDebug("COG", "Generating overviews of the imagery: end");

        GDALDestroyScaledProgress(pScaledProgress);
        if (eErr != CE_None)
        {
            return nullptr;
        }
    }

    CPLStringList aosOptions;
    aosOptions.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    aosOptions.SetNameValue("COMPRESS", osCompress);
    aosOptions.SetNameValue("TILED", "YES");
    aosOptions.SetNameValue("BLOCKXSIZE", osBlockSize);
    aosOptions.SetNameValue("BLOCKYSIZE", osBlockSize);
    const char *pszPredictor =
        CSLFetchNameValueDef(papszOptions, "PREDICTOR", "FALSE");
    const char *pszPredictorValue = GetPredictor(poSrcDS, pszPredictor);
    if (pszPredictorValue != nullptr)
    {
        aosOptions.SetNameValue("PREDICTOR", pszPredictorValue);
    }

    const char *pszQuality = CSLFetchNameValue(papszOptions, "QUALITY");
    if (EQUAL(osCompress, "JPEG"))
    {
        aosOptions.SetNameValue("JPEG_QUALITY", pszQuality);
        if (nBands == 3 && EQUAL(pszInterleave, "PIXEL"))
            aosOptions.SetNameValue("PHOTOMETRIC", "YCBCR");
    }
    else if (EQUAL(osCompress, "WEBP"))
    {
        if (pszQuality && atoi(pszQuality) == 100)
            aosOptions.SetNameValue("WEBP_LOSSLESS", "YES");
        aosOptions.SetNameValue("WEBP_LEVEL", pszQuality);
    }
    else if (EQUAL(osCompress, "DEFLATE") || EQUAL(osCompress, "LERC_DEFLATE"))
    {
        aosOptions.SetNameValue("ZLEVEL",
                                CSLFetchNameValue(papszOptions, "LEVEL"));
    }
    else if (EQUAL(osCompress, "ZSTD") || EQUAL(osCompress, "LERC_ZSTD"))
    {
        aosOptions.SetNameValue("ZSTD_LEVEL",
                                CSLFetchNameValue(papszOptions, "LEVEL"));
    }
    else if (EQUAL(osCompress, "LZMA"))
    {
        aosOptions.SetNameValue("LZMA_PRESET",
                                CSLFetchNameValue(papszOptions, "LEVEL"));
    }

    if (STARTS_WITH_CI(osCompress, "LERC"))
    {
        aosOptions.SetNameValue("MAX_Z_ERROR",
                                CSLFetchNameValue(papszOptions, "MAX_Z_ERROR"));
        aosOptions.SetNameValue(
            "MAX_Z_ERROR_OVERVIEW",
            CSLFetchNameValue(papszOptions, "MAX_Z_ERROR_OVERVIEW"));
    }

    if (STARTS_WITH_CI(osCompress, "JXL"))
    {
        for (const char *pszKey : {"JXL_LOSSLESS", "JXL_EFFORT", "JXL_DISTANCE",
                                   "JXL_ALPHA_DISTANCE"})
        {
            const char *pszValue = CSLFetchNameValue(papszOptions, pszKey);
            if (pszValue)
                aosOptions.SetNameValue(pszKey, pszValue);
        }
    }

    aosOptions.SetNameValue("BIGTIFF",
                            CSLFetchNameValue(papszOptions, "BIGTIFF"));
    aosOptions.SetNameValue("NUM_THREADS",
                            CSLFetchNameValue(papszOptions, "NUM_THREADS"));
    aosOptions.SetNameValue("GEOTIFF_VERSION",
                            CSLFetchNameValue(papszOptions, "GEOTIFF_VERSION"));
    aosOptions.SetNameValue("SPARSE_OK",
                            CSLFetchNameValue(papszOptions, "SPARSE_OK"));
    aosOptions.SetNameValue("NBITS", CSLFetchNameValue(papszOptions, "NBITS"));

    if (EQUAL(osOverviews, "NONE"))
    {
        aosOptions.SetNameValue("@OVERVIEW_DATASET", "");
    }
    else
    {
        if (!m_osTmpOverviewFilename.empty())
        {
            aosOptions.SetNameValue("@OVERVIEW_DATASET",
                                    m_osTmpOverviewFilename);
        }
        if (!m_osTmpMskOverviewFilename.empty())
        {
            aosOptions.SetNameValue("@MASK_OVERVIEW_DATASET",
                                    m_osTmpMskOverviewFilename);
        }
        aosOptions.SetNameValue(
            "@OVERVIEW_COUNT",
            CSLFetchNameValue(papszOptions, "OVERVIEW_COUNT"));
    }

    const CPLString osTilingScheme(
        CSLFetchNameValueDef(papszOptions, "TILING_SCHEME", "CUSTOM"));
    if (osTilingScheme != "CUSTOM")
    {
        aosOptions.SetNameValue("@TILING_SCHEME_NAME", osTilingScheme);
        aosOptions.SetNameValue("@TILING_SCHEME_ZOOM_LEVEL",
                                CPLSPrintf("%d", nZoomLevel));
        if (nAlignedLevels > 0)
        {
            aosOptions.SetNameValue("@TILING_SCHEME_ALIGNED_LEVELS",
                                    CPLSPrintf("%d", nAlignedLevels));
        }
    }
    const char *pszOverviewCompress = CSLFetchNameValueDef(
        papszOptions, "OVERVIEW_COMPRESS", osCompress.c_str());

    CPLConfigOptionSetter ovrCompressSetter("COMPRESS_OVERVIEW",
                                            pszOverviewCompress, true);
    const char *pszOverviewQuality =
        CSLFetchNameValue(papszOptions, "OVERVIEW_QUALITY");
    CPLConfigOptionSetter ovrQualityJpegSetter("JPEG_QUALITY_OVERVIEW",
                                               pszOverviewQuality, true);

    std::unique_ptr<CPLConfigOptionSetter> poWebpLosslessSetter;
    std::unique_ptr<CPLConfigOptionSetter> poWebpLevelSetter;
    if (EQUAL(pszOverviewCompress, "WEBP"))
    {
        if (pszOverviewQuality && CPLAtof(pszOverviewQuality) == 100.0)
        {
            poWebpLosslessSetter.reset(new CPLConfigOptionSetter(
                "WEBP_LOSSLESS_OVERVIEW", "TRUE", true));
        }
        else
        {
            poWebpLosslessSetter.reset(new CPLConfigOptionSetter(
                "WEBP_LOSSLESS_OVERVIEW", "FALSE", true));
            poWebpLevelSetter.reset(new CPLConfigOptionSetter(
                "WEBP_LEVEL_OVERVIEW", pszOverviewQuality, true));
        }
    }

    std::unique_ptr<CPLConfigOptionSetter> poPhotometricSetter;
    if (nBands == 3 && EQUAL(pszOverviewCompress, "JPEG") &&
        EQUAL(pszInterleave, "PIXEL"))
    {
        poPhotometricSetter.reset(
            new CPLConfigOptionSetter("PHOTOMETRIC_OVERVIEW", "YCBCR", true));
    }

    const char *osOvrPredictor =
        CSLFetchNameValueDef(papszOptions, "OVERVIEW_PREDICTOR", "FALSE");
    const char *pszOvrPredictorValue = GetPredictor(poSrcDS, osOvrPredictor);
    CPLConfigOptionSetter ovrPredictorSetter("PREDICTOR_OVERVIEW",
                                             pszOvrPredictorValue, true);

    GDALDriver *poGTiffDrv =
        GDALDriver::FromHandle(GDALGetDriverByName("GTiff"));
    if (!poGTiffDrv)
        return nullptr;
    void *pScaledProgress = GDALCreateScaledProgress(
        dfCurPixels / dfTotalPixelsToProcess, 1.0, pfnProgress, pProgressData);

    CPLConfigOptionSetter oSetterInternalMask("GDAL_TIFF_INTERNAL_MASK", "YES",
                                              false);

    const char *pszCopySrcMDD = CSLFetchNameValue(papszOptions, "COPY_SRC_MDD");
    if (pszCopySrcMDD)
        aosOptions.SetNameValue("COPY_SRC_MDD", pszCopySrcMDD);
    char **papszSrcMDD = CSLFetchNameValueMultiple(papszOptions, "SRC_MDD");
    for (CSLConstList papszSrcMDDIter = papszSrcMDD;
         papszSrcMDDIter && *papszSrcMDDIter; ++papszSrcMDDIter)
        aosOptions.AddNameValue("SRC_MDD", *papszSrcMDDIter);
    CSLDestroy(papszSrcMDD);

    if (EQUAL(pszInterleave, "TILE"))
    {
        aosOptions.SetNameValue("INTERLEAVE", "BAND");
        aosOptions.SetNameValue("@TILE_INTERLEAVE", "YES");
    }
    else
    {
        aosOptions.SetNameValue("INTERLEAVE", pszInterleave);
    }

    aosOptions.SetNameValue("@FLUSHCACHE", "YES");

    CPLDebug("COG", "Generating final product: start");
    auto poRet =
        poGTiffDrv->CreateCopy(pszFilename, poCurDS, false, aosOptions.List(),
                               GDALScaledProgress, pScaledProgress);

    GDALDestroyScaledProgress(pScaledProgress);

    CPLDebug("COG", "Generating final product: end");
    return poRet;
}

/************************************************************************/
/*                            COGCreateCopy()                           */
/************************************************************************/

static GDALDataset *COGCreateCopy(const char *pszFilename, GDALDataset *poSrcDS,
                                  int /*bStrict*/, char **papszOptions,
                                  GDALProgressFunc pfnProgress,
                                  void *pProgressData)
{
    return GDALCOGCreator().Create(pszFilename, poSrcDS, papszOptions,
                                   pfnProgress, pProgressData);
}

/************************************************************************/
/*                          GDALRegister_COG()                          */
/************************************************************************/

class GDALCOGDriver final : public GDALDriver
{
    std::mutex m_oMutex{};
    bool m_bInitialized = false;

    bool bHasLZW = false;
    bool bHasDEFLATE = false;
    bool bHasLZMA = false;
    bool bHasZSTD = false;
    bool bHasJPEG = false;
    bool bHasWebP = false;
    bool bHasLERC = false;
    std::string osCompressValues{};

    void InitializeCreationOptionList();

  public:
    GDALCOGDriver();

    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override;

    char **GetMetadata(const char *pszDomain) override
    {
        std::lock_guard oLock(m_oMutex);
        InitializeCreationOptionList();
        return GDALDriver::GetMetadata(pszDomain);
    }
};

GDALCOGDriver::GDALCOGDriver()
{
    // We could defer this in InitializeCreationOptionList() but with currently
    // released libtiff versions where there was a bug (now fixed) in
    // TIFFGetConfiguredCODECs(), this wouldn't work properly if the LERC codec
    // had been registered in between
    osCompressValues = GTiffGetCompressValues(bHasLZW, bHasDEFLATE, bHasLZMA,
                                              bHasZSTD, bHasJPEG, bHasWebP,
                                              bHasLERC, true /* bForCOG */);
    gbHasLZW = bHasLZW;
}

const char *GDALCOGDriver::GetMetadataItem(const char *pszName,
                                           const char *pszDomain)
{
    std::lock_guard oLock(m_oMutex);
    if (EQUAL(pszName, GDAL_DMD_CREATIONOPTIONLIST))
    {
        InitializeCreationOptionList();
    }
    return GDALDriver::GetMetadataItem(pszName, pszDomain);
}

void GDALCOGDriver::InitializeCreationOptionList()
{
    if (m_bInitialized)
        return;
    m_bInitialized = true;

    CPLString osOptions;
    osOptions = "<CreationOptionList>"
                "   <Option name='COMPRESS' type='string-select' default='";
    osOptions += bHasLZW ? "LZW" : "NONE";
    osOptions += "'>";
    osOptions += osCompressValues;
    osOptions += "   </Option>";

    osOptions +=
        "   <Option name='OVERVIEW_COMPRESS' type='string-select' default='";
    osOptions += bHasLZW ? "LZW" : "NONE";
    osOptions += "'>";
    osOptions += osCompressValues;
    osOptions += "   </Option>";

    if (bHasLZW || bHasDEFLATE || bHasZSTD || bHasLZMA)
    {
        const char *osPredictorOptions =
            "     <Value>YES</Value>"
            "     <Value>NO</Value>"
            "     <Value alias='2'>STANDARD</Value>"
            "     <Value alias='3'>FLOATING_POINT</Value>";

        osOptions +=
            "   <Option name='LEVEL' type='int' "
            "description='DEFLATE/ZSTD/LZMA compression level: 1 (fastest)'/>";

        osOptions +=
            "   <Option name='PREDICTOR' type='string-select' default='FALSE'>";
        osOptions += osPredictorOptions;
        osOptions += "   </Option>"
                     "   <Option name='OVERVIEW_PREDICTOR' "
                     "type='string-select' default='FALSE'>";
        osOptions += osPredictorOptions;
        osOptions += "   </Option>";
    }
    if (bHasJPEG || bHasWebP)
    {
        std::string osJPEG_WEBP;
        if (bHasJPEG)
            osJPEG_WEBP = "JPEG";
        if (bHasWebP)
        {
            if (!osJPEG_WEBP.empty())
                osJPEG_WEBP += '/';
            osJPEG_WEBP += "WEBP";
        }
        osOptions += "   <Option name='QUALITY' type='int' "
                     "description='" +
                     osJPEG_WEBP +
                     " quality 1-100' min='1' max='100' default='75'/>"
                     "   <Option name='OVERVIEW_QUALITY' type='int' "
                     "description='Overview " +
                     osJPEG_WEBP +
                     " quality 1-100' min='1' max='100' "
                     "default='75'/>";
    }
    if (bHasLERC)
    {
        osOptions +=
            ""
            "   <Option name='MAX_Z_ERROR' type='float' description='Maximum "
            "error for LERC compression' default='0'/>"
            "   <Option name='MAX_Z_ERROR_OVERVIEW' type='float' "
            "description='Maximum error for LERC compression in overviews' "
            "default='0'/>";
    }
#ifdef HAVE_JXL
    osOptions +=
        ""
        "   <Option name='JXL_LOSSLESS' type='boolean' description='Whether "
        "JPEGXL compression should be lossless' default='YES'/>"
        "   <Option name='JXL_EFFORT' type='int' description='Level of effort "
        "1(fast)-9(slow)' min='1' max='9' default='5'/>"
        "   <Option name='JXL_DISTANCE' type='float' description='Distance "
        "level for lossy compression (0=mathematically lossless, 1.0=visually "
        "lossless, usual range [0.5,3])' default='1.0' min='0.01' max='25.0'/>";
#ifdef HAVE_JxlEncoderSetExtraChannelDistance
    osOptions += "   <Option name='JXL_ALPHA_DISTANCE' type='float' "
                 "description='Distance level for alpha channel "
                 "(-1=same as non-alpha channels, "
                 "0=mathematically lossless, 1.0=visually lossless, "
                 "usual range [0.5,3])' default='-1' min='-1' max='25.0'/>";
#endif
#endif
    osOptions +=
        "   <Option name='NUM_THREADS' type='string' "
        "description='Number of worker threads for compression. "
        "Can be set to ALL_CPUS' default='1'/>"
        "   <Option name='NBITS' type='int' description='BITS for sub-byte "
        "files (1-7), sub-uint16_t (9-15), sub-uint32_t (17-31), or float32 "
        "(16)'/>"
        "   <Option name='BLOCKSIZE' type='int' "
        "description='Tile size in pixels' min='128' default='512'/>"
        "   <Option name='INTERLEAVE' type='string-select' default='PIXEL'>"
        "       <Value>BAND</Value>"
        "       <Value>PIXEL</Value>"
        "       <Value>TILE</Value>"
        "   </Option>"
        "   <Option name='BIGTIFF' type='string-select' description='"
        "Force creation of BigTIFF file'>"
        "     <Value>YES</Value>"
        "     <Value>NO</Value>"
        "     <Value>IF_NEEDED</Value>"
        "     <Value>IF_SAFER</Value>"
        "   </Option>"
        "   <Option name='RESAMPLING' type='string' "
        "description='Resampling method for overviews or warping'/>"
        "   <Option name='OVERVIEW_RESAMPLING' type='string' "
        "description='Resampling method for overviews'/>"
        "   <Option name='WARP_RESAMPLING' type='string' "
        "description='Resampling method for warping'/>"
        "   <Option name='OVERVIEWS' type='string-select' description='"
        "Behavior regarding overviews'>"
        "     <Value>AUTO</Value>"
        "     <Value>IGNORE_EXISTING</Value>"
        "     <Value>FORCE_USE_EXISTING</Value>"
        "     <Value>NONE</Value>"
        "   </Option>"
        "  <Option name='OVERVIEW_COUNT' type='int' min='0' "
        "description='Number of overviews'/>"
        "  <Option name='TILING_SCHEME' type='string-select' description='"
        "Which tiling scheme to use pre-defined value or custom inline/outline "
        "JSON definition' default='CUSTOM'>"
        "    <Value>CUSTOM</Value>";

    const auto tmsList = gdal::TileMatrixSet::listPredefinedTileMatrixSets();
    for (const auto &tmsName : tmsList)
    {
        const auto poTM = gdal::TileMatrixSet::parse(tmsName.c_str());
        if (poTM && poTM->haveAllLevelsSameTopLeft() &&
            poTM->haveAllLevelsSameTileSize() &&
            !poTM->hasVariableMatrixWidth())
        {
            osOptions += "    <Value>";
            osOptions += tmsName;
            osOptions += "</Value>";
        }
    }

    osOptions +=
        "  </Option>"
        "  <Option name='ZOOM_LEVEL' type='int' description='Target zoom "
        "level. "
        "Only used for TILING_SCHEME != CUSTOM'/>"
        "  <Option name='ZOOM_LEVEL_STRATEGY' type='string-select' "
        "description='Strategy to determine zoom level. "
        "Only used for TILING_SCHEME != CUSTOM' default='AUTO'>"
        "    <Value>AUTO</Value>"
        "    <Value>LOWER</Value>"
        "    <Value>UPPER</Value>"
        "  </Option>"
        "   <Option name='TARGET_SRS' type='string' "
        "description='Target SRS as EPSG:XXXX, WKT or PROJ string for "
        "reprojection'/>"
        "  <Option name='RES' type='float' description='"
        "Target resolution for reprojection'/>"
        "  <Option name='EXTENT' type='string' description='"
        "Target extent as minx,miny,maxx,maxy for reprojection'/>"
        "  <Option name='ALIGNED_LEVELS' type='int' description='"
        "Number of resolution levels for which the tiles from GeoTIFF and the "
        "specified tiling scheme match'/>"
        "  <Option name='ADD_ALPHA' type='boolean' description='Can be set to "
        "NO to "
        "disable the addition of an alpha band in case of reprojection' "
        "default='YES'/>"
#if LIBGEOTIFF_VERSION >= 1600
        "   <Option name='GEOTIFF_VERSION' type='string-select' default='AUTO' "
        "description='Which version of GeoTIFF must be used'>"
        "       <Value>AUTO</Value>"
        "       <Value>1.0</Value>"
        "       <Value>1.1</Value>"
        "   </Option>"
#endif
        "   <Option name='SPARSE_OK' type='boolean' description='Should empty "
        "blocks be omitted on disk?' default='FALSE'/>"
        "   <Option name='STATISTICS' type='string-select' default='AUTO' "
        "description='Which to add statistics to the output file'>"
        "       <Value>AUTO</Value>"
        "       <Value>YES</Value>"
        "       <Value>NO</Value>"
        "   </Option>"
        "</CreationOptionList>";

    SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, osOptions.c_str());
}

void GDALRegister_COG()

{
    if (GDALGetDriverByName("COG") != nullptr)
        return;

    auto poDriver = new GDALCOGDriver();
    poDriver->SetDescription("COG");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "Cloud optimized GeoTIFF generator");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/cog.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "tif tiff");

    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONDATATYPES,
        "Byte Int8 UInt16 Int16 UInt32 Int32 UInt64 Int64 Float32 "
        "Float64 CInt16 CInt32 CFloat32 CFloat64");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME,
                              "YES");

    poDriver->SetMetadataItem(GDAL_DCAP_COORDINATE_EPOCH, "YES");

    poDriver->pfnCreateCopy = COGCreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
