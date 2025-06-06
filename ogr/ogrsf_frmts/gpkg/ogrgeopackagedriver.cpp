/******************************************************************************
 *
 * Project:  GeoPackage Translator
 * Purpose:  Implements GeoPackageDriver.
 * Author:   Paul Ramsey <pramsey@boundlessgeo.com>
 *
 ******************************************************************************
 * Copyright (c) 2013, Paul Ramsey <pramsey@boundlessgeo.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_geopackage.h"

#include "tilematrixset.hpp"
#include "gdalalgorithm.h"

#include <cctype>
#include <mutex>

// g++ -g -Wall -fPIC -shared -o ogr_geopackage.so -Iport -Igcore -Iogr
// -Iogr/ogrsf_frmts -Iogr/ogrsf_frmts/gpkg ogr/ogrsf_frmts/gpkg/*.c* -L. -lgdal

static inline bool ENDS_WITH_CI(const char *a, const char *b)
{
    return strlen(a) >= strlen(b) && EQUAL(a + strlen(a) - strlen(b), b);
}

/************************************************************************/
/*                       OGRGeoPackageDriverIdentify()                  */
/************************************************************************/

static int OGRGeoPackageDriverIdentify(GDALOpenInfo *poOpenInfo,
                                       std::string &osFilenameInGpkgZip,
                                       bool bEmitWarning)
{
    if (STARTS_WITH_CI(poOpenInfo->pszFilename, "GPKG:"))
        return TRUE;

#ifdef ENABLE_SQL_GPKG_FORMAT
    if (poOpenInfo->pabyHeader &&
        STARTS_WITH(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                    "-- SQL GPKG"))
    {
        return TRUE;
    }
#endif

    // Try to recognize "foo.gpkg.zip"
    const size_t nFilenameLen = strlen(poOpenInfo->pszFilename);
    if ((poOpenInfo->nOpenFlags & GDAL_OF_UPDATE) == 0 &&
        nFilenameLen > strlen(".gpkg.zip") &&
        !STARTS_WITH(poOpenInfo->pszFilename, "/vsizip/") &&
        EQUAL(poOpenInfo->pszFilename + nFilenameLen - strlen(".gpkg.zip"),
              ".gpkg.zip"))
    {
        int nCountGpkg = 0;
        const CPLStringList aosFiles(VSIReadDirEx(
            (std::string("/vsizip/") + poOpenInfo->pszFilename).c_str(), 1000));
        for (int i = 0; i < aosFiles.size(); ++i)
        {
            const size_t nLen = strlen(aosFiles[i]);
            if (nLen > strlen(".gpkg") &&
                EQUAL(aosFiles[i] + nLen - strlen(".gpkg"), ".gpkg"))
            {
                osFilenameInGpkgZip = aosFiles[i];
                nCountGpkg++;
                if (nCountGpkg == 2)
                    return FALSE;
            }
        }
        return nCountGpkg == 1;
    }

    if (poOpenInfo->nHeaderBytes < 100 || poOpenInfo->pabyHeader == nullptr ||
        !STARTS_WITH(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                     "SQLite format 3"))
    {
        return FALSE;
    }

    /* Requirement 3: File name has to end in "gpkg" */
    /* http://opengis.github.io/geopackage/#_file_extension_name */
    /* But be tolerant, if the GPKG application id is found, because some */
    /* producers don't necessarily honour that requirement (#6396) */
    const char *pszExt = poOpenInfo->osExtension.c_str();
    const bool bIsRecognizedExtension =
        EQUAL(pszExt, "GPKG") || EQUAL(pszExt, "GPKX");

    /* Requirement 2: application id */
    /* http://opengis.github.io/geopackage/#_file_format */
    /* Be tolerant since some datasets don't actually follow that requirement */
    GUInt32 nApplicationId;
    memcpy(&nApplicationId, poOpenInfo->pabyHeader + knApplicationIdPos, 4);
    nApplicationId = CPL_MSBWORD32(nApplicationId);
    GUInt32 nUserVersion;
    memcpy(&nUserVersion, poOpenInfo->pabyHeader + knUserVersionPos, 4);
    nUserVersion = CPL_MSBWORD32(nUserVersion);
    if (nApplicationId != GP10_APPLICATION_ID &&
        nApplicationId != GP11_APPLICATION_ID &&
        nApplicationId != GPKG_APPLICATION_ID)
    {
#ifdef DEBUG
        if (EQUAL(CPLGetFilename(poOpenInfo->pszFilename), ".cur_input"))
        {
            return FALSE;
        }
#endif
        if (!bIsRecognizedExtension)
            return FALSE;

        if (bEmitWarning)
        {
            GByte abySignature[4 + 1];
            memcpy(abySignature, poOpenInfo->pabyHeader + knApplicationIdPos,
                   4);
            abySignature[4] = '\0';

            /* Is this a GPxx version ? */
            const bool bWarn = CPLTestBool(CPLGetConfigOption(
                "GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", "YES"));
            if (bWarn)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "GPKG: bad application_id=0x%02X%02X%02X%02X on '%s'",
                         abySignature[0], abySignature[1], abySignature[2],
                         abySignature[3], poOpenInfo->pszFilename);
            }
            else
            {
                CPLDebug("GPKG",
                         "bad application_id=0x%02X%02X%02X%02X on '%s'",
                         abySignature[0], abySignature[1], abySignature[2],
                         abySignature[3], poOpenInfo->pszFilename);
            }
        }
    }
    else if (nApplicationId == GPKG_APPLICATION_ID &&
             // Accept any 102XX version
             !((nUserVersion >= GPKG_1_2_VERSION &&
                nUserVersion < GPKG_1_2_VERSION + 99) ||
               // Accept any 103XX version
               (nUserVersion >= GPKG_1_3_VERSION &&
                nUserVersion < GPKG_1_3_VERSION + 99) ||
               // Accept any 104XX version
               (nUserVersion >= GPKG_1_4_VERSION &&
                nUserVersion < GPKG_1_4_VERSION + 99)))
    {
#ifdef DEBUG
        if (EQUAL(CPLGetFilename(poOpenInfo->pszFilename), ".cur_input"))
        {
            return FALSE;
        }
#endif
        if (!bIsRecognizedExtension)
            return FALSE;

        if (bEmitWarning)
        {
            GByte abySignature[4 + 1];
            memcpy(abySignature, poOpenInfo->pabyHeader + knUserVersionPos, 4);
            abySignature[4] = '\0';

            const bool bWarn = CPLTestBool(CPLGetConfigOption(
                "GPKG_WARN_UNRECOGNIZED_APPLICATION_ID", "YES"));
            if (bWarn)
            {
                if (nUserVersion > GPKG_1_4_VERSION)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "This version of GeoPackage "
                             "user_version=0x%02X%02X%02X%02X "
                             "(%u, v%d.%d.%d) on '%s' may only be "
                             "partially supported",
                             abySignature[0], abySignature[1], abySignature[2],
                             abySignature[3], nUserVersion,
                             nUserVersion / 10000, (nUserVersion % 10000) / 100,
                             nUserVersion % 100, poOpenInfo->pszFilename);
                }
                else
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "GPKG: unrecognized user_version="
                             "0x%02X%02X%02X%02X (%u) on '%s'",
                             abySignature[0], abySignature[1], abySignature[2],
                             abySignature[3], nUserVersion,
                             poOpenInfo->pszFilename);
                }
            }
            else
            {
                if (nUserVersion > GPKG_1_4_VERSION)
                {
                    CPLDebug("GPKG",
                             "This version of GeoPackage "
                             "user_version=0x%02X%02X%02X%02X "
                             "(%u, v%d.%d.%d) on '%s' may only be "
                             "partially supported",
                             abySignature[0], abySignature[1], abySignature[2],
                             abySignature[3], nUserVersion,
                             nUserVersion / 10000, (nUserVersion % 10000) / 100,
                             nUserVersion % 100, poOpenInfo->pszFilename);
                }
                else
                {
                    CPLDebug("GPKG",
                             "unrecognized user_version=0x%02X%02X%02X%02X"
                             "(%u) on '%s'",
                             abySignature[0], abySignature[1], abySignature[2],
                             abySignature[3], nUserVersion,
                             poOpenInfo->pszFilename);
                }
            }
        }
    }
    else if (!bIsRecognizedExtension
#ifdef DEBUG
             && !EQUAL(CPLGetFilename(poOpenInfo->pszFilename), ".cur_input")
#endif
             && !(STARTS_WITH(poOpenInfo->pszFilename, "/vsizip/") &&
                  poOpenInfo->IsExtensionEqualToCI("zip")) &&
             !STARTS_WITH(poOpenInfo->pszFilename, "/vsigzip/"))
    {
        if (bEmitWarning)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "File %s has GPKG application_id, but non conformant file "
                     "extension",
                     poOpenInfo->pszFilename);
        }
    }

    if ((poOpenInfo->nOpenFlags & GDAL_OF_RASTER) != 0 &&
        ENDS_WITH_CI(poOpenInfo->pszFilename, ".gti.gpkg"))
    {
        // Most likely handled by GTI driver, but we can't be sure
        return GDAL_IDENTIFY_UNKNOWN;
    }

    return TRUE;
}

