/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Helper code to implement overview and mask support for many
 *           drivers with no inherent format support.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2000, 2007, Frank Warmerdam
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_multiproc.h"
#include "gdal_priv.h"

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"

//! @cond Doxygen_Suppress
/************************************************************************/
/*                        GDALDefaultOverviews()                        */
/************************************************************************/

GDALDefaultOverviews::GDALDefaultOverviews()
    : poDS(nullptr), poODS(nullptr), bOvrIsAux(false), bCheckedForMask(false),
      bOwnMaskDS(false), poMaskDS(nullptr), poBaseDS(nullptr),
      bCheckedForOverviews(FALSE), pszInitName(nullptr), bInitNameIsOVR(false),
      papszInitSiblingFiles(nullptr)
{
}

/************************************************************************/
/*                       ~GDALDefaultOverviews()                        */
/************************************************************************/

GDALDefaultOverviews::~GDALDefaultOverviews()

{
    CPLFree(pszInitName);
    CSLDestroy(papszInitSiblingFiles);

    CloseDependentDatasets();
}

/************************************************************************/
/*                       CloseDependentDatasets()                       */
/************************************************************************/

int GDALDefaultOverviews::CloseDependentDatasets()
{
    bool bHasDroppedRef = false;
    if (poODS != nullptr)
    {
        bHasDroppedRef = true;
        poODS->FlushCache(true);
        GDALClose(poODS);
        poODS = nullptr;
    }

    if (poMaskDS != nullptr)
    {
        if (bOwnMaskDS)
        {
            bHasDroppedRef = true;
            poMaskDS->FlushCache(true);
            GDALClose(poMaskDS);
        }
        poMaskDS = nullptr;
    }

    return bHasDroppedRef;
}

/************************************************************************/
/*                           IsInitialized()                            */
/*                                                                      */
/*      Returns TRUE if we are initialized.                             */
/************************************************************************/

int GDALDefaultOverviews::IsInitialized()

