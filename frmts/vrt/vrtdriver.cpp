/******************************************************************************
 *
 * Project:  Virtual GDAL Datasets
 * Purpose:  Implementation of VRTDriver
 * Author:   Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "vrtdataset.h"

#include "cpl_minixml.h"
#include "cpl_string.h"
#include "gdal_alg_priv.h"
#include "gdal_frmts.h"
#include "vrtexpression.h"

#include <mutex>

/*! @cond Doxygen_Suppress */

/************************************************************************/
/*                             VRTDriver()                              */
/************************************************************************/

VRTDriver::VRTDriver() : papszSourceParsers(nullptr)
{
#if 0
    pDeserializerData = GDALRegisterTransformDeserializer(
        "WarpedOverviewTransformer",
        VRTWarpedOverviewTransform,
        VRTDeserializeWarpedOverviewTransformer );
#endif
}

/************************************************************************/
/*                             ~VRTDriver()                             */
/************************************************************************/

VRTDriver::~VRTDriver()

{
    CSLDestroy(papszSourceParsers);
    VRTDerivedRasterBand::Cleanup();
#if 0
    if(  pDeserializerData )
    {
        GDALUnregisterTransformDeserializer( pDeserializerData );
    }
#endif
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **VRTDriver::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALDriver::GetMetadataDomainList(), TRUE,
                                   "SourceParsers", nullptr);
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **VRTDriver::GetMetadata(const char *pszDomain)

{
    std::lock_guard oLock(m_oMutex);
    if (pszDomain && EQUAL(pszDomain, "SourceParsers"))
        return papszSourceParsers;

    return GDALDriver::GetMetadata(pszDomain);
}

/************************************************************************/
/*                            SetMetadata()                             */
/************************************************************************/

CPLErr VRTDriver::SetMetadata(char **papszMetadata, const char *pszDomain)

{
    std::lock_guard oLock(m_oMutex);
    if (pszDomain && EQUAL(pszDomain, "SourceParsers"))
    {
        m_oMapSourceParser.clear();
        CSLDestroy(papszSourceParsers);
        papszSourceParsers = CSLDuplicate(papszMetadata);
        return CE_None;
    }

    return GDALDriver::SetMetadata(papszMetadata, pszDomain);
}

/************************************************************************/
/*                          AddSourceParser()                           */
/************************************************************************/

void VRTDriver::AddSourceParser(const char *pszElementName,
                                VRTSourceParser pfnParser)

{
    m_oMapSourceParser[pszElementName] = pfnParser;

    // Below won't work on architectures with "capability pointers"

    char szPtrValue[128] = {'\0'};
    void *ptr;
    CPL_STATIC_ASSERT(sizeof(pfnParser) == sizeof(void *));
    memcpy(&ptr, &pfnParser, sizeof(void *));
    int nRet = CPLPrintPointer(szPtrValue, ptr, sizeof(szPtrValue));
    szPtrValue[nRet] = 0;

    papszSourceParsers =
        CSLSetNameValue(papszSourceParsers, pszElementName, szPtrValue);
}

/************************************************************************/
/*                            ParseSource()                             */
/************************************************************************/

VRTSource *VRTDriver::ParseSource(const CPLXMLNode *psSrc,
                                  const char *pszVRTPath,
                                  VRTMapSharedResources &oMapSharedSources)

{

    if (psSrc == nullptr || psSrc->eType != CXT_Element)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Corrupt or empty VRT source XML document.");
        return nullptr;
    }

    if (!m_oMapSourceParser.empty())
    {
        auto oIter = m_oMapSourceParser.find(psSrc->pszValue);
        if (oIter != m_oMapSourceParser.end())
        {
            return oIter->second(psSrc, pszVRTPath, oMapSharedSources);
        }
        return nullptr;
    }

    // Below won't work on architectures with "capability pointers"

    const char *pszParserFunc =
        CSLFetchNameValue(papszSourceParsers, psSrc->pszValue);
    if (pszParserFunc == nullptr)
        return nullptr;

    VRTSourceParser pfnParser;
    CPL_STATIC_ASSERT(sizeof(pfnParser) == sizeof(void *));
    void *ptr =
        CPLScanPointer(pszParserFunc, static_cast<int>(strlen(pszParserFunc)));
    memcpy(&pfnParser, &ptr, sizeof(void *));

    if (pfnParser == nullptr)
        return nullptr;

    return pfnParser(psSrc, pszVRTPath, oMapSharedSources);
}