static int OGRGeoPackageDriverIdentify(GDALOpenInfo *poOpenInfo)
{
    std::string osIgnored;
    return OGRGeoPackageDriverIdentify(poOpenInfo, osIgnored, false);
}

/************************************************************************/
/*                    OGRGeoPackageDriverGetSubdatasetInfo()            */
/************************************************************************/

struct OGRGeoPackageDriverSubdatasetInfo : public GDALSubdatasetInfo
{
  public:
    explicit OGRGeoPackageDriverSubdatasetInfo(const std::string &fileName)
        : GDALSubdatasetInfo(fileName)
    {
    }

    // GDALSubdatasetInfo interface
  private:
    void parseFileName() override;
};

void OGRGeoPackageDriverSubdatasetInfo::parseFileName()
{
    if (!STARTS_WITH_CI(m_fileName.c_str(), "GPKG:"))
    {
        return;
    }

    CPLStringList aosParts{CSLTokenizeString2(m_fileName.c_str(), ":", 0)};
    const int iPartsCount{CSLCount(aosParts)};

    if (iPartsCount == 3 || iPartsCount == 4)
    {

        m_driverPrefixComponent = aosParts[0];

        int subdatasetIndex{2};
        const bool hasDriveLetter{
            strlen(aosParts[1]) == 1 &&
            std::isalpha(static_cast<unsigned char>(aosParts[1][0]))};

        // Check for drive letter
        if (iPartsCount == 4)
        {
            // Invalid
            if (!hasDriveLetter)
            {
                return;
            }
            m_pathComponent = aosParts[1];
            m_pathComponent.append(":");
            m_pathComponent.append(aosParts[2]);
            subdatasetIndex++;
        }
        else  // count is 3
        {
            if (hasDriveLetter)
            {
                return;
            }
            m_pathComponent = aosParts[1];
        }

        m_subdatasetComponent = aosParts[subdatasetIndex];
    }
}