{
    OverviewScan();
    return poDS != nullptr;
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/

void GDALDefaultOverviews::Initialize(GDALDataset *poDSIn,
                                      const char *pszBasename,
                                      CSLConstList papszSiblingFiles,
                                      bool bNameIsOVR)

{
    poDS = poDSIn;

    /* -------------------------------------------------------------------- */
    /*      If we were already initialized, destroy the old overview        */
    /*      file handle.                                                    */
    /* -------------------------------------------------------------------- */
    if (poODS != nullptr)
    {
        GDALClose(poODS);
        poODS = nullptr;

        CPLDebug("GDAL", "GDALDefaultOverviews::Initialize() called twice - "
                         "this is odd and perhaps dangerous!");
    }

    /* -------------------------------------------------------------------- */
    /*      Store the initialization information for later use in           */
    /*      OverviewScan()                                                  */
    /* -------------------------------------------------------------------- */
    bCheckedForOverviews = FALSE;

    CPLFree(pszInitName);
    pszInitName = nullptr;
    if (pszBasename != nullptr)
        pszInitName = CPLStrdup(pszBasename);
    bInitNameIsOVR = bNameIsOVR;

    CSLDestroy(papszInitSiblingFiles);
    papszInitSiblingFiles = nullptr;
    if (papszSiblingFiles != nullptr)
        papszInitSiblingFiles = CSLDuplicate(papszSiblingFiles);
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/

/** Initialize the GDALDefaultOverviews instance.
 *
 * @param poDSIn Base dataset.
 * @param poOpenInfo Open info instance. Must not be NULL.
 * @param pszName Base dataset name. If set to NULL, poOpenInfo->pszFilename is
 *                used.
 * @param bTransferSiblingFilesIfLoaded Whether sibling files of poOpenInfo
 *                                      should be transferred to this
 *                                      GDALDefaultOverviews instance, if they
 *                                      have bean already loaded.
 * @since 3.10
 */
void GDALDefaultOverviews::Initialize(GDALDataset *poDSIn,
                                      GDALOpenInfo *poOpenInfo,
                                      const char *pszName,
                                      bool bTransferSiblingFilesIfLoaded)
{
    Initialize(poDSIn, pszName ? pszName : poOpenInfo->pszFilename);

    if (bTransferSiblingFilesIfLoaded && poOpenInfo->AreSiblingFilesLoaded())
        TransferSiblingFiles(poOpenInfo->StealSiblingFiles());
}

/************************************************************************/
/*                         TransferSiblingFiles()                       */
/*                                                                      */
/*      Contrary to Initialize(), this sets papszInitSiblingFiles but   */
/*      without duplicating the passed list. Which must be              */
/*      "de-allocatable" with CSLDestroy()                              */
/************************************************************************/

void GDALDefaultOverviews::TransferSiblingFiles(char **papszSiblingFiles)
{
    CSLDestroy(papszInitSiblingFiles);
    papszInitSiblingFiles = papszSiblingFiles;
}

namespace
{
// Prevent infinite recursion.
struct AntiRecursionStructDefaultOvr
{
    int nRecLevel = 0;
    std::set<CPLString> oSetFiles{};
};
}  // namespace

static void FreeAntiRecursionDefaultOvr(void *pData)
{
    delete static_cast<AntiRecursionStructDefaultOvr *>(pData);
}

static AntiRecursionStructDefaultOvr &GetAntiRecursionDefaultOvr()
{
    static AntiRecursionStructDefaultOvr dummy;
    int bMemoryErrorOccurred = false;
    void *pData =
        CPLGetTLSEx(CTLS_GDALDEFAULTOVR_ANTIREC, &bMemoryErrorOccurred);
    if (bMemoryErrorOccurred)
    {
        return dummy;
    }
    if (pData == nullptr)
    {
        auto pAntiRecursion = new AntiRecursionStructDefaultOvr();
        CPLSetTLSWithFreeFuncEx(CTLS_GDALDEFAULTOVR_ANTIREC, pAntiRecursion,
                                FreeAntiRecursionDefaultOvr,
                                &bMemoryErrorOccurred);
        if (bMemoryErrorOccurred)
        {
            delete pAntiRecursion;
            return dummy;
        }
        return *pAntiRecursion;
    }
    return *static_cast<AntiRecursionStructDefaultOvr *>(pData);
}

/************************************************************************/
/*                            OverviewScan()                            */
/*                                                                      */
/*      This is called to scan for overview files when a first          */
/*      request is made with regard to overviews.  It uses the          */
/*      pszInitName, bInitNameIsOVR and papszInitSiblingFiles           */
/*      information that was stored at Initialization() time.           */
/************************************************************************/

void GDALDefaultOverviews::OverviewScan()

{
    if (bCheckedForOverviews || poDS == nullptr)
        return;

    bCheckedForOverviews = true;
    if (pszInitName == nullptr)
        pszInitName = CPLStrdup(poDS->GetDescription());

    AntiRecursionStructDefaultOvr &antiRec = GetAntiRecursionDefaultOvr();
    // 32 should be enough to handle a .ovr.ovr.ovr...
    if (antiRec.nRecLevel == 32)
        return;
    if (antiRec.oSetFiles.find(pszInitName) != antiRec.oSetFiles.end())
        return;
    antiRec.oSetFiles.insert(pszInitName);
    ++antiRec.nRecLevel;

    CPLDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");

    /* -------------------------------------------------------------------- */
    /*      Open overview dataset if it exists.                             */
    /* -------------------------------------------------------------------- */
    if (!EQUAL(pszInitName, ":::VIRTUAL:::") &&
        GDALCanFileAcceptSidecarFile(pszInitName))
    {
        if (bInitNameIsOVR)
            osOvrFilename = pszInitName;
        else
            osOvrFilename.Printf("%s.ovr", pszInitName);

        std::vector<char> achOvrFilename;
        achOvrFilename.resize(osOvrFilename.size() + 1);
        memcpy(&(achOvrFilename[0]), osOvrFilename.c_str(),
               osOvrFilename.size() + 1);
        bool bExists = CPL_TO_BOOL(
            CPLCheckForFile(&achOvrFilename[0], papszInitSiblingFiles));
        osOvrFilename = &achOvrFilename[0];

#if !defined(_WIN32)
        if (!bInitNameIsOVR && !bExists && !papszInitSiblingFiles)
        {
            osOvrFilename.Printf("%s.OVR", pszInitName);
            memcpy(&(achOvrFilename[0]), osOvrFilename.c_str(),
                   osOvrFilename.size() + 1);
            bExists = CPL_TO_BOOL(
                CPLCheckForFile(&achOvrFilename[0], papszInitSiblingFiles));
            osOvrFilename = &achOvrFilename[0];
            if (!bExists)
                osOvrFilename.Printf("%s.ovr", pszInitName);
        }
#endif

        if (bExists)
        {
            poODS = GDALDataset::Open(
                osOvrFilename,
                GDAL_OF_RASTER |
                    (poDS->GetAccess() == GA_Update ? GDAL_OF_UPDATE : 0),
                nullptr, nullptr, papszInitSiblingFiles);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      We didn't find that, so try and find a corresponding aux        */
    /*      file.  Check that we are the dependent file of the aux          */
    /*      file.                                                           */
    /*                                                                      */
    /*      We only use the .aux file for overviews if they already have    */
    /*      overviews existing, or if USE_RRD is set true.                  */
    /* -------------------------------------------------------------------- */
    if (!poODS && !EQUAL(pszInitName, ":::VIRTUAL:::") &&
        GDALCanFileAcceptSidecarFile(pszInitName))
    {
        bool bTryFindAssociatedAuxFile = true;
        if (papszInitSiblingFiles)
        {
            CPLString osAuxFilename = CPLResetExtensionSafe(pszInitName, "aux");
            int iSibling = CSLFindString(papszInitSiblingFiles,
                                         CPLGetFilename(osAuxFilename));
            if (iSibling < 0)
            {
                osAuxFilename = pszInitName;
                osAuxFilename += ".aux";
                iSibling = CSLFindString(papszInitSiblingFiles,
                                         CPLGetFilename(osAuxFilename));
                if (iSibling < 0)
                    bTryFindAssociatedAuxFile = false;
            }
        }

        if (bTryFindAssociatedAuxFile)
        {
            poODS =
                GDALFindAssociatedAuxFile(pszInitName, poDS->GetAccess(), poDS);
        }

        if (poODS)
        {
            const bool bUseRRD =
                CPLTestBool(CPLGetConfigOption("USE_RRD", "NO"));

            bOvrIsAux = true;
            if (GetOverviewCount(1) == 0 && !bUseRRD)
            {
                bOvrIsAux = false;
                GDALClose(poODS);
                poODS = nullptr;
            }
            else
            {
                osOvrFilename = poODS->GetDescription();
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      If we still don't have an overview, check to see if we have     */
    /*      overview metadata referencing a remote (i.e. proxy) or local    */
    /*      subdataset overview dataset.                                    */
    /* -------------------------------------------------------------------- */
    if (poODS == nullptr)
    {
        const char *pszProxyOvrFilename =
            poDS->GetMetadataItem("OVERVIEW_FILE", "OVERVIEWS");

        if (pszProxyOvrFilename != nullptr)
        {
            if (STARTS_WITH_CI(pszProxyOvrFilename, ":::BASE:::"))
            {
                const CPLString osPath = CPLGetPathSafe(poDS->GetDescription());

                osOvrFilename = CPLFormFilenameSafe(
                    osPath, pszProxyOvrFilename + 10, nullptr);
            }
            else
            {
                osOvrFilename = pszProxyOvrFilename;
            }

            CPLPushErrorHandler(CPLQuietErrorHandler);
            poODS = GDALDataset::Open(
                osOvrFilename,
                GDAL_OF_RASTER |
                    (poDS->GetAccess() == GA_Update ? GDAL_OF_UPDATE : 0));
            CPLPopErrorHandler();
        }
    }

    /* -------------------------------------------------------------------- */
    /*      If we have an overview dataset, then mark all the overviews     */
    /*      with the base dataset  Used later for finding overviews         */
    /*      masks.  Uggg.                                                   */
    /* -------------------------------------------------------------------- */
    if (poODS)
    {
        const int nOverviewCount = GetOverviewCount(1);

        for (int iOver = 0; iOver < nOverviewCount; iOver++)
        {
            GDALRasterBand *const poBand = GetOverview(1, iOver);
            GDALDataset *const poOverDS =
                poBand != nullptr ? poBand->GetDataset() : nullptr;

            if (poOverDS != nullptr)
            {
                poOverDS->oOvManager.poBaseDS = poDS;
                poOverDS->oOvManager.poDS = poOverDS;
            }
        }
    }

    // Undo anti recursion protection
    antiRec.oSetFiles.erase(pszInitName);
    --antiRec.nRecLevel;
}

/************************************************************************/
/*                          GetOverviewCount()                          */
/************************************************************************/

int GDALDefaultOverviews::GetOverviewCount(int nBand)

{
    if (poODS == nullptr || nBand < 1 || nBand > poODS->GetRasterCount())
        return 0;

    GDALRasterBand *poBand = poODS->GetRasterBand(nBand);
    if (poBand == nullptr)
        return 0;

    if (bOvrIsAux)
        return poBand->GetOverviewCount();

    return poBand->GetOverviewCount() + 1;
}

/************************************************************************/
/*                            GetOverview()                             */
/************************************************************************/

GDALRasterBand *GDALDefaultOverviews::GetOverview(int nBand, int iOverview)

{
    if (poODS == nullptr || nBand < 1 || nBand > poODS->GetRasterCount())
        return nullptr;

    GDALRasterBand *const poBand = poODS->GetRasterBand(nBand);
    if (poBand == nullptr)
        return nullptr;

    if (bOvrIsAux)
        return poBand->GetOverview(iOverview);

    // TIFF case, base is overview 0.
    if (iOverview == 0)
        return poBand;

    if (iOverview - 1 >= poBand->GetOverviewCount())
        return nullptr;

    return poBand->GetOverview(iOverview - 1);
}

/************************************************************************/
/*                         GDALOvLevelAdjust()                          */
/*                                                                      */
/*      Some overview levels cannot be achieved closely enough to be    */
/*      recognised as the desired overview level.  This function        */
/*      will adjust an overview level to one that is achievable on      */
/*      the given raster size.                                          */
/*                                                                      */
/*      For instance a 1200x1200 image on which a 256 level overview    */
/*      is request will end up generating a 5x5 overview.  However,     */
/*      this will appear to the system be a level 240 overview.         */
/*      This function will adjust 256 to 240 based on knowledge of      */
/*      the image size.                                                 */
/************************************************************************/

int GDALOvLevelAdjust(int nOvLevel, int nXSize)

{
    int nOXSize = DIV_ROUND_UP(nXSize, nOvLevel);

    return static_cast<int>(0.5 + nXSize / static_cast<double>(nOXSize));
}

int GDALOvLevelAdjust2(int nOvLevel, int nXSize, int nYSize)

{
    // Select the larger dimension to have increased accuracy, but
    // with a slight preference to x even if (a bit) smaller than y
    // in an attempt to behave closer as previous behavior.
    if (nXSize >= nYSize / 2 && !(nXSize < nYSize && nXSize < nOvLevel))
    {
        const int nOXSize = DIV_ROUND_UP(nXSize, nOvLevel);

        return static_cast<int>(0.5 + nXSize / static_cast<double>(nOXSize));
    }

    const int nOYSize = DIV_ROUND_UP(nYSize, nOvLevel);

    return static_cast<int>(0.5 + nYSize / static_cast<double>(nOYSize));
}

/************************************************************************/
/*                         GetFloorPowerOfTwo()                         */
/************************************************************************/

static int GetFloorPowerOfTwo(int n)
{
    int p2 = 1;
    while ((n = n >> 1) > 0)
    {
        p2 <<= 1;
    }
    return p2;
}

/************************************************************************/
/*                         GDALComputeOvFactor()                        */
/************************************************************************/

int GDALComputeOvFactor(int nOvrXSize, int nRasterXSize, int nOvrYSize,
                        int nRasterYSize)
{
    // Select the larger dimension to have increased accuracy, but
    // with a slight preference to x even if (a bit) smaller than y
    // in an attempt to behave closer as previous behavior.
    if (nRasterXSize != 1 && nRasterXSize >= nRasterYSize / 2)
    {
        const int nVal = static_cast<int>(
            0.5 + nRasterXSize / static_cast<double>(nOvrXSize));
        // Try to return a power-of-two value
        const int nValPowerOfTwo = GetFloorPowerOfTwo(nVal);
        for (int fact = 1; fact <= 2 && nValPowerOfTwo <= INT_MAX / fact;
             ++fact)
        {
            if (DIV_ROUND_UP(nRasterXSize, fact * nValPowerOfTwo) == nOvrXSize)
                return fact * nValPowerOfTwo;
        }
        return nVal;
    }

    const int nVal =
        static_cast<int>(0.5 + nRasterYSize / static_cast<double>(nOvrYSize));
    // Try to return a power-of-two value
    const int nValPowerOfTwo = GetFloorPowerOfTwo(nVal);
    for (int fact = 1; fact <= 2 && nValPowerOfTwo <= INT_MAX / fact; ++fact)
    {
        if (DIV_ROUND_UP(nRasterYSize, fact * nValPowerOfTwo) == nOvrYSize)
            return fact * nValPowerOfTwo;
    }
    return nVal;
}

/************************************************************************/
/*                           CleanOverviews()                           */
/*                                                                      */
/*      Remove all existing overviews.                                  */
/************************************************************************/

CPLErr GDALDefaultOverviews::CleanOverviews()

{
    // Anything to do?
    if (poODS == nullptr)
        return CE_None;

    // Delete the overview file(s).
    GDALDriver *poOvrDriver = poODS->GetDriver();
    GDALClose(poODS);
    poODS = nullptr;

    CPLErr eErr =
        poOvrDriver != nullptr ? poOvrDriver->Delete(osOvrFilename) : CE_None;

    // Reset the saved overview filename.
    if (!EQUAL(poDS->GetDescription(), ":::VIRTUAL:::"))
    {
        const bool bUseRRD = CPLTestBool(CPLGetConfigOption("USE_RRD", "NO"));

        if (bUseRRD)
            osOvrFilename =
                CPLResetExtensionSafe(poDS->GetDescription(), "aux");
        else
            osOvrFilename = std::string(poDS->GetDescription()).append(".ovr");
    }
    else
    {
        osOvrFilename = "";
    }

    if (HaveMaskFile() && poMaskDS)
    {
        const CPLErr eErr2 = poMaskDS->BuildOverviews(
            nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
        if (eErr2 != CE_None)
            return eErr2;
    }

    return eErr;
}

/************************************************************************/
/*                      BuildOverviewsSubDataset()                      */
/************************************************************************/

CPLErr GDALDefaultOverviews::BuildOverviewsSubDataset(
    const char *pszPhysicalFile, const char *pszResampling, int nOverviews,
    const int *panOverviewList, int nBands, const int *panBandList,
    GDALProgressFunc pfnProgress, void *pProgressData,
    CSLConstList papszOptions)

{
    if (osOvrFilename.length() == 0 && nOverviews > 0)
    {
        VSIStatBufL sStatBuf;

        int iSequence = 0;  // Used after for.
        for (iSequence = 0; iSequence < 100; iSequence++)
        {
            osOvrFilename.Printf("%s_%d.ovr", pszPhysicalFile, iSequence);
            if (VSIStatExL(osOvrFilename, &sStatBuf, VSI_STAT_EXISTS_FLAG) != 0)
            {
                CPLString osAdjustedOvrFilename;

                if (poDS->GetMOFlags() & GMO_PAM_CLASS)
                {
                    osAdjustedOvrFilename.Printf(
                        ":::BASE:::%s_%d.ovr", CPLGetFilename(pszPhysicalFile),
                        iSequence);
                }
                else
                {
                    osAdjustedOvrFilename = osOvrFilename;
                }

                poDS->SetMetadataItem("OVERVIEW_FILE", osAdjustedOvrFilename,
                                      "OVERVIEWS");
                break;
            }
        }

        if (iSequence == 100)
            osOvrFilename = "";
    }

    return BuildOverviews(nullptr, pszResampling, nOverviews, panOverviewList,
                          nBands, panBandList, pfnProgress, pProgressData,
                          papszOptions);
}

/************************************************************************/
/*                           GetOptionValue()                           */
/************************************************************************/

static const char *GetOptionValue(CSLConstList papszOptions,
                                  const char *pszOptionKey,
                                  const char *pszConfigOptionKey)
{
    const char *pszVal =
        pszOptionKey ? CSLFetchNameValue(papszOptions, pszOptionKey) : nullptr;
    if (pszVal)
    {
        return pszVal;
    }
    pszVal = CSLFetchNameValue(papszOptions, pszConfigOptionKey);
    if (pszVal)
    {
        return pszVal;
    }
    pszVal = CPLGetConfigOption(pszConfigOptionKey, nullptr);
    return pszVal;
}

/************************************************************************/
/*                CheckSrcOverviewsConsistencyWithBase()                */
/************************************************************************/

/*static */ bool GDALDefaultOverviews::CheckSrcOverviewsConsistencyWithBase(
    GDALDataset *poFullResDS, const std::vector<GDALDataset *> &apoSrcOvrDS)
{
    const auto poThisCRS = poFullResDS->GetSpatialRef();
    GDALGeoTransform thisGT;
    const bool bThisHasGT = poFullResDS->GetGeoTransform(thisGT) == CE_None;
    for (auto *poSrcOvrDS : apoSrcOvrDS)
    {
        if (poSrcOvrDS->GetRasterXSize() > poFullResDS->GetRasterXSize() ||
            poSrcOvrDS->GetRasterYSize() > poFullResDS->GetRasterYSize())
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "AddOverviews(): at least one input dataset has dimensions "
                "larger than the full resolution dataset.");
            return false;
        }
        if (poSrcOvrDS->GetRasterXSize() == 0 ||
            poSrcOvrDS->GetRasterYSize() == 0)
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "AddOverviews(): at least one input dataset has one of its "
                "dimensions equal to 0.");
            return false;
        }
        if (poSrcOvrDS->GetRasterCount() != poFullResDS->GetRasterCount())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "AddOverviews(): at least one input dataset not the same "
                     "number of bands than the full resolution dataset.");
            return false;
        }
        if (poThisCRS)
        {
            if (const auto poOvrCRS = poSrcOvrDS->GetSpatialRef())
            {
                if (!poOvrCRS->IsSame(poThisCRS))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "AddOverviews(): at least one input dataset has "
                             "its CRS "
                             "different from the one of the full resolution "
                             "dataset.");
                    return false;
                }
            }
        }
        if (bThisHasGT)
        {
            GDALGeoTransform ovrGT;
            const bool bOvrHasGT =
                poSrcOvrDS->GetGeoTransform(ovrGT) == CE_None;
            const double dfOvrXRatio =
                static_cast<double>(poFullResDS->GetRasterXSize()) /
                poSrcOvrDS->GetRasterXSize();
            const double dfOvrYRatio =
                static_cast<double>(poFullResDS->GetRasterYSize()) /
                poSrcOvrDS->GetRasterYSize();
            if (bOvrHasGT && !(std::fabs(thisGT[0] - ovrGT[0]) <=
                                   0.5 * std::fabs(ovrGT[1]) &&
                               std::fabs(thisGT[1] - ovrGT[1] / dfOvrXRatio) <=
                                   0.1 * std::fabs(ovrGT[1]) &&
                               std::fabs(thisGT[2] - ovrGT[2] / dfOvrYRatio) <=
                                   0.1 * std::fabs(ovrGT[2]) &&
                               std::fabs(thisGT[3] - ovrGT[3]) <=
                                   0.5 * std::fabs(ovrGT[5]) &&
                               std::fabs(thisGT[4] - ovrGT[4] / dfOvrXRatio) <=
                                   0.1 * std::fabs(ovrGT[4]) &&
                               std::fabs(thisGT[5] - ovrGT[5] / dfOvrYRatio) <=
                                   0.1 * std::fabs(ovrGT[5])))
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "AddOverviews(): at least one input dataset has its "
                    "geospatial extent "
                    "different from the one of the full resolution dataset.");
                return false;
            }
        }
    }
    return true;
}