/************************************************************************/
/*                           VRTCreateCopy()                            */
/************************************************************************/

static GDALDataset *VRTCreateCopy(const char *pszFilename, GDALDataset *poSrcDS,
                                  int /* bStrict */, char **papszOptions,
                                  GDALProgressFunc pfnProgress,
                                  void *pProgressData)
{
    CPLAssert(nullptr != poSrcDS);

    VRTDataset *poSrcVRTDS = nullptr;

    void *pHandle = poSrcDS->GetInternalHandle("VRT_DATASET");
    if (pHandle && poSrcDS->GetInternalHandle(nullptr) == nullptr)
    {
        poSrcVRTDS = static_cast<VRTDataset *>(pHandle);
    }
    else
    {
        poSrcVRTDS = dynamic_cast<VRTDataset *>(poSrcDS);
    }

    /* -------------------------------------------------------------------- */
    /*      If the source dataset is a virtual dataset then just write      */
    /*      it to disk as a special case to avoid extra layers of           */
    /*      indirection.                                                    */
    /* -------------------------------------------------------------------- */
    if (poSrcVRTDS)
    {

        /* --------------------------------------------------------------------
         */
        /*      Convert tree to a single block of XML text. */
        /* --------------------------------------------------------------------
         */
        char *pszVRTPath = CPLStrdup(CPLGetPathSafe(pszFilename).c_str());
        poSrcVRTDS->UnsetPreservedRelativeFilenames();
        CPLXMLNode *psDSTree = poSrcVRTDS->SerializeToXML(pszVRTPath);

        char *pszXML = CPLSerializeXMLTree(psDSTree);

        CPLDestroyXMLNode(psDSTree);

        CPLFree(pszVRTPath);

        /* --------------------------------------------------------------------
         */
        /*      Write to disk. */
        /* --------------------------------------------------------------------
         */
        GDALDataset *pCopyDS = nullptr;

        if (0 != strlen(pszFilename))
        {
            VSILFILE *fpVRT = VSIFOpenL(pszFilename, "wb");
            if (fpVRT == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Cannot create %s",
                         pszFilename);
                CPLFree(pszXML);
                return nullptr;
            }

            bool bRet = VSIFWriteL(pszXML, strlen(pszXML), 1, fpVRT) > 0;
            if (VSIFCloseL(fpVRT) != 0)
                bRet = false;

            if (bRet)
                pCopyDS = GDALDataset::Open(
                    pszFilename,
                    GDAL_OF_RASTER | GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE);
        }
        else
        {
            /* No destination file is given, so pass serialized XML directly. */
            pCopyDS = GDALDataset::Open(pszXML, GDAL_OF_RASTER |
                                                    GDAL_OF_MULTIDIM_RASTER |
                                                    GDAL_OF_UPDATE);
        }

        CPLFree(pszXML);

        return pCopyDS;
    }

    /* -------------------------------------------------------------------- */
    /*      Multidimensional raster ?                                       */
    /* -------------------------------------------------------------------- */
    auto poSrcGroup = poSrcDS->GetRootGroup();
    if (poSrcGroup != nullptr)
    {
        auto poDstDS = std::unique_ptr<GDALDataset>(
            VRTDataset::CreateMultiDimensional(pszFilename, nullptr, nullptr));
        if (!poDstDS)
            return nullptr;
        auto poDstGroup = poDstDS->GetRootGroup();
        if (!poDstGroup)
            return nullptr;
        if (GDALDriver::DefaultCreateCopyMultiDimensional(
                poSrcDS, poDstDS.get(), false, nullptr, nullptr, nullptr) !=
            CE_None)
            return nullptr;
        if (pfnProgress)
            pfnProgress(1.0, "", pProgressData);
        return poDstDS.release();
    }

    /* -------------------------------------------------------------------- */
    /*      Create the virtual dataset.                                     */
    /* -------------------------------------------------------------------- */
    auto poVRTDS = VRTDataset::CreateVRTDataset(
        pszFilename, poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(), 0,
        GDT_Byte, papszOptions);
    if (poVRTDS == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Do we have a geotransform?                                      */
    /* -------------------------------------------------------------------- */
    GDALGeoTransform gt;
    if (poSrcDS->GetGeoTransform(gt) == CE_None)
    {
        poVRTDS->SetGeoTransform(gt);
    }

    /* -------------------------------------------------------------------- */
    /*      Copy projection                                                 */
    /* -------------------------------------------------------------------- */
    poVRTDS->SetSpatialRef(poSrcDS->GetSpatialRef());

    /* -------------------------------------------------------------------- */
    /*      Emit dataset level metadata.                                    */
    /* -------------------------------------------------------------------- */
    const char *pszCopySrcMDD =
        CSLFetchNameValueDef(papszOptions, "COPY_SRC_MDD", "AUTO");
    char **papszSrcMDD = CSLFetchNameValueMultiple(papszOptions, "SRC_MDD");
    if (EQUAL(pszCopySrcMDD, "AUTO") || CPLTestBool(pszCopySrcMDD) ||
        papszSrcMDD)
    {
        if (!papszSrcMDD || CSLFindString(papszSrcMDD, "") >= 0 ||
            CSLFindString(papszSrcMDD, "_DEFAULT_") >= 0)
        {
            poVRTDS->SetMetadata(poSrcDS->GetMetadata());
        }

        /* -------------------------------------------------------------------- */
        /*      Copy any special domains that should be transportable.          */
        /* -------------------------------------------------------------------- */
        constexpr const char *apszDefaultDomains[] = {"RPC", "IMD",
                                                      "GEOLOCATION"};
        for (const char *pszDomain : apszDefaultDomains)
        {
            if (!papszSrcMDD || CSLFindString(papszSrcMDD, pszDomain) >= 0)
            {
                char **papszMD = poSrcDS->GetMetadata(pszDomain);
                if (papszMD)
                    poVRTDS->SetMetadata(papszMD, pszDomain);
            }
        }

        if ((!EQUAL(pszCopySrcMDD, "AUTO") && CPLTestBool(pszCopySrcMDD)) ||
            papszSrcMDD)
        {
            char **papszDomainList = poSrcDS->GetMetadataDomainList();
            constexpr const char *apszReservedDomains[] = {
                "IMAGE_STRUCTURE", "DERIVED_SUBDATASETS"};
            for (char **papszIter = papszDomainList; papszIter && *papszIter;
                 ++papszIter)
            {
                const char *pszDomain = *papszIter;
                if (pszDomain[0] != 0 &&
                    (!papszSrcMDD ||
                     CSLFindString(papszSrcMDD, pszDomain) >= 0))
                {
                    bool bCanCopy = true;
                    for (const char *pszOtherDomain : apszDefaultDomains)
                    {
                        if (EQUAL(pszDomain, pszOtherDomain))
                        {
                            bCanCopy = false;
                            break;
                        }
                    }
                    if (!papszSrcMDD)
                    {
                        for (const char *pszOtherDomain : apszReservedDomains)
                        {
                            if (EQUAL(pszDomain, pszOtherDomain))
                            {
                                bCanCopy = false;
                                break;
                            }
                        }
                    }
                    if (bCanCopy)
                    {
                        poVRTDS->SetMetadata(poSrcDS->GetMetadata(pszDomain),
                                             pszDomain);
                    }
                }
            }
            CSLDestroy(papszDomainList);
        }
    }
    CSLDestroy(papszSrcMDD);

    {
        const char *pszInterleave =
            poSrcDS->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE");
        if (pszInterleave)
        {
            poVRTDS->SetMetadataItem("INTERLEAVE", pszInterleave,
                                     "IMAGE_STRUCTURE");
        }
    }
    {
        const char *pszCompression =
            poSrcDS->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE");
        if (pszCompression)
        {
            poVRTDS->SetMetadataItem("COMPRESSION", pszCompression,
                                     "IMAGE_STRUCTURE");
        }
    }

    /* -------------------------------------------------------------------- */
    /*      GCPs                                                            */
    /* -------------------------------------------------------------------- */
    if (poSrcDS->GetGCPCount() > 0)
    {
        poVRTDS->SetGCPs(poSrcDS->GetGCPCount(), poSrcDS->GetGCPs(),
                         poSrcDS->GetGCPSpatialRef());
    }

    /* -------------------------------------------------------------------- */
    /*      Loop over all the bands.                                        */
    /* -------------------------------------------------------------------- */
    for (int iBand = 0; iBand < poSrcDS->GetRasterCount(); iBand++)
    {
        GDALRasterBand *poSrcBand = poSrcDS->GetRasterBand(iBand + 1);

        /* --------------------------------------------------------------------
         */
        /*      Create the band with the appropriate band type. */
        /* --------------------------------------------------------------------
         */
        CPLStringList aosAddBandOptions;
        int nBlockXSize = poVRTDS->GetBlockXSize();
        int nBlockYSize = poVRTDS->GetBlockYSize();
        if (!poVRTDS->IsBlockSizeSpecified())
        {
            poSrcBand->GetBlockSize(&nBlockXSize, &nBlockYSize);
        }
        aosAddBandOptions.SetNameValue("BLOCKXSIZE",
                                       CPLSPrintf("%d", nBlockXSize));
        aosAddBandOptions.SetNameValue("BLOCKYSIZE",
                                       CPLSPrintf("%d", nBlockYSize));
        poVRTDS->AddBand(poSrcBand->GetRasterDataType(), aosAddBandOptions);

        VRTSourcedRasterBand *poVRTBand = static_cast<VRTSourcedRasterBand *>(
            poVRTDS->GetRasterBand(iBand + 1));

        /* --------------------------------------------------------------------
         */
        /*      Setup source mapping. */
        /* --------------------------------------------------------------------
         */
        poVRTBand->AddSimpleSource(poSrcBand);

        /* --------------------------------------------------------------------
         */
        /*      Emit various band level metadata. */
        /* --------------------------------------------------------------------
         */
        poVRTBand->CopyCommonInfoFrom(poSrcBand);

        const char *pszCompression =
            poSrcBand->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE");
        if (pszCompression)
        {
            poVRTBand->SetMetadataItem("COMPRESSION", pszCompression,
                                       "IMAGE_STRUCTURE");
        }

        /* --------------------------------------------------------------------
         */
        /*      Add specific mask band. */
        /* --------------------------------------------------------------------
         */
        if ((poSrcBand->GetMaskFlags() &
             (GMF_PER_DATASET | GMF_ALL_VALID | GMF_NODATA)) == 0)
        {
            auto poVRTMaskBand = std::make_unique<VRTSourcedRasterBand>(
                poVRTDS.get(), 0, poSrcBand->GetMaskBand()->GetRasterDataType(),
                poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize());
            poVRTMaskBand->AddMaskBandSource(poSrcBand);
            poVRTBand->SetMaskBand(std::move(poVRTMaskBand));
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Add dataset mask band                                           */
    /* -------------------------------------------------------------------- */
    if (poSrcDS->GetRasterCount() != 0 &&
        poSrcDS->GetRasterBand(1) != nullptr &&
        poSrcDS->GetRasterBand(1)->GetMaskFlags() == GMF_PER_DATASET)
    {
        GDALRasterBand *poSrcBand = poSrcDS->GetRasterBand(1);
        auto poVRTMaskBand = std::make_unique<VRTSourcedRasterBand>(
            poVRTDS.get(), 0, poSrcBand->GetMaskBand()->GetRasterDataType(),
            poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize());
        poVRTMaskBand->AddMaskBandSource(poSrcBand);
        poVRTDS->SetMaskBand(std::move(poVRTMaskBand));
    }

    if (strcmp(pszFilename, "") != 0)
    {
        CPLErrorReset();
        poVRTDS->FlushCache(true);
        if (CPLGetLastErrorType() != CE_None)
        {
            poVRTDS.reset();
        }
    }

    if (pfnProgress)
        pfnProgress(1.0, "", pProgressData);

    return poVRTDS.release();
}

/************************************************************************/
/*                          GDALRegister_VRT()                          */
/************************************************************************/

void GDALRegister_VRT()

{
    if (GDALGetDriverByName("VRT") != nullptr)
        return;

    static std::once_flag flag;
    std::call_once(flag,
                   []()
                   {
                       // First register the pixel functions
                       GDALRegisterDefaultPixelFunc();

                       // Register functions for VRTProcessedDataset
                       GDALVRTRegisterDefaultProcessedDatasetFuncs();
                   });

    VRTDriver *poDriver = new VRTDriver();

    poDriver->SetDescription("VRT");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Virtual Raster");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "vrt");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/vrt.html");
    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONDATATYPES,
        "Byte Int8 Int16 UInt16 Int32 UInt32 Int64 UInt64 "
        "Float16 Float32 Float64 "
        "CInt16 CInt32 CFloat16 CFloat32 CFloat64");
    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>\n"
        "   <Option name='SUBCLASS' type='string-select' "
        "default='VRTDataset'>\n"
        "       <Value>VRTDataset</Value>\n"
        "       <Value>VRTWarpedDataset</Value>\n"
        "   </Option>\n"
        "   <Option name='BLOCKXSIZE' type='int' description='Block width'/>\n"
        "   <Option name='BLOCKYSIZE' type='int' description='Block height'/>\n"
        "</CreationOptionList>\n");

    poDriver->pfnCreateCopy = VRTCreateCopy;
    poDriver->pfnCreate = VRTDataset::Create;
    poDriver->pfnCreateMultiDimensional = VRTDataset::CreateMultiDimensional;