static GDALSubdatasetInfo *
OGRGeoPackageDriverGetSubdatasetInfo(const char *pszFileName)
{
    if (STARTS_WITH_CI(pszFileName, "GPKG:"))
    {
        std::unique_ptr<GDALSubdatasetInfo> info =
            std::make_unique<OGRGeoPackageDriverSubdatasetInfo>(pszFileName);
        if (!info->GetSubdatasetComponent().empty() &&
            !info->GetPathComponent().empty())
        {
            return info.release();
        }
    }
    return nullptr;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

static GDALDataset *OGRGeoPackageDriverOpen(GDALOpenInfo *poOpenInfo)
{
    std::string osFilenameInGpkgZip;
    if (OGRGeoPackageDriverIdentify(poOpenInfo, osFilenameInGpkgZip, true) ==
        GDAL_IDENTIFY_FALSE)
        return nullptr;

    GDALGeoPackageDataset *poDS = new GDALGeoPackageDataset();

    if (!poDS->Open(poOpenInfo, osFilenameInGpkgZip))
    {
        delete poDS;
        poDS = nullptr;
    }

    return poDS;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

static GDALDataset *OGRGeoPackageDriverCreate(const char *pszFilename,
                                              int nXSize, int nYSize,
                                              int nBands, GDALDataType eDT,
                                              char **papszOptions)
{
    if (strcmp(pszFilename, ":memory:") != 0)
    {
        const size_t nFilenameLen = strlen(pszFilename);
        if (nFilenameLen > strlen(".gpkg.zip") &&
            !STARTS_WITH(pszFilename, "/vsizip/") &&
            EQUAL(pszFilename + nFilenameLen - strlen(".gpkg.zip"),
                  ".gpkg.zip"))
        {
            // do nothing
        }
        else
        {
            const std::string osExt = CPLGetExtensionSafe(pszFilename);
            const bool bIsRecognizedExtension =
                EQUAL(osExt.c_str(), "GPKG") || EQUAL(osExt.c_str(), "GPKX");
            if (!bIsRecognizedExtension)
            {
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "The filename extension should be 'gpkg' instead of '%s' "
                    "to conform to the GPKG specification.",
                    osExt.c_str());
            }
        }
    }

    GDALGeoPackageDataset *poDS = new GDALGeoPackageDataset();

    if (!poDS->Create(pszFilename, nXSize, nYSize, nBands, eDT, papszOptions))
    {
        delete poDS;
        poDS = nullptr;
    }

    return poDS;
}

/************************************************************************/
/*                               Delete()                               */
/************************************************************************/

static CPLErr OGRGeoPackageDriverDelete(const char *pszFilename)

{
    std::string osAuxXml(pszFilename);
    osAuxXml += ".aux.xml";
    VSIStatBufL sStat;
    if (VSIStatL(osAuxXml.c_str(), &sStat) == 0)
        CPL_IGNORE_RET_VAL(VSIUnlink(osAuxXml.c_str()));

    if (VSIUnlink(pszFilename) == 0)
        return CE_None;
    else
        return CE_Failure;
}

/************************************************************************/
/*                          GDALGPKGDriver                              */
/************************************************************************/

class GDALGPKGDriver final : public GDALDriver
{
    std::mutex m_oMutex{};
    bool m_bInitialized = false;

    void InitializeCreationOptionList();

  public:
    GDALGPKGDriver() = default;

    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override;

    char **GetMetadata(const char *pszDomain) override
    {
        std::lock_guard oLock(m_oMutex);
        InitializeCreationOptionList();
        return GDALDriver::GetMetadata(pszDomain);
    }
};

const char *GDALGPKGDriver::GetMetadataItem(const char *pszName,
                                            const char *pszDomain)
{
    std::lock_guard oLock(m_oMutex);
    if (EQUAL(pszName, GDAL_DMD_CREATIONOPTIONLIST))
    {
        InitializeCreationOptionList();
    }
    return GDALDriver::GetMetadataItem(pszName, pszDomain);
}

#define COMPRESSION_OPTIONS                                                    \
    "  <Option name='TILE_FORMAT' type='string-select' scope='raster' "        \
    "description='Format to use to create tiles' default='AUTO'>"              \
    "    <Value>AUTO</Value>"                                                  \
    "    <Value>PNG_JPEG</Value>"                                              \
    "    <Value>PNG</Value>"                                                   \
    "    <Value>PNG8</Value>"                                                  \
    "    <Value>JPEG</Value>"                                                  \
    "    <Value>WEBP</Value>"                                                  \
    "    <Value>TIFF</Value>"                                                  \
    "  </Option>"                                                              \
    "  <Option name='QUALITY' type='int' min='1' max='100' scope='raster' "    \
    "description='Quality for JPEG and WEBP tiles' default='75'/>"             \
    "  <Option name='ZLEVEL' type='int' min='1' max='9' scope='raster' "       \
    "description='DEFLATE compression level for PNG tiles' default='6'/>"      \
    "  <Option name='DITHER' type='boolean' scope='raster' "                   \
    "description='Whether to apply Floyd-Steinberg dithering (for "            \
    "TILE_FORMAT=PNG8)' default='NO'/>"

void GDALGPKGDriver::InitializeCreationOptionList()
{
    if (m_bInitialized)
        return;
    m_bInitialized = true;

    const char *pszCOBegin =
        "<CreationOptionList>"
        "  <Option name='RASTER_TABLE' type='string' scope='raster' "
        "description='Name of tile user table'/>"
        "  <Option name='APPEND_SUBDATASET' type='boolean' scope='raster' "
        "description='Set to YES to add a new tile user table to an existing "
        "GeoPackage instead of replacing it' default='NO'/>"
        "  <Option name='RASTER_IDENTIFIER' type='string' scope='raster' "
        "description='Human-readable identifier (e.g. short name)'/>"
        "  <Option name='RASTER_DESCRIPTION' type='string' scope='raster' "
        "description='Human-readable description'/>"
        "  <Option name='BLOCKSIZE' type='int' scope='raster' "
        "description='Block size in pixels' default='256' max='4096'/>"
        "  <Option name='BLOCKXSIZE' type='int' scope='raster' "
        "description='Block width in pixels' default='256' max='4096'/>"
        "  <Option name='BLOCKYSIZE' type='int' scope='raster' "
        "description='Block height in pixels' default='256' "
        "max='4096'/>" COMPRESSION_OPTIONS
        "  <Option name='TILING_SCHEME' type='string' scope='raster' "
        "description='Which tiling scheme to use: pre-defined value or custom "
        "inline/outline JSON definition' default='CUSTOM'>"
        "    <Value>CUSTOM</Value>"
        "    <Value>GoogleCRS84Quad</Value>"
        "    <Value>PseudoTMS_GlobalGeodetic</Value>"
        "    <Value>PseudoTMS_GlobalMercator</Value>";

    const char *pszCOEnd =
        "  </Option>"
        "  <Option name='ZOOM_LEVEL' type='integer' scope='raster' "
        "description='Zoom level of full resolution. Only "
        "used for TILING_SCHEME != CUSTOM' min='0' max='30'/>"
        "  <Option name='ZOOM_LEVEL_STRATEGY' type='string-select' "
        "scope='raster' description='Strategy to determine zoom level. Only "
        "used for TILING_SCHEME != CUSTOM' default='AUTO'>"
        "    <Value>AUTO</Value>"
        "    <Value>LOWER</Value>"
        "    <Value>UPPER</Value>"
        "  </Option>"
        "  <Option name='RESAMPLING' type='string-select' scope='raster' "
        "description='Resampling algorithm. Only used for TILING_SCHEME != "
        "CUSTOM' default='BILINEAR'>"
        "    <Value>NEAREST</Value>"
        "    <Value>BILINEAR</Value>"
        "    <Value>CUBIC</Value>"
        "    <Value>CUBICSPLINE</Value>"
        "    <Value>LANCZOS</Value>"
        "    <Value>MODE</Value>"
        "    <Value>AVERAGE</Value>"
        "  </Option>"
        "  <Option name='PRECISION' type='float' scope='raster' "
        "description='Smallest significant value. Only used for tiled gridded "
        "coverage datasets' default='1'/>"
        "  <Option name='UOM' type='string' scope='raster' description='Unit "
        "of Measurement. Only used for tiled gridded coverage datasets' />"
        "  <Option name='FIELD_NAME' type='string' scope='raster' "
        "description='Field name. Only used for tiled gridded coverage "
        "datasets' default='Height'/>"
        "  <Option name='QUANTITY_DEFINITION' type='string' scope='raster' "
        "description='Description of the field. Only used for tiled gridded "
        "coverage datasets' default='Height'/>"
        "  <Option name='GRID_CELL_ENCODING' type='string-select' "
        "scope='raster' description='Grid cell encoding. Only used for tiled "
        "gridded coverage datasets' default='grid-value-is-center'>"
        "     <Value>grid-value-is-center</Value>"
        "     <Value>grid-value-is-area</Value>"
        "     <Value>grid-value-is-corner</Value>"
        "  </Option>"
        "  <Option name='VERSION' type='string-select' description='Set "
        "GeoPackage version (for application_id and user_version fields)' "
        "default='AUTO'>"
        "     <Value>AUTO</Value>"
        "     <Value>1.0</Value>"
        "     <Value>1.1</Value>"
        "     <Value>1.2</Value>"
        "     <Value>1.3</Value>"
        "     <Value>1.4</Value>"
        "  </Option>"
        "  <Option name='DATETIME_FORMAT' type='string-select' "
        "description='How to encode DateTime not in UTC' default='WITH_TZ'>"
        "     <Value>WITH_TZ</Value>"
        "     <Value>UTC</Value>"
        "  </Option>"
#ifdef ENABLE_GPKG_OGR_CONTENTS
        "  <Option name='ADD_GPKG_OGR_CONTENTS' type='boolean' "
        "description='Whether to add a gpkg_ogr_contents table to keep feature "
        "count' default='YES'/>"
#endif
        "  <Option name='CRS_WKT_EXTENSION' type='boolean' "
        "description='Whether to create the database with the crs_wkt "
        "extension'/>"
        "  <Option name='METADATA_TABLES' type='boolean' "
        "description='Whether to create the metadata related system tables'/>"
        "</CreationOptionList>";

    std::string osOptions(pszCOBegin);
    const auto tmsList = gdal::TileMatrixSet::listPredefinedTileMatrixSets();
    for (const auto &tmsName : tmsList)
    {
        const auto poTM = gdal::TileMatrixSet::parse(tmsName.c_str());
        if (poTM && poTM->haveAllLevelsSameTopLeft() &&
            poTM->haveAllLevelsSameTileSize() &&
            poTM->hasOnlyPowerOfTwoVaryingScales() &&
            !poTM->hasVariableMatrixWidth())
        {
            osOptions += "    <Value>";
            osOptions += tmsName;
            osOptions += "</Value>";
        }
    }
    osOptions += pszCOEnd;

    SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, osOptions.c_str());
}