/************************************************************************/
/*                           AddOverviews()                             */
/************************************************************************/

CPLErr GDALDefaultOverviews::AddOverviews(
    [[maybe_unused]] const char *pszBasename,
    [[maybe_unused]] const std::vector<GDALDataset *> &apoSrcOvrDSIn,
    [[maybe_unused]] GDALProgressFunc pfnProgress,
    [[maybe_unused]] void *pProgressData,
    [[maybe_unused]] CSLConstList papszOptions)
{
#ifdef HAVE_TIFF
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    if (CreateOrOpenOverviewFile(pszBasename, papszOptions) != CE_None)
        return CE_Failure;

    if (bOvrIsAux)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "AddOverviews() not supported for .aux overviews");
        return CE_Failure;
    }

    if (!GDALDefaultOverviews::CheckSrcOverviewsConsistencyWithBase(
            poDS, apoSrcOvrDSIn))
        return CE_Failure;

    std::vector<GDALDataset *> apoSrcOvrDS = apoSrcOvrDSIn;
    // Sort overviews by descending size
    std::sort(apoSrcOvrDS.begin(), apoSrcOvrDS.end(),
              [](const GDALDataset *poDS1, const GDALDataset *poDS2)
              { return poDS1->GetRasterXSize() > poDS2->GetRasterXSize(); });

    auto poBand = poDS->GetRasterBand(1);
    if (!poBand)
        return CE_Failure;

    // Determine which overview levels must be created
    std::vector<std::pair<int, int>> anOverviewSizes;
    for (auto *poSrcOvrDS : apoSrcOvrDS)
    {
        bool bFound = false;
        for (int j = 0; j < poBand->GetOverviewCount(); j++)
        {
            GDALRasterBand *poOverview = poBand->GetOverview(j);
            if (poOverview && poOverview->GetDataset() &&
                poOverview->GetDataset() != poDS &&
                poOverview->GetXSize() == poSrcOvrDS->GetRasterXSize() &&
                poOverview->GetYSize() == poSrcOvrDS->GetRasterYSize())
            {
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            anOverviewSizes.emplace_back(poSrcOvrDS->GetRasterXSize(),
                                         poSrcOvrDS->GetRasterYSize());
        }
    }

    CPLErr eErr = CE_None;

    if (!anOverviewSizes.empty())
    {
        if (poODS != nullptr)
        {
            delete poODS;
            poODS = nullptr;
        }

        const int nBands = poDS->GetRasterCount();
        std::vector<GDALRasterBand *> apoBands;
        for (int i = 0; i < nBands; ++i)
            apoBands.push_back(poDS->GetRasterBand(i + 1));

        eErr = GTIFFBuildOverviewsEx(osOvrFilename, nBands, apoBands.data(),
                                     static_cast<int>(apoSrcOvrDS.size()),
                                     nullptr, anOverviewSizes.data(), "NONE",
                                     nullptr, GDALDummyProgress, nullptr);

        // Probe for proxy overview filename.
        if (eErr == CE_Failure)
        {
            const char *pszProxyOvrFilename =
                poDS->GetMetadataItem("FILENAME", "ProxyOverviewRequest");

            if (pszProxyOvrFilename != nullptr)
            {
                osOvrFilename = pszProxyOvrFilename;
                eErr = GTIFFBuildOverviewsEx(
                    osOvrFilename, nBands, apoBands.data(),
                    static_cast<int>(apoSrcOvrDS.size()), nullptr,
                    anOverviewSizes.data(), "NONE", nullptr, GDALDummyProgress,
                    nullptr);
            }
        }

        if (eErr == CE_None)
        {
            poODS = GDALDataset::Open(osOvrFilename,
                                      GDAL_OF_RASTER | GDAL_OF_UPDATE);
            if (poODS == nullptr)
                eErr = CE_Failure;
        }
    }

    // almost 0, but not 0 to please Coverity Scan
    double dfTotalPixels = std::numeric_limits<double>::min();
    for (const auto *poSrcOvrDS : apoSrcOvrDS)
    {
        dfTotalPixels += static_cast<double>(poSrcOvrDS->GetRasterXSize()) *
                         poSrcOvrDS->GetRasterYSize();
    }

    // Copy source datasets into target overview datasets
    double dfCurPixels = 0;
    for (auto *poSrcOvrDS : apoSrcOvrDS)
    {
        GDALDataset *poDstOvrDS = nullptr;
        for (int j = 0; eErr == CE_None && j < poBand->GetOverviewCount(); j++)
        {
            GDALRasterBand *poOverview = poBand->GetOverview(j);
            if (poOverview &&
                poOverview->GetXSize() == poSrcOvrDS->GetRasterXSize() &&
                poOverview->GetYSize() == poSrcOvrDS->GetRasterYSize())
            {
                poDstOvrDS = poOverview->GetDataset();
                break;
            }
        }
        if (poDstOvrDS)
        {
            const double dfThisPixels =
                static_cast<double>(poSrcOvrDS->GetRasterXSize()) *
                poSrcOvrDS->GetRasterYSize();
            void *pScaledProgressData = GDALCreateScaledProgress(
                dfCurPixels / dfTotalPixels,
                (dfCurPixels + dfThisPixels) / dfTotalPixels, pfnProgress,
                pProgressData);
            dfCurPixels += dfThisPixels;
            eErr = GDALDatasetCopyWholeRaster(GDALDataset::ToHandle(poSrcOvrDS),
                                              GDALDataset::ToHandle(poDstOvrDS),
                                              nullptr, GDALScaledProgress,
                                              pScaledProgressData);
            GDALDestroyScaledProgress(pScaledProgressData);
        }
    }

    return eErr;
#else
    CPLError(CE_Failure, CPLE_NotSupported,
             "AddOverviews() not supported due to GeoTIFF driver missing");
    return CE_Failure;
#endif
}