#ifndef NO_OPEN
    poDriver->pfnOpen = VRTDataset::Open;
    poDriver->pfnIdentify = VRTDataset::Identify;
    poDriver->pfnDelete = VRTDataset::Delete;

    poDriver->SetMetadataItem(
        GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "  <Option name='ROOT_PATH' type='string' description='Root path to "
        "evaluate "
        "relative paths inside the VRT. Mainly useful for inlined VRT, or "
        "in-memory "
        "VRT, where their own directory does not make sense'/>"
        "<Option name='NUM_THREADS' type='string' description="
        "'Number of worker threads for reading. Can be set to ALL_CPUS' "
        "default='ALL_CPUS'/>"
        "</OpenOptionList>");
#endif

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_COORDINATE_EPOCH, "YES");

    poDriver->SetMetadataItem(GDAL_DCAP_UPDATE, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_UPDATE_ITEMS,
                              "GeoTransform SRS GCPs NoData "
                              "ColorInterpretation "
                              "DatasetMetadata BandMetadata");

    const char *pszExpressionDialects = "ExpressionDialects";
#if defined(GDAL_VRT_ENABLE_MUPARSER) && defined(GDAL_VRT_ENABLE_EXPRTK)
    poDriver->SetMetadataItem(pszExpressionDialects, "muparser,exprtk");