/************************************************************************/
/*                    OGRGeoPackageRepackAlgorithm                      */
/************************************************************************/

#ifndef _
#define _(x) x
#endif

class OGRGeoPackageRepackAlgorithm final : public GDALAlgorithm
{
  public:
    OGRGeoPackageRepackAlgorithm()
        : GDALAlgorithm("repack", "Repack/vacuum in-place a GeoPackage dataset",
                        "/drivers/vector/gpkg.html")
    {
        constexpr int type = GDAL_OF_RASTER | GDAL_OF_VECTOR | GDAL_OF_UPDATE;
        auto &arg =
            AddArg("dataset", 0, _("GeoPackage dataset"), &m_dataset, type)
                .SetPositional()
                .SetRequired();
        SetAutoCompleteFunctionForFilename(arg, type);
    }

  protected:
    bool RunImpl(GDALProgressFunc, void *) override;

  private:
    GDALArgDatasetValue m_dataset{};
};

bool OGRGeoPackageRepackAlgorithm::RunImpl(GDALProgressFunc, void *)
{
    auto poDS =
        dynamic_cast<GDALGeoPackageDataset *>(m_dataset.GetDatasetRef());
    if (!poDS)
    {
        ReportError(CE_Failure, CPLE_AppDefined, "%s is not a GeoPackage",
                    m_dataset.GetName().c_str());
        return false;
    }
    CPLErrorReset();
    delete poDS->ExecuteSQL("VACUUM", nullptr, nullptr);
    return CPLGetLastErrorType() == CE_None;
}