/************************************************************************/
/*                      CreateOrOpenOverviewFile()                      */
/************************************************************************/

CPLErr GDALDefaultOverviews::CreateOrOpenOverviewFile(const char *pszBasename,
                                                      CSLConstList papszOptions)
{

    /* -------------------------------------------------------------------- */
    /*      If we don't already have an overview file, we need to decide    */
    /*      what format to use.                                             */
    /* -------------------------------------------------------------------- */
    if (poODS == nullptr)
    {
        const char *pszUseRRD =
            GetOptionValue(papszOptions, nullptr, "USE_RRD");
        bOvrIsAux = pszUseRRD && CPLTestBool(pszUseRRD);
        if (bOvrIsAux)
        {
            osOvrFilename =
                CPLResetExtensionSafe(poDS->GetDescription(), "aux");

            VSIStatBufL sStatBuf;
            if (VSIStatExL(osOvrFilename, &sStatBuf, VSI_STAT_EXISTS_FLAG) == 0)
                osOvrFilename.Printf("%s.aux", poDS->GetDescription());
        }
    }
    /* -------------------------------------------------------------------- */
    /*      If we already have the overviews open, but they are             */
    /*      read-only, then try and reopen them read-write.                 */
    /* -------------------------------------------------------------------- */
    else if (poODS->GetAccess() == GA_ReadOnly)
    {
        GDALClose(poODS);
        poODS =
            GDALDataset::Open(osOvrFilename, GDAL_OF_RASTER | GDAL_OF_UPDATE);
        if (poODS == nullptr)
            return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      If a basename is provided, use it to override the internal      */
    /*      overview filename.                                              */
    /* -------------------------------------------------------------------- */
    if (pszBasename == nullptr && osOvrFilename.length() == 0)
        pszBasename = poDS->GetDescription();

    if (pszBasename != nullptr)
    {
        if (bOvrIsAux)
            osOvrFilename.Printf("%s.aux", pszBasename);
        else
            osOvrFilename.Printf("%s.ovr", pszBasename);
    }

    return CE_None;
}

/************************************************************************/
/*                           BuildOverviews()                           */
/************************************************************************/

CPLErr GDALDefaultOverviews::BuildOverviews(
    const char *pszBasename, const char *pszResampling, int nOverviews,
    const int *panOverviewList, int nBands, const int *panBandList,
    GDALProgressFunc pfnProgress, void *pProgressData,
    CSLConstList papszOptions)

{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    if (nOverviews == 0)
        return CleanOverviews();

    if (CreateOrOpenOverviewFile(pszBasename, papszOptions) != CE_None)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Our TIFF overview support currently only works safely if all    */
    /*      bands are handled at the same time.                             */
    /* -------------------------------------------------------------------- */
    if (!bOvrIsAux && nBands != poDS->GetRasterCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Generation of overviews in external TIFF currently only "
                 "supported when operating on all bands.  "
                 "Operation failed.");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish which of the overview levels we already have, and     */
    /*      which are new.  We assume that band 1 of the file is            */
    /*      representative.                                                 */
    /* -------------------------------------------------------------------- */
    GDALRasterBand *poBand = poDS->GetRasterBand(1);

    int nNewOverviews = 0;
    int *panNewOverviewList =
        static_cast<int *>(CPLCalloc(sizeof(int), nOverviews));
    double dfAreaNewOverviews = 0;
    double dfAreaRefreshedOverviews = 0;
    std::vector<bool> abValidLevel(nOverviews, true);
    std::vector<bool> abRequireRefresh(nOverviews, false);
    bool bFoundSinglePixelOverview = false;
    for (int i = 0; i < nOverviews && poBand != nullptr; i++)
    {
        // If we already have a 1x1 overview and this new one would result
        // in it too, then don't create it.
        if (bFoundSinglePixelOverview &&
            DIV_ROUND_UP(poBand->GetXSize(), panOverviewList[i]) == 1 &&
            DIV_ROUND_UP(poBand->GetYSize(), panOverviewList[i]) == 1)
        {
            abValidLevel[i] = false;
            continue;
        }

        for (int j = 0; j < poBand->GetOverviewCount(); j++)
        {
            GDALRasterBand *poOverview = poBand->GetOverview(j);
            if (poOverview == nullptr)
                continue;

            int nOvFactor =
                GDALComputeOvFactor(poOverview->GetXSize(), poBand->GetXSize(),
                                    poOverview->GetYSize(), poBand->GetYSize());

            if (nOvFactor == panOverviewList[i] ||
                nOvFactor == GDALOvLevelAdjust2(panOverviewList[i],
                                                poBand->GetXSize(),
                                                poBand->GetYSize()))
            {
                const auto osNewResampling =
                    GDALGetNormalizedOvrResampling(pszResampling);
                const char *pszExistingResampling =
                    poOverview->GetMetadataItem("RESAMPLING");
                if (pszExistingResampling &&
                    pszExistingResampling != osNewResampling)
                {
                    if (auto l_poODS = poOverview->GetDataset())
                    {
                        if (auto poDriver = l_poODS->GetDriver())
                        {
                            if (EQUAL(poDriver->GetDescription(), "GTiff"))
                            {
                                poOverview->SetMetadataItem(
                                    "RESAMPLING", osNewResampling.c_str());
                            }
                        }
                    }
                }

                abRequireRefresh[i] = true;
                break;
            }
        }

        if (abValidLevel[i])
        {
            const double dfArea =
                1.0 /
                (static_cast<double>(panOverviewList[i]) * panOverviewList[i]);
            dfAreaRefreshedOverviews += dfArea;
            if (!abRequireRefresh[i])
            {
                dfAreaNewOverviews += dfArea;
                panNewOverviewList[nNewOverviews++] = panOverviewList[i];
            }

            if (DIV_ROUND_UP(poBand->GetXSize(), panOverviewList[i]) == 1 &&
                DIV_ROUND_UP(poBand->GetYSize(), panOverviewList[i]) == 1)
            {
                bFoundSinglePixelOverview = true;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Build band list.                                                */
    /* -------------------------------------------------------------------- */
    GDALRasterBand **pahBands = static_cast<GDALRasterBand **>(
        CPLCalloc(sizeof(GDALRasterBand *), nBands));
    for (int i = 0; i < nBands; i++)
        pahBands[i] = poDS->GetRasterBand(panBandList[i]);

    /* -------------------------------------------------------------------- */
    /*      Build new overviews - Imagine.  Keep existing file open if      */
    /*      we have it.  But mark all overviews as in need of               */
    /*      regeneration, since HFAAuxBuildOverviews() doesn't actually     */
    /*      produce the imagery.                                            */
    /* -------------------------------------------------------------------- */

    CPLErr eErr = CE_None;

    void *pScaledOverviewWithoutMask = GDALCreateScaledProgress(
        0, (HaveMaskFile() && poMaskDS) ? double(nBands) / (nBands + 1) : 1,
        pfnProgress, pProgressData);

    const auto AvoidZero = [](double x)
    {
        if (x == 0)
            return 1.0;
        return x;
    };

    void *pScaledProgress = GDALCreateScaledProgress(
        0, dfAreaNewOverviews / AvoidZero(dfAreaRefreshedOverviews),
        GDALScaledProgress, pScaledOverviewWithoutMask);
    if (bOvrIsAux)
    {
#ifdef NO_HFA_SUPPORT
        CPLError(CE_Failure, CPLE_NotSupported,
                 "This build does not support creating .aux overviews");
        eErr = CE_Failure;
#else
        if (nNewOverviews == 0)
        {
            /* if we call HFAAuxBuildOverviews() with nNewOverviews == 0 */
            /* because that there's no new, this will wipe existing */
            /* overviews (#4831) */
            // eErr = CE_None;
        }
        else
        {
            eErr = HFAAuxBuildOverviews(
                osOvrFilename, poDS, &poODS, nBands, panBandList, nNewOverviews,
                panNewOverviewList, pszResampling, GDALScaledProgress,
                pScaledProgress, papszOptions);
        }

        // HFAAuxBuildOverviews doesn't actually generate overviews
        dfAreaNewOverviews = 0.0;
        for (int j = 0; j < nOverviews; j++)
        {
            if (abValidLevel[j])
                abRequireRefresh[j] = true;
        }
#endif
    }

    /* -------------------------------------------------------------------- */
    /*      Build new overviews - TIFF.  Close TIFF files while we          */
    /*      operate on it.                                                  */
    /* -------------------------------------------------------------------- */
    else
    {
        if (poODS != nullptr)
        {
            delete poODS;
            poODS = nullptr;
        }

#ifdef HAVE_TIFF
        eErr = GTIFFBuildOverviews(
            osOvrFilename, nBands, pahBands, nNewOverviews, panNewOverviewList,
            pszResampling, GDALScaledProgress, pScaledProgress, papszOptions);

        // Probe for proxy overview filename.
        if (eErr == CE_Failure)
        {
            const char *pszProxyOvrFilename =
                poDS->GetMetadataItem("FILENAME", "ProxyOverviewRequest");

            if (pszProxyOvrFilename != nullptr)
            {
                osOvrFilename = pszProxyOvrFilename;
                eErr = GTIFFBuildOverviews(osOvrFilename, nBands, pahBands,
                                           nNewOverviews, panNewOverviewList,
                                           pszResampling, GDALScaledProgress,
                                           pScaledProgress, papszOptions);
            }
        }

        if (eErr == CE_None)
        {
            poODS = GDALDataset::Open(osOvrFilename,
                                      GDAL_OF_RASTER | GDAL_OF_UPDATE);
            if (poODS == nullptr)
                eErr = CE_Failure;
        }
#else
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Cannot build TIFF overviews due to GeoTIFF driver missing");
        eErr = CE_Failure;
#endif
    }

    GDALDestroyScaledProgress(pScaledProgress);

    /* -------------------------------------------------------------------- */
    /*      Refresh old overviews that were listed.                         */
    /* -------------------------------------------------------------------- */
    GDALRasterBand **papoOverviewBands =
        static_cast<GDALRasterBand **>(CPLCalloc(sizeof(void *), nOverviews));

    for (int iBand = 0; iBand < nBands && eErr == CE_None; iBand++)
    {
        poBand = poDS->GetRasterBand(panBandList[iBand]);
        if (poBand == nullptr)
        {
            eErr = CE_Failure;
            break;
        }

        nNewOverviews = 0;
        std::vector<bool> abAlreadyUsedOverviewBand(poBand->GetOverviewCount(),
                                                    false);

        for (int i = 0; i < nOverviews; i++)
        {
            if (!abValidLevel[i] || !abRequireRefresh[i])
                continue;

            for (int j = 0; j < poBand->GetOverviewCount(); j++)
            {
                if (abAlreadyUsedOverviewBand[j])
                    continue;

                GDALRasterBand *poOverview = poBand->GetOverview(j);
                if (poOverview == nullptr)
                    continue;

                int bHasNoData = FALSE;
                double noDataValue = poBand->GetNoDataValue(&bHasNoData);

                if (bHasNoData)
                    poOverview->SetNoDataValue(noDataValue);

                const int nOvFactor = GDALComputeOvFactor(
                    poOverview->GetXSize(), poBand->GetXSize(),
                    poOverview->GetYSize(), poBand->GetYSize());

                if (nOvFactor == panOverviewList[i] ||
                    nOvFactor == GDALOvLevelAdjust2(panOverviewList[i],
                                                    poBand->GetXSize(),
                                                    poBand->GetYSize()))
                {
                    abAlreadyUsedOverviewBand[j] = true;
                    CPLAssert(nNewOverviews < poBand->GetOverviewCount());
                    papoOverviewBands[nNewOverviews++] = poOverview;
                    break;
                }
            }
        }

        if (nNewOverviews > 0)
        {
            const double dfOffset =
                dfAreaNewOverviews / AvoidZero(dfAreaRefreshedOverviews);
            const double dfScale = 1.0 - dfOffset;
            pScaledProgress = GDALCreateScaledProgress(
                dfOffset + dfScale * iBand / nBands,
                dfOffset + dfScale * (iBand + 1) / nBands, GDALScaledProgress,
                pScaledOverviewWithoutMask);
            eErr = GDALRegenerateOverviewsEx(
                GDALRasterBand::ToHandle(poBand), nNewOverviews,
                reinterpret_cast<GDALRasterBandH *>(papoOverviewBands),
                pszResampling, GDALScaledProgress, pScaledProgress,
                papszOptions);
            GDALDestroyScaledProgress(pScaledProgress);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup                                                         */
    /* -------------------------------------------------------------------- */
    CPLFree(papoOverviewBands);
    CPLFree(panNewOverviewList);
    CPLFree(pahBands);
    GDALDestroyScaledProgress(pScaledOverviewWithoutMask);

    /* -------------------------------------------------------------------- */
    /*      If we have a mask file, we need to build its overviews too.     */
    /* -------------------------------------------------------------------- */
    if (HaveMaskFile() && eErr == CE_None)
    {
        pScaledProgress = GDALCreateScaledProgress(
            double(nBands) / (nBands + 1), 1.0, pfnProgress, pProgressData);
        eErr = BuildOverviewsMask(pszResampling, nOverviews, panOverviewList,
                                  GDALScaledProgress, pScaledProgress,
                                  papszOptions);
        GDALDestroyScaledProgress(pScaledProgress);
    }

    /* -------------------------------------------------------------------- */
    /*      If we have an overview dataset, then mark all the overviews     */
    /*      with the base dataset  Used later for finding overviews         */
    /*      masks.  Uggg.                                                   */
    /* -------------------------------------------------------------------- */
    if (poODS)
    {
        const int nOverviewCount = GetOverviewCount(1);

        for (int iOver = 0; iOver < nOverviewCount; iOver++)
        {
            GDALRasterBand *poOtherBand = GetOverview(1, iOver);
            GDALDataset *poOverDS =
                poOtherBand != nullptr ? poOtherBand->GetDataset() : nullptr;

            if (poOverDS != nullptr)
            {
                poOverDS->oOvManager.poBaseDS = poDS;
                poOverDS->oOvManager.poDS = poOverDS;
            }
        }
    }

    return eErr;
}

/************************************************************************/
/*                          BuildOverviewsMask()                        */
/************************************************************************/

CPLErr GDALDefaultOverviews::BuildOverviewsMask(const char *pszResampling,
                                                int nOverviews,
                                                const int *panOverviewList,
                                                GDALProgressFunc pfnProgress,
                                                void *pProgressData,
                                                CSLConstList papszOptions)
{
    CPLErr eErr = CE_None;
    if (HaveMaskFile() && poMaskDS)
    {
        // Some options are not compatible with mask overviews
        // so unset them, and define more sensible values.
        CPLStringList aosMaskOptions(papszOptions);
        const char *pszCompress =
            GetOptionValue(papszOptions, "COMPRESS", "COMPRESS_OVERVIEW");
        const bool bJPEG = pszCompress && EQUAL(pszCompress, "JPEG");
        const char *pszPhotometric =
            GetOptionValue(papszOptions, "PHOTOMETRIC", "PHOTOMETRIC_OVERVIEW");
        const bool bPHOTOMETRIC_YCBCR =
            pszPhotometric && EQUAL(pszPhotometric, "YCBCR");
        if (bJPEG)
            aosMaskOptions.SetNameValue("COMPRESS", "DEFLATE");
        if (bPHOTOMETRIC_YCBCR)
            aosMaskOptions.SetNameValue("PHOTOMETRIC", "MINISBLACK");

        eErr = poMaskDS->BuildOverviews(
            pszResampling, nOverviews, panOverviewList, 0, nullptr, pfnProgress,
            pProgressData, aosMaskOptions.List());

        if (bOwnMaskDS)
        {
            // Reset the poMask member of main dataset bands, since it
            // will become invalid after poMaskDS closing.
            for (int iBand = 1; iBand <= poDS->GetRasterCount(); iBand++)
            {
                GDALRasterBand *poOtherBand = poDS->GetRasterBand(iBand);
                if (poOtherBand != nullptr)
                    poOtherBand->InvalidateMaskBand();
            }

            GDALClose(poMaskDS);
        }

        // force next request to reread mask file.
        poMaskDS = nullptr;
        bOwnMaskDS = false;
        bCheckedForMask = false;
    }

    return eErr;
}

/************************************************************************/
/*                           CreateMaskBand()                           */
/************************************************************************/

CPLErr GDALDefaultOverviews::CreateMaskBand(int nFlags, int nBand)

{
    if (nBand < 1)
        nFlags |= GMF_PER_DATASET;

    /* -------------------------------------------------------------------- */
    /*      ensure existing file gets opened if there is one.               */
    /* -------------------------------------------------------------------- */
    CPL_IGNORE_RET_VAL(HaveMaskFile());

    /* -------------------------------------------------------------------- */
    /*      Try creating the mask file.                                     */
    /* -------------------------------------------------------------------- */
    if (poMaskDS == nullptr)
    {
        GDALDriver *const poDr =
            static_cast<GDALDriver *>(GDALGetDriverByName("GTiff"));

        if (poDr == nullptr)
            return CE_Failure;

        GDALRasterBand *const poTBand = poDS->GetRasterBand(1);
        if (poTBand == nullptr)
            return CE_Failure;

        const int nBands =
            (nFlags & GMF_PER_DATASET) ? 1 : poDS->GetRasterCount();

        char **papszOpt = CSLSetNameValue(nullptr, "COMPRESS", "DEFLATE");
        papszOpt = CSLSetNameValue(papszOpt, "INTERLEAVE", "BAND");

        int nBX = 0;
        int nBY = 0;
        poTBand->GetBlockSize(&nBX, &nBY);

        // Try to create matching tile size if legal in TIFF.
        if ((nBX % 16) == 0 && (nBY % 16) == 0)
        {
            papszOpt = CSLSetNameValue(papszOpt, "TILED", "YES");
            papszOpt = CSLSetNameValue(papszOpt, "BLOCKXSIZE",
                                       CPLString().Printf("%d", nBX));
            papszOpt = CSLSetNameValue(papszOpt, "BLOCKYSIZE",
                                       CPLString().Printf("%d", nBY));
        }

        CPLString osMskFilename;
        osMskFilename.Printf("%s.msk", poDS->GetDescription());
        poMaskDS =
            poDr->Create(osMskFilename, poDS->GetRasterXSize(),
                         poDS->GetRasterYSize(), nBands, GDT_Byte, papszOpt);
        CSLDestroy(papszOpt);

        if (poMaskDS == nullptr)  // Presumably error already issued.
            return CE_Failure;

        bOwnMaskDS = true;
    }

    /* -------------------------------------------------------------------- */
    /*      Save the mask flags for this band.                              */
    /* -------------------------------------------------------------------- */
    if (nBand > poMaskDS->GetRasterCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to create a mask band for band %d of %s, "
                 "but the .msk file has a PER_DATASET mask.",
                 nBand, poDS->GetDescription());
        return CE_Failure;
    }

    for (int iBand = 0; iBand < poDS->GetRasterCount(); iBand++)
    {
        // we write only the info for this band, unless we are
        // using PER_DATASET in which case we write for all.
        if (nBand != iBand + 1 && !(nFlags & GMF_PER_DATASET))
            continue;

        poMaskDS->SetMetadataItem(
            CPLString().Printf("INTERNAL_MASK_FLAGS_%d", iBand + 1),
            CPLString().Printf("%d", nFlags));
    }

    return CE_None;
}

/************************************************************************/
/*                            GetMaskBand()                             */
/************************************************************************/

// Secret code meaning we don't handle this band.
constexpr int MISSING_FLAGS = 0x8000;

GDALRasterBand *GDALDefaultOverviews::GetMaskBand(int nBand)

{
    const int nFlags = GetMaskFlags(nBand);

    if (poMaskDS == nullptr || nFlags == MISSING_FLAGS)
        return nullptr;

    if (nFlags & GMF_PER_DATASET)
        return poMaskDS->GetRasterBand(1);

    if (nBand > 0)
        return poMaskDS->GetRasterBand(nBand);

    return nullptr;
}

/************************************************************************/
/*                            GetMaskFlags()                            */
/************************************************************************/

int GDALDefaultOverviews::GetMaskFlags(int nBand)

{
    /* -------------------------------------------------------------------- */
    /*      Fetch this band's metadata entry.  They are of the form:        */
    /*        INTERNAL_MASK_FLAGS_n: flags                                  */
    /* -------------------------------------------------------------------- */
    if (!HaveMaskFile())
        return 0;

    const char *pszValue = poMaskDS->GetMetadataItem(
        CPLString().Printf("INTERNAL_MASK_FLAGS_%d", std::max(nBand, 1)));

    if (pszValue == nullptr)
        return MISSING_FLAGS;

    return atoi(pszValue);
}

/************************************************************************/
/*                            HaveMaskFile()                            */
/*                                                                      */
/*      Check for a mask file if we haven't already done so.            */
/*      Returns TRUE if we have one, otherwise FALSE.                   */
/************************************************************************/

int GDALDefaultOverviews::HaveMaskFile(char **papszSiblingFiles,
                                       const char *pszBasename)

{
    /* -------------------------------------------------------------------- */
    /*      Have we already checked for masks?                              */
    /* -------------------------------------------------------------------- */
    if (bCheckedForMask)
        return poMaskDS != nullptr;

    if (papszSiblingFiles == nullptr)
        papszSiblingFiles = papszInitSiblingFiles;

    /* -------------------------------------------------------------------- */
    /*      Are we an overview?  If so we need to find the corresponding    */
    /*      overview in the base files mask file (if there is one).         */
    /* -------------------------------------------------------------------- */
    if (poBaseDS != nullptr && poBaseDS->oOvManager.HaveMaskFile())
    {
        GDALRasterBand *const poBaseBand = poBaseDS->GetRasterBand(1);
        GDALDataset *poMaskDSTemp = nullptr;
        if (poBaseBand != nullptr)
        {
            GDALRasterBand *poBaseMask = poBaseBand->GetMaskBand();
            if (poBaseMask != nullptr)
            {
                const int nOverviewCount = poBaseMask->GetOverviewCount();
                for (int iOver = 0; iOver < nOverviewCount; iOver++)
                {
                    GDALRasterBand *const poOverBand =
                        poBaseMask->GetOverview(iOver);
                    if (poOverBand == nullptr)
                        continue;

                    if (poOverBand->GetXSize() == poDS->GetRasterXSize() &&
                        poOverBand->GetYSize() == poDS->GetRasterYSize())
                    {
                        poMaskDSTemp = poOverBand->GetDataset();
                        break;
                    }
                }
            }
        }

        if (poMaskDSTemp != poDS)
        {
            poMaskDS = poMaskDSTemp;
            bCheckedForMask = true;
            bOwnMaskDS = false;

            return poMaskDS != nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Are we even initialized?  If not, we apparently don't want      */
    /*      to support overviews and masks.                                 */
    /* -------------------------------------------------------------------- */
    if (poDS == nullptr)
        return FALSE;

    /* -------------------------------------------------------------------- */
    /*      Check for .msk file.                                            */
    /* -------------------------------------------------------------------- */
    bCheckedForMask = true;

    if (pszBasename == nullptr)
        pszBasename = poDS->GetDescription();

    // Don't bother checking for masks of masks.
    if (EQUAL(CPLGetExtensionSafe(pszBasename).c_str(), "msk"))
        return FALSE;

    if (!GDALCanFileAcceptSidecarFile(pszBasename))
        return FALSE;
    CPLString osMskFilename;
    osMskFilename.Printf("%s.msk", pszBasename);

    std::vector<char> achMskFilename;
    achMskFilename.resize(osMskFilename.size() + 1);
    memcpy(&(achMskFilename[0]), osMskFilename.c_str(),
           osMskFilename.size() + 1);
    bool bExists =
        CPL_TO_BOOL(CPLCheckForFile(&achMskFilename[0], papszSiblingFiles));
    osMskFilename = &achMskFilename[0];

#if !defined(_WIN32)
    if (!bExists && !papszSiblingFiles)
    {
        osMskFilename.Printf("%s.MSK", pszBasename);
        memcpy(&(achMskFilename[0]), osMskFilename.c_str(),
               osMskFilename.size() + 1);
        bExists =
            CPL_TO_BOOL(CPLCheckForFile(&achMskFilename[0], papszSiblingFiles));
        osMskFilename = &achMskFilename[0];
    }
#endif

    if (!bExists)
        return FALSE;

    /* -------------------------------------------------------------------- */
    /*      Open the file.                                                  */
    /* -------------------------------------------------------------------- */
    poMaskDS = GDALDataset::Open(
        osMskFilename,
        GDAL_OF_RASTER | (poDS->GetAccess() == GA_Update ? GDAL_OF_UPDATE : 0),
        nullptr, nullptr, papszInitSiblingFiles);
    CPLAssert(poMaskDS != poDS);

    if (poMaskDS == nullptr)
        return FALSE;

    bOwnMaskDS = true;

    return TRUE;
}

/************************************************************************/
/*                    GDALGetNormalizedOvrResampling()                  */
/************************************************************************/

std::string GDALGetNormalizedOvrResampling(const char *pszResampling)
{
    if (pszResampling &&
        EQUAL(pszResampling, "AVERAGE_BIT2GRAYSCALE_MINISWHITE"))
        return "AVERAGE_BIT2GRAYSCALE_MINISWHITE";
    else if (pszResampling && STARTS_WITH_CI(pszResampling, "AVERAGE_BIT2"))
        return "AVERAGE_BIT2GRAYSCALE";
    else if (pszResampling && STARTS_WITH_CI(pszResampling, "NEAR"))
        return "NEAREST";
    else if (pszResampling && EQUAL(pszResampling, "AVERAGE_MAGPHASE"))
        return "AVERAGE_MAGPHASE";
    else if (pszResampling && STARTS_WITH_CI(pszResampling, "AVER"))
        return "AVERAGE";
    else if (pszResampling && !EQUAL(pszResampling, "NONE"))
    {
        return CPLString(pszResampling).toupper();
    }
    return std::string();
}

//! @endcond