#elif defined(GDAL_VRT_ENABLE_MUPARSER)
    poDriver->SetMetadataItem(pszExpressionDialects, "muparser");
#elif defined(GDAL_VRT_ENABLE_EXPRTK)
    poDriver->SetMetadataItem(pszExpressionDialects, "exprtk");
#else
    poDriver->SetMetadataItem(pszExpressionDialects, "none");
#endif

#ifdef GDAL_VRT_ENABLE_MUPARSER
    if (gdal::MuParserHasDefineFunUserData())
    {
        poDriver->SetMetadataItem("MUPARSER_HAS_DEFINE_FUN_USER_DATA", "YES");
    }
#endif

#ifdef GDAL_VRT_ENABLE_RAWRASTERBAND
    poDriver->SetMetadataItem("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES");
#endif

    poDriver->AddSourceParser("SimpleSource", VRTParseCoreSources);
    poDriver->AddSourceParser("ComplexSource", VRTParseCoreSources);
    poDriver->AddSourceParser("AveragedSource", VRTParseCoreSources);
    poDriver->AddSourceParser("NoDataFromMaskSource", VRTParseCoreSources);
    poDriver->AddSourceParser("KernelFilteredSource", VRTParseFilterSources);
    poDriver->AddSourceParser("ArraySource", VRTParseArraySource);

    GetGDALDriverManager()->RegisterDriver(poDriver);
}

/*! @endcond */