/************************************************************************/
/*               OGRGeoPackageDriverInstantiateAlgorithm()              */
/************************************************************************/

static GDALAlgorithm *
OGRGeoPackageDriverInstantiateAlgorithm(const std::vector<std::string> &aosPath)
{
    if (aosPath.size() == 1 && aosPath[0] == "repack")
    {
        return std::make_unique<OGRGeoPackageRepackAlgorithm>().release();
    }
    else
    {
        return nullptr;
    }
}

/************************************************************************/
/*                         RegisterOGRGeoPackage()                       */
/************************************************************************/

void RegisterOGRGeoPackage()
{
    if (GDALGetDriverByName("GPKG") != nullptr)
        return;

    GDALDriver *poDriver = new GDALGPKGDriver();

    poDriver->SetDescription("GPKG");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DELETE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DELETE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_REORDER_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CURVE_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_Z_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_SUBDATASETS, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS,
                              "NATIVE OGRSQL SQLITE");

    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "GeoPackage");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "gpkg gpkg.zip");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/vector/gpkg.html");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES,
                              "Byte Int16 UInt16 Float32");

    poDriver->SetMetadataItem(
        GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "  <Option name='LIST_ALL_TABLES' type='string-select' scope='vector' "
        "description='Whether all tables, including those non listed in "
        "gpkg_contents, should be listed' default='AUTO'>"
        "    <Value>AUTO</Value>"
        "    <Value>YES</Value>"
        "    <Value>NO</Value>"
        "  </Option>"
        "  <Option name='TABLE' type='string' scope='raster' description='Name "
        "of tile user-table'/>"
        "  <Option name='ZOOM_LEVEL' type='integer' scope='raster' "
        "description='Zoom level of full resolution. If not specified, maximum "
        "non-empty zoom level'/>"
        "  <Option name='BAND_COUNT' type='string-select' scope='raster' "
        "description='Number of raster bands (only for Byte data type)' "
        "default='AUTO'>"
        "    <Value>AUTO</Value>"
        "    <Value>1</Value>"
        "    <Value>2</Value>"
        "    <Value>3</Value>"
        "    <Value>4</Value>"
        "  </Option>"
        "  <Option name='MINX' type='float' scope='raster' "
        "description='Minimum X of area of interest'/>"
        "  <Option name='MINY' type='float' scope='raster' "
        "description='Minimum Y of area of interest'/>"
        "  <Option name='MAXX' type='float' scope='raster' "
        "description='Maximum X of area of interest'/>"
        "  <Option name='MAXY' type='float' scope='raster' "
        "description='Maximum Y of area of interest'/>"
        "  <Option name='USE_TILE_EXTENT' type='boolean' scope='raster' "
        "description='Use tile extent of content to determine area of "
        "interest' default='NO'/>"
        "  <Option name='WHERE' type='string' scope='raster' description='SQL "
        "WHERE clause to be appended to tile requests'/>" COMPRESSION_OPTIONS
        "  <Option name='PRELUDE_STATEMENTS' type='string' "
        "scope='raster,vector' description='SQL statement(s) to send on the "
        "SQLite connection before any other ones'/>"
        "  <Option name='NOLOCK' type='boolean' description='Whether the "
        "database should be opened in nolock mode'/>"
        "  <Option name='IMMUTABLE' type='boolean' description='Whether the "
        "database should be opened in immutable mode'/>"
        "</OpenOptionList>");

    poDriver->SetMetadataItem(
        GDAL_DS_LAYER_CREATIONOPTIONLIST,
        "<LayerCreationOptionList>"
        "  <Option name='LAUNDER' type='boolean' description='Whether layer "
        "and field names will be laundered.' default='NO'/>"
        "  <Option name='GEOMETRY_NAME' type='string' description='Name of "
        "geometry column.' default='geom' deprecated_alias='GEOMETRY_COLUMN'/>"
        "  <Option name='GEOMETRY_NULLABLE' type='boolean' "
        "description='Whether the values of the geometry column can be NULL' "
        "default='YES'/>"
        "  <Option name='SRID' type='integer' description='Forced srs_id of "
        "the "
        "entry in the gpkg_spatial_ref_sys table to point to'/>"
        "  <Option name='DISCARD_COORD_LSB' type='boolean' "
        "description='Whether the geometry coordinate precision should be used "
        "to set to zero non-significant least-significant bits of geometries. "
        "Helps when further compression is used' default='NO'/>"
        "  <Option name='UNDO_DISCARD_COORD_LSB_ON_READING' type='boolean' "
        "description='Whether to ask GDAL to take into coordinate precision to "
        "undo the effects of DISCARD_COORD_LSB' default='NO'/>"
        "  <Option name='FID' type='string' description='Name of the FID "
        "column to create' default='fid'/>"
        "  <Option name='OVERWRITE' type='boolean' description='Whether to "
        "overwrite an existing table with the layer name to be created' "
        "default='NO'/>"
        "  <Option name='PRECISION' type='boolean' description='Whether text "
        "fields created should keep the width' default='YES'/>"
        "  <Option name='TRUNCATE_FIELDS' type='boolean' description='Whether "
        "to truncate text content that exceeds maximum width' default='NO'/>"
        "  <Option name='SPATIAL_INDEX' type='boolean' description='Whether to "
        "create a spatial index' default='YES'/>"
        "  <Option name='IDENTIFIER' type='string' description='Identifier of "
        "the layer, as put in the contents table'/>"
        "  <Option name='DESCRIPTION' type='string' description='Description "
        "of the layer, as put in the contents table'/>"
        "  <Option name='ASPATIAL_VARIANT' type='string-select' "
        "description='How to register non spatial tables' "
        "default='GPKG_ATTRIBUTES'>"
        "     <Value>GPKG_ATTRIBUTES</Value>"
        "     <Value>NOT_REGISTERED</Value>"
        "  </Option>"
        "  <Option name='DATETIME_PRECISION' type='string-select' "
        "description='Number of components of datetime fields' "
        "default='AUTO'>"
        "     <Value>AUTO</Value>"
        "     <Value>MILLISECOND</Value>"
        "     <Value>SECOND</Value>"
        "     <Value>MINUTE</Value>"
        "  </Option>"
        "</LayerCreationOptionList>");

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                              "Integer Integer64 Real String Date DateTime "
                              "Binary");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATASUBTYPES,
                              "Boolean Int16 Float32 JSON");
    poDriver->SetMetadataItem(GDAL_DMD_CREATION_FIELD_DEFN_FLAGS,
                              "WidthPrecision Nullable Default Unique "
                              "Comment AlternativeName Domain");

    poDriver->SetMetadataItem(GDAL_DMD_ALTER_FIELD_DEFN_FLAGS,
                              "Name Type WidthPrecision Nullable Default "
                              "Unique Domain AlternativeName Comment");

    poDriver->SetMetadataItem(GDAL_DCAP_NOTNULL_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DEFAULT_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_UNIQUE_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_NOTNULL_GEOMFIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MULTIPLE_VECTOR_LAYERS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_FIELD_DOMAINS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_RELATIONSHIPS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_RELATIONSHIP, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DELETE_RELATIONSHIP, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_UPDATE_RELATIONSHIP, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_FLUSHCACHE_CONSISTENT_STATE, "YES");

    poDriver->SetMetadataItem(GDAL_DMD_RELATIONSHIP_FLAGS,
                              "ManyToMany Association");

    poDriver->SetMetadataItem(GDAL_DCAP_RENAME_LAYERS, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_CREATION_FIELD_DOMAIN_TYPES,
                              "Coded Range Glob");

    poDriver->SetMetadataItem(GDAL_DMD_ALTER_GEOM_FIELD_DEFN_FLAGS,
                              "Name SRS CoordinateEpoch");

    poDriver->SetMetadataItem(
        GDAL_DMD_RELATIONSHIP_RELATED_TABLE_TYPES,
        "features media simple_attributes attributes tiles");

    poDriver->SetMetadataItem(GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION, "YES");

#ifdef ENABLE_SQL_GPKG_FORMAT
    poDriver->SetMetadataItem("ENABLE_SQL_GPKG_FORMAT", "YES");
#endif
#ifdef SQLITE_HAS_COLUMN_METADATA
    poDriver->SetMetadataItem("SQLITE_HAS_COLUMN_METADATA", "YES");
#endif

    poDriver->SetMetadataItem(GDAL_DCAP_UPDATE, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_UPDATE_ITEMS,
                              "DatasetMetadata BandMetadata RasterValues "
                              "LayerMetadata Features");

    poDriver->pfnOpen = OGRGeoPackageDriverOpen;
    poDriver->pfnIdentify = OGRGeoPackageDriverIdentify;
    poDriver->pfnCreate = OGRGeoPackageDriverCreate;
    poDriver->pfnCreateCopy = GDALGeoPackageDataset::CreateCopy;
    poDriver->pfnDelete = OGRGeoPackageDriverDelete;
    poDriver->pfnGetSubdatasetInfoFunc = OGRGeoPackageDriverGetSubdatasetInfo;

    poDriver->pfnInstantiateAlgorithm = OGRGeoPackageDriverInstantiateAlgorithm;
    poDriver->DeclareAlgorithm({"repack"});

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
