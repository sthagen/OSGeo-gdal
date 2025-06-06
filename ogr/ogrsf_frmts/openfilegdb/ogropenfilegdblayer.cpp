/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements Open FileGDB OGR driver.
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_openfilegdb.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <limits>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minixml.h"
#include "cpl_quad_tree.h"
#include "cpl_string.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"
#include "ogrsf_frmts.h"
#include "filegdbtable.h"
#include "ogr_swq.h"
#include "filegdb_coordprec_read.h"

OGROpenFileGDBGeomFieldDefn::~OGROpenFileGDBGeomFieldDefn() = default;

OGROpenFileGDBFeatureDefn::~OGROpenFileGDBFeatureDefn() = default;

/************************************************************************/
/*                      OGROpenFileGDBLayer()                           */
/************************************************************************/

OGROpenFileGDBLayer::OGROpenFileGDBLayer(
    OGROpenFileGDBDataSource *poDS, const char *pszGDBFilename,
    const char *pszName, const std::string &osDefinition,
    const std::string &osDocumentation, bool bEditable,
    OGRwkbGeometryType eGeomType, const std::string &osParentDefinition)
    : m_poDS(poDS), m_osGDBFilename(pszGDBFilename), m_osName(pszName),
      m_bEditable(bEditable), m_osDefinition(osDefinition),
      m_osDocumentation(osDocumentation)
{
    // TODO(rouault): What error on compiler versions?  r33032 does not say.

    // We cannot initialize m_poFeatureDefn in above list. MSVC doesn't like
    // this to be used in initialization list.
    m_poFeatureDefn = new OGROpenFileGDBFeatureDefn(this, pszName, false);
    SetDescription(m_poFeatureDefn->GetName());
    m_poFeatureDefn->SetGeomType(wkbNone);
    m_poFeatureDefn->Reference();

    m_eGeomType = eGeomType;

    if (!m_osDefinition.empty())
    {
        BuildGeometryColumnGDBv10(osParentDefinition);
    }

    // bSealFields = false because we do lazy resolution of fields
    m_poFeatureDefn->Seal(/* bSealFields = */ false);
}

/************************************************************************/
/*                      OGROpenFileGDBLayer()                           */
/************************************************************************/

OGROpenFileGDBLayer::OGROpenFileGDBLayer(OGROpenFileGDBDataSource *poDS,
                                         const char *pszGDBFilename,
                                         const char *pszName,
                                         OGRwkbGeometryType eType,
                                         CSLConstList papszOptions)
    : m_poDS(poDS), m_osGDBFilename(pszGDBFilename), m_osName(pszName),
      m_aosCreationOptions(papszOptions), m_eGeomType(eType),
      m_bArcGISPro32OrLater(
          EQUAL(CSLFetchNameValueDef(papszOptions, "TARGET_ARCGIS_VERSION", ""),
                "ARCGIS_PRO_3_2_OR_LATER"))
{
}

/***********************************************************************/
/*                      ~OGROpenFileGDBLayer()                         */
/***********************************************************************/

OGROpenFileGDBLayer::~OGROpenFileGDBLayer()
{
    OGROpenFileGDBLayer::SyncToDisk();

    if (m_poFeatureDefn)
    {
        m_poFeatureDefn->UnsetLayer();
        m_poFeatureDefn->Release();
    }

    delete m_poLyrTable;

    delete m_poAttributeIterator;
    delete m_poIterMinMax;
    delete m_poSpatialIndexIterator;
    delete m_poCombinedIterator;
    if (m_pQuadTree != nullptr)
        CPLQuadTreeDestroy(m_pQuadTree);
    CPLFree(m_pahFilteredFeatures);
}

/************************************************************************/
/*                                 Close()                              */
/************************************************************************/

void OGROpenFileGDBLayer::Close()
{
    delete m_poLyrTable;
    m_poLyrTable = nullptr;
    m_bValidLayerDefn = FALSE;
}

/************************************************************************/
/*                     BuildGeometryColumnGDBv10()                      */
/************************************************************************/

int OGROpenFileGDBLayer::BuildGeometryColumnGDBv10(
    const std::string &osParentDefinition)
{
    CPLXMLNode *psTree = CPLParseXMLString(m_osDefinition.c_str());
    if (psTree == nullptr)
    {
        m_osDefinition = "";
        return FALSE;
    }

    CPLStripXMLNamespace(psTree, nullptr, TRUE);
    /* CPLSerializeXMLTreeToFile( psTree, "/dev/stderr" ); */
    const CPLXMLNode *psInfo = CPLSearchXMLNode(psTree, "=DEFeatureClassInfo");
    if (psInfo == nullptr)
        psInfo = CPLSearchXMLNode(psTree, "=DETableInfo");
    if (psInfo == nullptr)
    {
        m_osDefinition = "";
        CPLDestroyXMLNode(psTree);
        return FALSE;
    }

    const char *pszAliasName = CPLGetXMLValue(psInfo, "AliasName", nullptr);
    if (pszAliasName && strcmp(pszAliasName, GetDescription()) != 0)
    {
        SetMetadataItem("ALIAS_NAME", pszAliasName);
    }

    m_bTimeInUTC = CPLTestBool(CPLGetXMLValue(psInfo, "IsTimeInUTC", "false"));

    /* We cannot trust the XML definition to build the field definitions. */
    /* It sometimes misses a few fields ! */

    const bool bHasZ = CPLTestBool(CPLGetXMLValue(psInfo, "HasZ", "NO"));
    const bool bHasM = CPLTestBool(CPLGetXMLValue(psInfo, "HasM", "NO"));
    const char *pszShapeType = CPLGetXMLValue(psInfo, "ShapeType", nullptr);
    const char *pszShapeFieldName =
        CPLGetXMLValue(psInfo, "ShapeFieldName", nullptr);
    if (pszShapeType != nullptr && pszShapeFieldName != nullptr)
    {
        m_eGeomType =
            FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI(pszShapeType);

        if (EQUAL(pszShapeType, "esriGeometryMultiPatch"))
        {
            if (m_poLyrTable == nullptr)
            {
                m_poLyrTable = new FileGDBTable();
                if (!(m_poLyrTable->Open(m_osGDBFilename, m_bEditable,
                                         GetDescription())))
                {
                    Close();
                }
            }
            if (m_poLyrTable != nullptr)
            {
                m_iGeomFieldIdx = m_poLyrTable->GetGeomFieldIdx();
                if (m_iGeomFieldIdx >= 0)
                {
                    FileGDBGeomField *poGDBGeomField =
                        reinterpret_cast<FileGDBGeomField *>(
                            m_poLyrTable->GetField(m_iGeomFieldIdx));
                    m_poGeomConverter.reset(
                        FileGDBOGRGeometryConverter::BuildConverter(
                            poGDBGeomField));
                    TryToDetectMultiPatchKind();
                }
            }
        }

        if (bHasZ)
            m_eGeomType = wkbSetZ(m_eGeomType);
        if (bHasM)
            m_eGeomType = wkbSetM(m_eGeomType);

        auto poGeomFieldDefn = std::make_unique<OGROpenFileGDBGeomFieldDefn>(
            nullptr, pszShapeFieldName, m_eGeomType);

        const CPLXMLNode *psGPFieldInfoExs =
            CPLGetXMLNode(psInfo, "GPFieldInfoExs");
        if (psGPFieldInfoExs)
        {
            for (const CPLXMLNode *psChild = psGPFieldInfoExs->psChild; psChild;
                 psChild = psChild->psNext)
            {
                if (psChild->eType != CXT_Element)
                    continue;
                if (EQUAL(psChild->pszValue, "GPFieldInfoEx") &&
                    EQUAL(CPLGetXMLValue(psChild, "Name", ""),
                          pszShapeFieldName))
                {
                    poGeomFieldDefn->SetNullable(CPLTestBool(
                        CPLGetXMLValue(psChild, "IsNullable", "TRUE")));
                    break;
                }
            }
        }

        const CPLXMLNode *psSpatialReference =
            CPLGetXMLNode(psInfo, "SpatialReference");
        if (psSpatialReference)
        {
            poGeomFieldDefn->SetCoordinatePrecision(
                GDBGridSettingsToOGR(psSpatialReference));
        }

        OGRSpatialReference *poParentSRS = nullptr;
        if (!osParentDefinition.empty())
        {
            CPLXMLNode *psParentTree =
                CPLParseXMLString(osParentDefinition.c_str());
            if (psParentTree != nullptr)
            {
                CPLStripXMLNamespace(psParentTree, nullptr, TRUE);
                CPLXMLNode *psParentInfo =
                    CPLSearchXMLNode(psParentTree, "=DEFeatureDataset");
                if (psParentInfo != nullptr)
                {
                    poParentSRS = m_poDS->BuildSRS(psParentInfo);
                }
                CPLDestroyXMLNode(psParentTree);
            }
            if (poParentSRS == nullptr)
            {
                CPLDebug("OpenFileGDB", "Cannot get SRS from feature dataset");
            }
        }

        auto poSRS = m_poDS->BuildSRS(psInfo);
        if (poParentSRS)
        {
            if (poSRS)
            {
                if (!poSRS->IsSame(poParentSRS))
                {
                    // Not sure this situation is really valid (seems more a
                    // bug of the editing software), but happens with
                    // https://github.com/OSGeo/gdal/issues/5747
                    // In the situation of
                    // https://github.com/OSGeo/gdal/issues/5747, the SRS inside
                    // the .gdbtable is consistent with the XML definition of
                    // the feature dataset, so it seems that the XML
                    // definition of the feature table lacked an update.
                    CPLDebug(
                        "OpenFileGDB",
                        "Table %s declare a CRS '%s' in its XML definition, "
                        "but its feature dataset declares '%s'. "
                        "Using the later",
                        GetDescription(), poSRS->GetName(),
                        poParentSRS->GetName());
                }
                poSRS->Release();
            }
            // Always use the SRS of the feature dataset
            poSRS = poParentSRS;
        }
        if (poSRS != nullptr)
        {
            poGeomFieldDefn->SetSpatialRef(poSRS);
            poSRS->Dereference();
        }
        m_poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
    }
    else
    {
        m_eGeomType = wkbNone;
    }
    CPLDestroyXMLNode(psTree);

    return TRUE;
}

/************************************************************************/
/*                   TryToDetectMultiPatchKind()                        */
/************************************************************************/

// If the first and last feature have the same geometry type, then use
// it for the whole layer.
void OGROpenFileGDBLayer::TryToDetectMultiPatchKind()
{
    CPLAssert(m_poLyrTable != nullptr);
    CPLAssert(m_iGeomFieldIdx >= 0);

    if (m_poLyrTable->GetTotalRecordCount() == 0)
        return;
    const int64_t nFirstIdx = m_poLyrTable->GetAndSelectNextNonEmptyRow(0);
    if (nFirstIdx < 0)
        return;

    const OGRField *psField = m_poLyrTable->GetFieldValue(m_iGeomFieldIdx);
    if (psField == nullptr)
        return;
    OGRGeometry *poGeom = m_poGeomConverter->GetAsGeometry(psField);
    if (poGeom == nullptr)
        return;
    const OGRwkbGeometryType eType = poGeom->getGeometryType();
    delete poGeom;

    int64_t nLastIdx = m_poLyrTable->GetTotalRecordCount() - 1;
    const GUInt32 nErrorCount = CPLGetErrorCounter();
    while (nLastIdx > nFirstIdx &&
           m_poLyrTable->GetOffsetInTableForRow(nLastIdx) == 0 &&
           nErrorCount == CPLGetErrorCounter())
    {
        nLastIdx--;
    }
    if (nLastIdx > nFirstIdx && m_poLyrTable->SelectRow(nLastIdx))
    {
        psField = m_poLyrTable->GetFieldValue(m_iGeomFieldIdx);
        if (psField == nullptr)
        {
            m_eGeomType = eType;
            return;
        }
        poGeom = m_poGeomConverter->GetAsGeometry(psField);
        if (poGeom == nullptr)
        {
            m_eGeomType = eType;
            return;
        }
        if (eType == poGeom->getGeometryType())
            m_eGeomType = eType;
        delete poGeom;
    }
}

/************************************************************************/
/*                      BuildLayerDefinition()                          */
/************************************************************************/

int OGROpenFileGDBLayer::BuildLayerDefinition()
{
    if (m_bValidLayerDefn >= 0)
        return m_bValidLayerDefn;

    if (m_poLyrTable == nullptr)
    {
        m_poLyrTable = new FileGDBTable();
        if (!(m_poLyrTable->Open(m_osGDBFilename, m_bEditable,
                                 GetDescription())))
        {
            if (m_bEditable)
            {
                // Retry in read-only mode
                m_bEditable = false;
                delete m_poLyrTable;
                m_poLyrTable = new FileGDBTable();
                if (!(m_poLyrTable->Open(m_osGDBFilename, m_bEditable,
                                         GetDescription())))
                {
                    Close();
                    return FALSE;
                }
                else
                {
                    CPLError(
                        CE_Failure, CPLE_FileIO,
                        "Cannot open %s in update mode, but only in read-only",
                        GetDescription());
                }
            }
            else
            {
                Close();
                return FALSE;
            }
        }
    }

    m_bValidLayerDefn = TRUE;
    auto oTemporaryUnsealer(m_poFeatureDefn->GetTemporaryUnsealer());

    m_iGeomFieldIdx = m_poLyrTable->GetGeomFieldIdx();
    if (m_iGeomFieldIdx >= 0)
    {
        FileGDBGeomField *poGDBGeomField = cpl::down_cast<FileGDBGeomField *>(
            m_poLyrTable->GetField(m_iGeomFieldIdx));
        m_poGeomConverter.reset(
            FileGDBOGRGeometryConverter::BuildConverter(poGDBGeomField));

#ifdef DEBUG
        const auto poSRS = GetSpatialRef();
        if (poSRS != nullptr && !poGDBGeomField->GetWKT().empty() &&
            poGDBGeomField->GetWKT()[0] != '{')
        {
            auto poSRSFromGDBTable =
                m_poDS->BuildSRS(poGDBGeomField->GetWKT().c_str());
            if (poSRSFromGDBTable)
            {
                if (!poSRS->IsSame(poSRSFromGDBTable))
                {
                    CPLDebug("OpenFileGDB",
                             "Table %s declare a CRS '%s' in its XML "
                             "definition (or in its parent's one), "
                             "but its .gdbtable declares '%s'. "
                             "Using the former",
                             GetDescription(), poSRS->GetName(),
                             poSRSFromGDBTable->GetName());
                }
                poSRSFromGDBTable->Release();
            }
        }
#endif

        if (!(m_poLyrTable->CanUseIndices() &&
              m_poLyrTable->HasSpatialIndex() &&
              CPLTestBool(CPLGetConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX",
                                             "YES"))) &&
            CPLTestBool(CPLGetConfigOption("OPENFILEGDB_IN_MEMORY_SPI", "YES")))
        {
            CPLRectObj sGlobalBounds;
            sGlobalBounds.minx = poGDBGeomField->GetXMin();
            sGlobalBounds.miny = poGDBGeomField->GetYMin();
            sGlobalBounds.maxx = poGDBGeomField->GetXMax();
            sGlobalBounds.maxy = poGDBGeomField->GetYMax();
            m_pQuadTree = CPLQuadTreeCreate(&sGlobalBounds, nullptr);
            CPLQuadTreeSetMaxDepth(
                m_pQuadTree,
                CPLQuadTreeGetAdvisedMaxDepth(
                    static_cast<int>(std::min<int64_t>(
                        INT_MAX, m_poLyrTable->GetValidRecordCount()))));
        }
        else
        {
            m_eSpatialIndexState = SPI_INVALID;
        }
    }

    if (m_iGeomFieldIdx >= 0 &&
        (m_osDefinition.empty() ||
         m_poFeatureDefn->OGRFeatureDefn::GetGeomFieldCount() == 0))
    {
        /* FileGDB v9 case */
        FileGDBGeomField *poGDBGeomField = reinterpret_cast<FileGDBGeomField *>(
            m_poLyrTable->GetField(m_iGeomFieldIdx));
        const char *pszName = poGDBGeomField->GetName().c_str();
        const FileGDBTableGeometryType eGDBGeomType =
            m_poLyrTable->GetGeometryType();

        OGRwkbGeometryType eGeomType = wkbUnknown;
        switch (eGDBGeomType)
        {
            case FGTGT_NONE: /* doesn't make sense ! */
                break;
            case FGTGT_POINT:
                eGeomType = wkbPoint;
                break;
            case FGTGT_MULTIPOINT:
                eGeomType = wkbMultiPoint;
                break;
            case FGTGT_LINE:
                eGeomType = wkbMultiLineString;
                break;
            case FGTGT_POLYGON:
                eGeomType = wkbMultiPolygon;
                break;
            case FGTGT_MULTIPATCH:
                eGeomType = wkbUnknown;
                break;
        }

        if (m_eGeomType != wkbUnknown &&
            wkbFlatten(eGeomType) != wkbFlatten(m_eGeomType))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Inconsistency for layer geometry type");
        }

        m_eGeomType = eGeomType;

        if (eGDBGeomType == FGTGT_MULTIPATCH)
        {
            TryToDetectMultiPatchKind();
        }

        if (m_poLyrTable->GetGeomTypeHasZ())
            m_eGeomType = wkbSetZ(m_eGeomType);

        if (m_poLyrTable->GetGeomTypeHasM())
            m_eGeomType = wkbSetM(m_eGeomType);

        {
            auto poGeomFieldDefn =
                std::make_unique<OGROpenFileGDBGeomFieldDefn>(nullptr, pszName,
                                                              m_eGeomType);
            poGeomFieldDefn->SetNullable(poGDBGeomField->IsNullable());

            m_poFeatureDefn->AddGeomFieldDefn(std::move(poGeomFieldDefn));
        }
        auto poGeomFieldDefn = m_poFeatureDefn->GetGeomFieldDefn(0);

        OGRSpatialReference *poSRS = nullptr;
        if (!poGDBGeomField->GetWKT().empty() &&
            poGDBGeomField->GetWKT()[0] != '{')
        {
            poSRS = m_poDS->BuildSRS(poGDBGeomField->GetWKT().c_str());
        }
        if (poSRS != nullptr)
        {
            poGeomFieldDefn->SetSpatialRef(poSRS);
            poSRS->Dereference();
        }
    }
    else if (m_osDefinition.empty() && m_iGeomFieldIdx < 0)
    {
        m_eGeomType = wkbNone;
    }

    CPLXMLTreeCloser oTree(nullptr);
    const CPLXMLNode *psGPFieldInfoExs = nullptr;

    std::string osAreaFieldName;
    std::string osLengthFieldName;
    if (!m_osDefinition.empty())
    {
        oTree.reset(CPLParseXMLString(m_osDefinition.c_str()));
        if (oTree != nullptr)
        {
            CPLStripXMLNamespace(oTree.get(), nullptr, TRUE);
            CPLXMLNode *psInfo =
                CPLSearchXMLNode(oTree.get(), "=DEFeatureClassInfo");
            if (psInfo == nullptr)
                psInfo = CPLSearchXMLNode(oTree.get(), "=DETableInfo");
            if (psInfo != nullptr)
            {
                psGPFieldInfoExs = CPLGetXMLNode(psInfo, "GPFieldInfoExs");
                osAreaFieldName = CPLGetXMLValue(psInfo, "AreaFieldName", "");
                osLengthFieldName =
                    CPLGetXMLValue(psInfo, "LengthFieldName", "");
                m_osPath = CPLGetXMLValue(psInfo, "CatalogPath", "");
            }
        }
    }

    for (int i = 0; i < m_poLyrTable->GetFieldCount(); i++)
    {
        if (i == m_iGeomFieldIdx)
            continue;
        if (i == m_poLyrTable->GetObjectIdFieldIdx())
            continue;

        FileGDBField *poGDBField = m_poLyrTable->GetField(i);
        OGRFieldType eType = OFTString;
        OGRFieldSubType eSubType = OFSTNone;
        int nWidth = poGDBField->GetMaxWidth();
        switch (poGDBField->GetType())
        {
            case FGFT_INT16:
                eType = OFTInteger;
                eSubType = OFSTInt16;
                break;
            case FGFT_INT32:
                eType = OFTInteger;
                break;
            case FGFT_FLOAT32:
                eType = OFTReal;
                eSubType = OFSTFloat32;
                break;
            case FGFT_FLOAT64:
                eType = OFTReal;
                break;
            case FGFT_STRING:
                /* nWidth = poGDBField->GetMaxWidth(); */
                eType = OFTString;
                break;
            case FGFT_GUID:
            case FGFT_GLOBALID:
            case FGFT_XML:
                eType = OFTString;
                break;
            case FGFT_DATETIME:
                eType = OFTDateTime;
                break;
            case FGFT_UNDEFINED:
            case FGFT_OBJECTID:
            case FGFT_GEOMETRY:
                CPLAssert(false);
                break;
            case FGFT_BINARY:
            {
                /* Special case for v9 GDB_UserMetadata table */
                if (m_iFieldToReadAsBinary < 0 &&
                    poGDBField->GetName() == "Xml" &&
                    poGDBField->GetType() == FGFT_BINARY)
                {
                    m_iFieldToReadAsBinary = i;
                    eType = OFTString;
                }
                else
                {
                    eType = OFTBinary;
                }
                break;
            }
            case FGFT_RASTER:
            {
                const FileGDBRasterField *rasterField =
                    cpl::down_cast<const FileGDBRasterField *>(poGDBField);
                if (rasterField->GetRasterType() ==
                    FileGDBRasterField::Type::MANAGED)
                    eType = OFTInteger;
                else if (rasterField->GetRasterType() ==
                         FileGDBRasterField::Type::EXTERNAL)
                    eType = OFTString;
                else
                    eType = OFTBinary;
                break;
            }
            case FGFT_INT64:
                m_bArcGISPro32OrLater = true;
                eType = OFTInteger64;
                break;
            case FGFT_DATE:
                m_bArcGISPro32OrLater = true;
                eType = OFTDate;
                break;
            case FGFT_TIME:
                m_bArcGISPro32OrLater = true;
                eType = OFTTime;
                break;
            case FGFT_DATETIME_WITH_OFFSET:
                m_bArcGISPro32OrLater = true;
                eType = OFTDateTime;
                break;
        }
        OGRFieldDefn oFieldDefn(poGDBField->GetName().c_str(), eType);
        oFieldDefn.SetAlternativeName(poGDBField->GetAlias().c_str());
        oFieldDefn.SetSubType(eSubType);
        // On creation in the FileGDB driver (GDBFieldTypeToLengthInBytes) if
        // string width is 0, we pick up DEFAULT_STRING_WIDTH=65536 by default
        // to mean unlimited string length, but we do not want to advertise
        // such a big number.
        if (eType == OFTString &&
            (nWidth < DEFAULT_STRING_WIDTH ||
             CPLTestBool(CPLGetConfigOption(
                 "OPENFILEGDB_REPORT_GENUINE_FIELD_WIDTH", "NO"))))
        {
            oFieldDefn.SetWidth(nWidth);
        }
        oFieldDefn.SetNullable(poGDBField->IsNullable());

        const CPLXMLNode *psFieldDef = nullptr;
        if (psGPFieldInfoExs != nullptr)
        {
            for (const CPLXMLNode *psChild = psGPFieldInfoExs->psChild;
                 psChild != nullptr; psChild = psChild->psNext)
            {
                if (psChild->eType != CXT_Element)
                    continue;
                if (EQUAL(psChild->pszValue, "GPFieldInfoEx") &&
                    EQUAL(CPLGetXMLValue(psChild, "Name", ""),
                          poGDBField->GetName().c_str()))
                {
                    psFieldDef = psChild;
                    break;
                }
            }
        }

        if (psFieldDef && poGDBField->GetType() == FGFT_DATETIME)
        {
            if (EQUAL(CPLGetXMLValue(psFieldDef, "HighPrecision", ""), "true"))
            {
                poGDBField->SetHighPrecision();
            }
        }

        const OGRField *psDefault = poGDBField->GetDefault();
        if (!OGR_RawField_IsUnset(psDefault) && !OGR_RawField_IsNull(psDefault))
        {
            if (eType == OFTString)
            {
                CPLString osDefault("'");
                char *pszTmp =
                    CPLEscapeString(psDefault->String, -1, CPLES_SQL);
                osDefault += pszTmp;
                CPLFree(pszTmp);
                osDefault += "'";
                oFieldDefn.SetDefault(osDefault);
            }
            else if (eType == OFTInteger || eType == OFTReal ||
                     eType == OFTInteger64)
            {
                // GDBs and the FileGDB SDK are not always reliable for
                // numeric values It often occurs that the XML definition in
                // a00000004.gdbtable does not match the default values (in
                // binary) found in the field definition section of the
                // .gdbtable of the layers themselves So check consistency.

                const char *pszDefaultValue = nullptr;
                if (psFieldDef)
                {
                    // From ArcGIS this is called DefaultValueNumeric
                    // for integer and real.
                    // From FileGDB API this is
                    // called DefaultValue xsi:type=xs:int for integer
                    // and DefaultValueNumeric for real ...
                    pszDefaultValue = CPLGetXMLValue(
                        psFieldDef, "DefaultValueNumeric", nullptr);
                    if (pszDefaultValue == nullptr)
                        pszDefaultValue =
                            CPLGetXMLValue(psFieldDef, "DefaultValue", nullptr);
                    // For ArcGIS Pro 3.2 and esriFieldTypeBigInteger, this is
                    // DefaultValueInteger
                    if (pszDefaultValue == nullptr)
                        pszDefaultValue = CPLGetXMLValue(
                            psFieldDef, "DefaultValueInteger", nullptr);
                }
                if (pszDefaultValue != nullptr)
                {
                    if (eType == OFTInteger)
                    {
                        if (atoi(pszDefaultValue) != psDefault->Integer)
                        {
                            CPLDebug(
                                "OpenFileGDB",
                                "For field %s, XML definition mentions %s "
                                "as default value whereas .gdbtable header "
                                "mentions %d. Using %s",
                                poGDBField->GetName().c_str(), pszDefaultValue,
                                psDefault->Integer, pszDefaultValue);
                        }
                        oFieldDefn.SetDefault(pszDefaultValue);
                    }
                    else if (eType == OFTReal)
                    {
                        if (fabs(CPLAtof(pszDefaultValue) - psDefault->Real) >
                            1e-15)
                        {
                            CPLDebug(
                                "OpenFileGDB",
                                "For field %s, XML definition "
                                "mentions %s as default value whereas "
                                ".gdbtable header mentions %.17g. Using %s",
                                poGDBField->GetName().c_str(), pszDefaultValue,
                                psDefault->Real, pszDefaultValue);
                        }
                        oFieldDefn.SetDefault(pszDefaultValue);
                    }
                    else if (eType == OFTInteger64)
                    {
                        if (CPLAtoGIntBig(pszDefaultValue) !=
                            psDefault->Integer64)
                        {
                            CPLDebug(
                                "OpenFileGDB",
                                "For field %s, XML definition mentions %s "
                                "as default value whereas .gdbtable header "
                                "mentions " CPL_FRMT_GIB ". Using %s",
                                poGDBField->GetName().c_str(), pszDefaultValue,
                                psDefault->Integer64, pszDefaultValue);
                        }
                        oFieldDefn.SetDefault(pszDefaultValue);
                    }
                }
            }
            else if (eType == OFTDateTime)
            {
                if (poGDBField->GetType() == FGFT_DATETIME_WITH_OFFSET)
                {
                    oFieldDefn.SetDefault(CPLSPrintf(
                        "'%04d/%02d/%02d %02d:%02d:%06.03f%c%02d:%02d'",
                        psDefault->Date.Year, psDefault->Date.Month,
                        psDefault->Date.Day, psDefault->Date.Hour,
                        psDefault->Date.Minute, psDefault->Date.Second,
                        psDefault->Date.TZFlag >= 100 ? '+' : '-',
                        std::abs(psDefault->Date.TZFlag - 100) / 4,
                        (std::abs(psDefault->Date.TZFlag - 100) % 4) * 15));
                }
                else
                {
                    oFieldDefn.SetDefault(CPLSPrintf(
                        "'%04d/%02d/%02d %02d:%02d:%02d'", psDefault->Date.Year,
                        psDefault->Date.Month, psDefault->Date.Day,
                        psDefault->Date.Hour, psDefault->Date.Minute,
                        static_cast<int>(psDefault->Date.Second)));
                }
            }
            else if (eType == OFTDate)
                oFieldDefn.SetDefault(
                    CPLSPrintf("'%04d/%02d/%02d'", psDefault->Date.Year,
                               psDefault->Date.Month, psDefault->Date.Day));
            else if (eType == OFTTime)
                oFieldDefn.SetDefault(
                    CPLSPrintf("'%02d:%02d:%02d'", psDefault->Date.Hour,
                               psDefault->Date.Minute,
                               static_cast<int>(psDefault->Date.Second)));
        }

        if (psFieldDef)
        {
            const char *pszDomainName =
                CPLGetXMLValue(psFieldDef, "DomainName", nullptr);
            if (pszDomainName)
                oFieldDefn.SetDomainName(pszDomainName);
        }

        if (osAreaFieldName == poGDBField->GetName() &&
            oFieldDefn.GetType() == OFTReal)
        {
            m_iAreaField = m_poFeatureDefn->GetFieldCount();
            oFieldDefn.SetDefault("FILEGEODATABASE_SHAPE_AREA");
        }
        else if (osLengthFieldName == poGDBField->GetName() &&
                 oFieldDefn.GetType() == OFTReal)
        {
            m_iLengthField = m_poFeatureDefn->GetFieldCount();
            oFieldDefn.SetDefault("FILEGEODATABASE_SHAPE_LENGTH");
        }

        m_poFeatureDefn->AddFieldDefn(&oFieldDefn);
    }

    if (m_poLyrTable->HasDeletedFeaturesListed())
    {
        OGRFieldDefn oFieldDefn("_deleted_", OFTInteger);
        m_poFeatureDefn->AddFieldDefn(&oFieldDefn);
    }

    return TRUE;
}

/************************************************************************/
/*                           GetGeomType()                              */
/************************************************************************/

OGRwkbGeometryType OGROpenFileGDBLayer::GetGeomType()
{
    if (m_eGeomType == wkbUnknown ||
        m_osDefinition.empty() /* FileGDB v9 case */)
    {
        (void)BuildLayerDefinition();
    }

    return m_eGeomType;
}

/***********************************************************************/
/*                          GetLayerDefn()                             */
/***********************************************************************/

OGRFeatureDefn *OGROpenFileGDBLayer::GetLayerDefn()
{
    return m_poFeatureDefn;
}

/***********************************************************************/
/*                          GetFIDColumn()                             */
/***********************************************************************/

const char *OGROpenFileGDBLayer::GetFIDColumn()
{
    if (!BuildLayerDefinition())
        return "";
    int iIdx = m_poLyrTable->GetObjectIdFieldIdx();
    if (iIdx < 0)
        return "";
    return m_poLyrTable->GetField(iIdx)->GetName().c_str();
}

/***********************************************************************/
/*                          ResetReading()                             */
/***********************************************************************/

void OGROpenFileGDBLayer::ResetReading()
{
    if (m_iCurFeat != 0)
    {
        if (m_eSpatialIndexState == SPI_IN_BUILDING)
            m_eSpatialIndexState = SPI_INVALID;
    }
    m_bEOF = FALSE;
    m_iCurFeat = 0;
    if (m_poAttributeIterator)
        m_poAttributeIterator->Reset();
    if (m_poSpatialIndexIterator)
        m_poSpatialIndexIterator->Reset();
    if (m_poCombinedIterator)
        m_poCombinedIterator->Reset();
}

/***********************************************************************/
/*                        ISetSpatialFilter()                          */
/***********************************************************************/

OGRErr OGROpenFileGDBLayer::ISetSpatialFilter(int iGeomField,
                                              const OGRGeometry *poGeom)
{
    if (!BuildLayerDefinition())
        return OGRERR_FAILURE;

    OGRLayer::ISetSpatialFilter(iGeomField, poGeom);

    if (m_bFilterIsEnvelope)
    {
        OGREnvelope sLayerEnvelope;
        if (GetExtent(&sLayerEnvelope, FALSE) == OGRERR_NONE)
        {
            if (m_sFilterEnvelope.MinX <= sLayerEnvelope.MinX &&
                m_sFilterEnvelope.MinY <= sLayerEnvelope.MinY &&
                m_sFilterEnvelope.MaxX >= sLayerEnvelope.MaxX &&
                m_sFilterEnvelope.MaxY >= sLayerEnvelope.MaxY)
            {
#ifdef DEBUG
                CPLDebug("OpenFileGDB", "Disabling spatial filter since it "
                                        "contains the layer spatial extent");
#endif
                poGeom = nullptr;
                OGRLayer::ISetSpatialFilter(iGeomField, poGeom);
            }
        }
    }

    if (poGeom != nullptr)
    {
        if (m_poSpatialIndexIterator == nullptr &&
            m_poLyrTable->CanUseIndices() && m_poLyrTable->HasSpatialIndex() &&
            CPLTestBool(
                CPLGetConfigOption("OPENFILEGDB_USE_SPATIAL_INDEX", "YES")))
        {
            m_poSpatialIndexIterator = FileGDBSpatialIndexIterator::Build(
                m_poLyrTable, m_sFilterEnvelope);
        }
        else if (m_poSpatialIndexIterator != nullptr)
        {
            if (!m_poSpatialIndexIterator->SetEnvelope(m_sFilterEnvelope))
            {
                delete m_poSpatialIndexIterator;
                m_poSpatialIndexIterator = nullptr;
            }
        }
        else if (m_eSpatialIndexState == SPI_COMPLETED)
        {
            CPLRectObj aoi;
            aoi.minx = m_sFilterEnvelope.MinX;
            aoi.miny = m_sFilterEnvelope.MinY;
            aoi.maxx = m_sFilterEnvelope.MaxX;
            aoi.maxy = m_sFilterEnvelope.MaxY;
            CPLFree(m_pahFilteredFeatures);
            m_nFilteredFeatureCount = -1;
            m_pahFilteredFeatures =
                CPLQuadTreeSearch(m_pQuadTree, &aoi, &m_nFilteredFeatureCount);
            if (m_nFilteredFeatureCount >= 0)
            {
                size_t *panStart =
                    reinterpret_cast<size_t *>(m_pahFilteredFeatures);
                std::sort(panStart, panStart + m_nFilteredFeatureCount);
            }
        }

        m_poLyrTable->InstallFilterEnvelope(&m_sFilterEnvelope);
    }
    else
    {
        delete m_poSpatialIndexIterator;
        m_poSpatialIndexIterator = nullptr;
        CPLFree(m_pahFilteredFeatures);
        m_pahFilteredFeatures = nullptr;
        m_nFilteredFeatureCount = -1;
        m_poLyrTable->InstallFilterEnvelope(nullptr);
    }

    BuildCombinedIterator();

    return OGRERR_NONE;
}

/***********************************************************************/
/*                            CompValues()                             */
/***********************************************************************/

static int CompValues(OGRFieldDefn *poFieldDefn, const swq_expr_node *poValue1,
                      const swq_expr_node *poValue2)
{
    int ret = 0;
    switch (poFieldDefn->GetType())
    {
        case OFTInteger:
        {
            int n1, n2;
            if (poValue1->field_type == SWQ_FLOAT)
                n1 = static_cast<int>(poValue1->float_value);
            else
                n1 = static_cast<int>(poValue1->int_value);
            if (poValue2->field_type == SWQ_FLOAT)
                n2 = static_cast<int>(poValue2->float_value);
            else
                n2 = static_cast<int>(poValue2->int_value);
            if (n1 < n2)
                ret = -1;
            else if (n1 == n2)
                ret = 0;
            else
                ret = 1;
            break;
        }

        case OFTReal:
            if (poValue1->float_value < poValue2->float_value)
                ret = -1;
            else if (poValue1->float_value == poValue2->float_value)
                ret = 0;
            else
                ret = 1;
            break;

        case OFTString:
            ret = strcmp(poValue1->string_value, poValue2->string_value);
            break;

        case OFTDate:
        case OFTTime:
        case OFTDateTime:
        {
            if ((poValue1->field_type == SWQ_TIMESTAMP ||
                 poValue1->field_type == SWQ_DATE ||
                 poValue1->field_type == SWQ_TIME) &&
                (poValue2->field_type == SWQ_TIMESTAMP ||
                 poValue2->field_type == SWQ_DATE ||
                 poValue2->field_type == SWQ_TIME))
            {
                ret = strcmp(poValue1->string_value, poValue2->string_value);
            }
            break;
        }

        default:
            break;
    }
    return ret;
}

/***********************************************************************/
/*                    OGROpenFileGDBIsComparisonOp()                   */
/***********************************************************************/

int OGROpenFileGDBIsComparisonOp(int op)
{
    return (op == SWQ_EQ || op == SWQ_NE || op == SWQ_LT || op == SWQ_LE ||
            op == SWQ_GT || op == SWQ_GE);
}

/***********************************************************************/
/*                        AreExprExclusive()                           */
/***********************************************************************/

static const struct
{
    swq_op op1;
    swq_op op2;
    int expected_comp_1;
    int expected_comp_2;
} asPairsOfComparisons[] = {
    {SWQ_EQ, SWQ_EQ, -1, 1},   {SWQ_LT, SWQ_GT, -1, 0},
    {SWQ_GT, SWQ_LT, 0, 1},    {SWQ_LT, SWQ_GE, -1, 999},
    {SWQ_LE, SWQ_GE, -1, 999}, {SWQ_LE, SWQ_GT, -1, 999},
    {SWQ_GE, SWQ_LE, 1, 999},  {SWQ_GE, SWQ_LT, 1, 999},
    {SWQ_GT, SWQ_LE, 1, 999}};

static int AreExprExclusive(OGRFeatureDefn *poFeatureDefn,
                            const swq_expr_node *poNode1,
                            const swq_expr_node *poNode2)
{
    if (poNode1->eNodeType != SNT_OPERATION)
        return FALSE;
    if (poNode2->eNodeType != SNT_OPERATION)
        return FALSE;

    const size_t nPairs =
        sizeof(asPairsOfComparisons) / sizeof(asPairsOfComparisons[0]);
    for (size_t i = 0; i < nPairs; i++)
    {
        if (poNode1->nOperation == asPairsOfComparisons[i].op1 &&
            poNode2->nOperation == asPairsOfComparisons[i].op2 &&
            poNode1->nSubExprCount == 2 && poNode2->nSubExprCount == 2)
        {
            swq_expr_node *poColumn1 = poNode1->papoSubExpr[0];
            swq_expr_node *poValue1 = poNode1->papoSubExpr[1];
            swq_expr_node *poColumn2 = poNode2->papoSubExpr[0];
            swq_expr_node *poValue2 = poNode2->papoSubExpr[1];
            if (poColumn1->eNodeType == SNT_COLUMN &&
                poValue1->eNodeType == SNT_CONSTANT &&
                poColumn2->eNodeType == SNT_COLUMN &&
                poValue2->eNodeType == SNT_CONSTANT &&
                poColumn1->field_index == poColumn2->field_index &&
                poColumn1->field_index < poFeatureDefn->GetFieldCount())
            {
                OGRFieldDefn *poFieldDefn =
                    poFeatureDefn->GetFieldDefn(poColumn1->field_index);

                const int nComp = CompValues(poFieldDefn, poValue1, poValue2);
                return nComp == asPairsOfComparisons[i].expected_comp_1 ||
                       nComp == asPairsOfComparisons[i].expected_comp_2;
            }
            return FALSE;
        }
    }

    if ((poNode2->nOperation == SWQ_ISNULL &&
         OGROpenFileGDBIsComparisonOp(poNode1->nOperation) &&
         poNode1->nSubExprCount == 2 && poNode2->nSubExprCount == 1) ||
        (poNode1->nOperation == SWQ_ISNULL &&
         OGROpenFileGDBIsComparisonOp(poNode2->nOperation) &&
         poNode2->nSubExprCount == 2 && poNode1->nSubExprCount == 1))
    {
        swq_expr_node *poColumn1 = poNode1->papoSubExpr[0];
        swq_expr_node *poColumn2 = poNode2->papoSubExpr[0];
        if (poColumn1->eNodeType == SNT_COLUMN &&
            poColumn2->eNodeType == SNT_COLUMN &&
            poColumn1->field_index == poColumn2->field_index &&
            poColumn1->field_index < poFeatureDefn->GetFieldCount())
        {
            return TRUE;
        }
    }

    /* In doubt: return FALSE */
    return FALSE;
}

/***********************************************************************/
/*                     FillTargetValueFromSrcExpr()                    */
/***********************************************************************/

static int FillTargetValueFromSrcExpr(OGRFieldDefn *poFieldDefn,
                                      OGRField *poTargetValue,
                                      const swq_expr_node *poSrcValue)
{
    switch (poFieldDefn->GetType())
    {
        case OFTInteger:
            if (poSrcValue->field_type == SWQ_FLOAT)
                poTargetValue->Integer =
                    static_cast<int>(poSrcValue->float_value);
            else
                poTargetValue->Integer =
                    static_cast<int>(poSrcValue->int_value);
            break;

        case OFTInteger64:
            if (poSrcValue->field_type == SWQ_FLOAT)
                poTargetValue->Integer64 =
                    static_cast<GIntBig>(poSrcValue->float_value);
            else
                poTargetValue->Integer64 = poSrcValue->int_value;
            break;

        case OFTReal:
            poTargetValue->Real = poSrcValue->float_value;
            break;

        case OFTString:
            poTargetValue->String = poSrcValue->string_value;
            break;

        case OFTDate:
        case OFTTime:
        case OFTDateTime:
            if (poSrcValue->field_type == SWQ_TIMESTAMP ||
                poSrcValue->field_type == SWQ_DATE ||
                poSrcValue->field_type == SWQ_TIME)
            {
                int nYear = 0, nMonth = 0, nDay = 0, nHour = 0, nMin = 0,
                    nSec = 0;
                if (sscanf(poSrcValue->string_value,
                           "%04d/%02d/%02d %02d:%02d:%02d", &nYear, &nMonth,
                           &nDay, &nHour, &nMin, &nSec) == 6 ||
                    sscanf(poSrcValue->string_value, "%04d/%02d/%02d", &nYear,
                           &nMonth, &nDay) == 3 ||
                    sscanf(poSrcValue->string_value, "%02d:%02d:%02d", &nHour,
                           &nMin, &nSec) == 3)
                {
                    poTargetValue->Date.Year = static_cast<GInt16>(nYear);
                    poTargetValue->Date.Month = static_cast<GByte>(nMonth);
                    poTargetValue->Date.Day = static_cast<GByte>(nDay);
                    poTargetValue->Date.Hour = static_cast<GByte>(nHour);
                    poTargetValue->Date.Minute = static_cast<GByte>(nMin);
                    poTargetValue->Date.Second = static_cast<GByte>(nSec);
                    poTargetValue->Date.TZFlag = 0;
                    poTargetValue->Date.Reserved = 0;
                }
                else
                    return FALSE;
            }
            else
                return FALSE;
            break;

        default:
            return FALSE;
    }
    return TRUE;
}

/***********************************************************************/
/*                        GetColumnSubNode()                           */
/***********************************************************************/

static swq_expr_node *GetColumnSubNode(swq_expr_node *poNode)
{
    if (poNode->eNodeType == SNT_OPERATION && poNode->nSubExprCount == 2)
    {
        if (poNode->papoSubExpr[0]->eNodeType == SNT_COLUMN)
            return poNode->papoSubExpr[0];
        if (poNode->papoSubExpr[1]->eNodeType == SNT_COLUMN)
            return poNode->papoSubExpr[1];
    }
    return nullptr;
}

/***********************************************************************/
/*                        GetConstantSubNode()                         */
/***********************************************************************/

static swq_expr_node *GetConstantSubNode(swq_expr_node *poNode)
{
    if (poNode->eNodeType == SNT_OPERATION && poNode->nSubExprCount == 2)
    {
        if (poNode->papoSubExpr[1]->eNodeType == SNT_CONSTANT)
            return poNode->papoSubExpr[1];
        if (poNode->papoSubExpr[0]->eNodeType == SNT_CONSTANT)
            return poNode->papoSubExpr[0];
    }
    return nullptr;
}

/***********************************************************************/
/*                     BuildIteratorFromExprNode()                     */
/***********************************************************************/

FileGDBIterator *
OGROpenFileGDBLayer::BuildIteratorFromExprNode(swq_expr_node *poNode)
{
    if (m_bIteratorSufficientToEvaluateFilter == FALSE)
        return nullptr;

    if (poNode->eNodeType == SNT_OPERATION && poNode->nOperation == SWQ_AND &&
        poNode->nSubExprCount == 2)
    {
        // Even if there is only one branch of the 2 that results to an
        // iterator, it is useful. Of course, the iterator will not be
        // sufficient to evaluatethe filter, but it will be a super-set of the
        // features
        FileGDBIterator *poIter1 =
            BuildIteratorFromExprNode(poNode->papoSubExpr[0]);

        /* In case the first branch didn't result to an iterator, temporarily */
        /* restore the flag */
        const bool bSaveIteratorSufficientToEvaluateFilter =
            CPL_TO_BOOL(m_bIteratorSufficientToEvaluateFilter);
        m_bIteratorSufficientToEvaluateFilter = -1;
        FileGDBIterator *poIter2 =
            BuildIteratorFromExprNode(poNode->papoSubExpr[1]);
        m_bIteratorSufficientToEvaluateFilter =
            bSaveIteratorSufficientToEvaluateFilter;

        if (poIter1 != nullptr && poIter2 != nullptr)
            return FileGDBIterator::BuildAnd(poIter1, poIter2, true);
        m_bIteratorSufficientToEvaluateFilter = FALSE;
        if (poIter1 != nullptr)
            return poIter1;
        if (poIter2 != nullptr)
            return poIter2;
    }

    else if (poNode->eNodeType == SNT_OPERATION &&
             poNode->nOperation == SWQ_OR && poNode->nSubExprCount == 2)
    {
        /* For a OR, we need an iterator for the 2 branches */
        FileGDBIterator *poIter1 =
            BuildIteratorFromExprNode(poNode->papoSubExpr[0]);
        if (poIter1 != nullptr)
        {
            FileGDBIterator *poIter2 =
                BuildIteratorFromExprNode(poNode->papoSubExpr[1]);
            if (poIter2 == nullptr)
            {
                delete poIter1;
            }
            else
            {
                return FileGDBIterator::BuildOr(
                    poIter1, poIter2,
                    AreExprExclusive(GetLayerDefn(), poNode->papoSubExpr[0],
                                     poNode->papoSubExpr[1]));
            }
        }
    }

    else if (poNode->eNodeType == SNT_OPERATION &&
             (OGROpenFileGDBIsComparisonOp(poNode->nOperation) ||
              poNode->nOperation == SWQ_ILIKE) &&
             poNode->nSubExprCount == 2)
    {
        swq_expr_node *poColumn = GetColumnSubNode(poNode);
        swq_expr_node *poValue = GetConstantSubNode(poNode);
        if (poColumn != nullptr && poValue != nullptr &&
            poColumn->field_index < GetLayerDefn()->GetFieldCount())
        {
            OGRFieldDefn *poFieldDefn =
                GetLayerDefn()->GetFieldDefn(poColumn->field_index);

            int nTableColIdx =
                m_poLyrTable->GetFieldIdx(poFieldDefn->GetNameRef());
            if (nTableColIdx >= 0 &&
                m_poLyrTable->GetField(nTableColIdx)->HasIndex())
            {
                OGRField sValue;

                if (FillTargetValueFromSrcExpr(poFieldDefn, &sValue, poValue))
                {
                    FileGDBSQLOp eOp = FGSO_EQ;
                    CPL_IGNORE_RET_VAL(eOp);
                    if (poColumn == poNode->papoSubExpr[0])
                    {
                        switch (poNode->nOperation)
                        {
                            case SWQ_LE:
                                eOp = FGSO_LE;
                                break;
                            case SWQ_LT:
                                eOp = FGSO_LT;
                                break;
                            case SWQ_NE:
                                eOp = FGSO_EQ; /* yes : EQ */
                                break;
                            case SWQ_EQ:
                                eOp = FGSO_EQ;
                                break;
                            case SWQ_GE:
                                eOp = FGSO_GE;
                                break;
                            case SWQ_GT:
                                eOp = FGSO_GT;
                                break;
                            case SWQ_ILIKE:
                                eOp = FGSO_ILIKE;
                                break;
                            default:
                                CPLAssert(false);
                                break;
                        }
                    }
                    else
                    {
                        /* If "constant op column", then we must reverse */
                        /* the operator */
                        switch (poNode->nOperation)
                        {
                            case SWQ_LE:
                                eOp = FGSO_GE;
                                break;
                            case SWQ_LT:
                                eOp = FGSO_GT;
                                break;
                            case SWQ_NE:
                                eOp = FGSO_EQ; /* yes : EQ */
                                break;
                            case SWQ_EQ:
                                eOp = FGSO_EQ;
                                break;
                            case SWQ_GE:
                                eOp = FGSO_LE;
                                break;
                            case SWQ_GT:
                                eOp = FGSO_LT;
                                break;
                            case SWQ_ILIKE:
                                eOp = FGSO_ILIKE;
                                break;
                            default:
                                CPLAssert(false);
                                break;
                        }
                    }

                    bool bIteratorSufficient = true;
                    auto poField = m_poLyrTable->GetField(nTableColIdx);
                    std::string osTruncatedStr;  // keep it in this scope !
                    if (poField->GetType() == FGFT_STRING &&
                        poFieldDefn->GetType() == OFTString)
                    {
                        // If we have an equality comparison, but the index
                        // uses LOWER(), transform it to a ILIKE comparison
                        if (eOp == FGSO_EQ && poField->HasIndex() &&
                            STARTS_WITH_CI(
                                poField->GetIndex()->GetExpression().c_str(),
                                "LOWER("))
                        {
                            // Note: FileGDBIndexIterator::SetConstraint()
                            // checks that the string to compare with has no
                            // wildcard
                            eOp = FGSO_ILIKE;

                            // In theory, a ILIKE is not sufficient as it is
                            // case insensitive, whereas one could expect
                            // equality testing to be case sensitive... but
                            // it is not in OGR SQL...
                            // So we can comment the below line
                            // bIteratorSufficient = false;
                        }

                        // As the index use ' ' as padding value, we cannot
                        // fully trust the index.
                        else if ((eOp == FGSO_EQ &&
                                  poNode->nOperation != SWQ_NE) ||
                                 eOp == FGSO_GE)
                            bIteratorSufficient = false;
                        else
                            return nullptr;

                        const int nMaxWidthIndexedStr =
                            poField->GetIndex()->GetMaxWidthInBytes(
                                m_poLyrTable);
                        if (nMaxWidthIndexedStr > 0)
                        {
                            wchar_t *pWide = CPLRecodeToWChar(
                                sValue.String, CPL_ENC_UTF8, CPL_ENC_UCS2);
                            if (pWide)
                            {
                                const size_t nUCS2Len = wcslen(pWide);
                                if (nUCS2Len * sizeof(uint16_t) >
                                    static_cast<size_t>(nMaxWidthIndexedStr))
                                {
                                    pWide[nMaxWidthIndexedStr /
                                          sizeof(uint16_t)] = 0;
                                    char *pszTruncated = CPLRecodeFromWChar(
                                        pWide, CPL_ENC_UCS2, CPL_ENC_UTF8);
                                    if (pszTruncated)
                                    {
                                        osTruncatedStr = pszTruncated;
                                        sValue.String = &osTruncatedStr[0];
                                        CPLFree(pszTruncated);
                                    }
                                }
                                CPLFree(pWide);
                            }
                        }
                    }
                    else if (eOp == FGSO_ILIKE)
                        return nullptr;

                    FileGDBIterator *poIter = FileGDBIterator::Build(
                        m_poLyrTable, nTableColIdx, TRUE, eOp,
                        poFieldDefn->GetType(), &sValue);
                    if (poIter != nullptr)
                        m_bIteratorSufficientToEvaluateFilter =
                            bIteratorSufficient;
                    if (poIter && poNode->nOperation == SWQ_NE)
                        return FileGDBIterator::BuildNot(poIter);
                    else
                        return poIter;
                }
            }
        }
    }
    else if (poNode->eNodeType == SNT_OPERATION &&
             poNode->nOperation == SWQ_ISNULL && poNode->nSubExprCount == 1)
    {
        swq_expr_node *poColumn = poNode->papoSubExpr[0];
        if (poColumn->eNodeType == SNT_COLUMN &&
            poColumn->field_index < GetLayerDefn()->GetFieldCount())
        {
            OGRFieldDefn *poFieldDefn =
                GetLayerDefn()->GetFieldDefn(poColumn->field_index);

            int nTableColIdx =
                m_poLyrTable->GetFieldIdx(poFieldDefn->GetNameRef());
            if (nTableColIdx >= 0 &&
                m_poLyrTable->GetField(nTableColIdx)->HasIndex())
            {
                FileGDBIterator *poIter = FileGDBIterator::BuildIsNotNull(
                    m_poLyrTable, nTableColIdx, TRUE);
                if (poIter)
                {
                    m_bIteratorSufficientToEvaluateFilter = TRUE;
                    poIter = FileGDBIterator::BuildNot(poIter);
                }
                return poIter;
            }
        }
    }
    else if (poNode->eNodeType == SNT_OPERATION &&
             poNode->nOperation == SWQ_NOT && poNode->nSubExprCount == 1 &&
             poNode->papoSubExpr[0]->eNodeType == SNT_OPERATION &&
             poNode->papoSubExpr[0]->nOperation == SWQ_ISNULL &&
             poNode->papoSubExpr[0]->nSubExprCount == 1)
    {
        swq_expr_node *poColumn = poNode->papoSubExpr[0]->papoSubExpr[0];
        if (poColumn->eNodeType == SNT_COLUMN &&
            poColumn->field_index < GetLayerDefn()->GetFieldCount())
        {
            OGRFieldDefn *poFieldDefn =
                GetLayerDefn()->GetFieldDefn(poColumn->field_index);

            int nTableColIdx =
                m_poLyrTable->GetFieldIdx(poFieldDefn->GetNameRef());
            if (nTableColIdx >= 0 &&
                m_poLyrTable->GetField(nTableColIdx)->HasIndex())
            {
                FileGDBIterator *poIter = FileGDBIterator::BuildIsNotNull(
                    m_poLyrTable, nTableColIdx, TRUE);
                if (poIter)
                    m_bIteratorSufficientToEvaluateFilter = TRUE;
                return poIter;
            }
        }
    }
    else if (poNode->eNodeType == SNT_OPERATION &&
             poNode->nOperation == SWQ_IN && poNode->nSubExprCount >= 2)
    {
        swq_expr_node *poColumn = poNode->papoSubExpr[0];
        if (poColumn->eNodeType == SNT_COLUMN &&
            poColumn->field_index < GetLayerDefn()->GetFieldCount())
        {
            bool bAllConstants = true;
            for (int i = 1; i < poNode->nSubExprCount; i++)
            {
                if (poNode->papoSubExpr[i]->eNodeType != SNT_CONSTANT)
                    bAllConstants = false;
            }
            OGRFieldDefn *poFieldDefn =
                GetLayerDefn()->GetFieldDefn(poColumn->field_index);

            int nTableColIdx =
                m_poLyrTable->GetFieldIdx(poFieldDefn->GetNameRef());
            if (bAllConstants && nTableColIdx >= 0 &&
                m_poLyrTable->GetField(nTableColIdx)->HasIndex())
            {
                FileGDBIterator *poRet = nullptr;

                bool bIteratorSufficient = true;
                auto poField = m_poLyrTable->GetField(nTableColIdx);

                for (int i = 1; i < poNode->nSubExprCount; i++)
                {
                    OGRField sValue;
                    if (!FillTargetValueFromSrcExpr(poFieldDefn, &sValue,
                                                    poNode->papoSubExpr[i]))
                    {
                        delete poRet;
                        poRet = nullptr;
                        break;
                    }

                    std::string osTruncatedStr;  // keep it in this scope !
                    if (poField->GetType() == FGFT_STRING &&
                        poFieldDefn->GetType() == OFTString)
                    {
                        const int nMaxWidthIndexedStr =
                            poField->GetIndex()->GetMaxWidthInBytes(
                                m_poLyrTable);
                        if (nMaxWidthIndexedStr > 0)
                        {
                            wchar_t *pWide = CPLRecodeToWChar(
                                sValue.String, CPL_ENC_UTF8, CPL_ENC_UCS2);
                            if (pWide)
                            {
                                const size_t nUCS2Len = wcslen(pWide);
                                if (nUCS2Len * sizeof(uint16_t) >
                                    static_cast<size_t>(nMaxWidthIndexedStr))
                                {
                                    pWide[nMaxWidthIndexedStr /
                                          sizeof(uint16_t)] = 0;
                                    char *pszTruncated = CPLRecodeFromWChar(
                                        pWide, CPL_ENC_UCS2, CPL_ENC_UTF8);
                                    if (pszTruncated)
                                    {
                                        osTruncatedStr = pszTruncated;
                                        sValue.String = &osTruncatedStr[0];
                                        CPLFree(pszTruncated);
                                    }
                                }
                                CPLFree(pWide);
                            }
                        }

                        // As the index use ' ' as padding value, we cannot
                        // fully trust the index.
                        bIteratorSufficient = false;
                    }

                    FileGDBIterator *poIter = FileGDBIterator::Build(
                        m_poLyrTable, nTableColIdx, TRUE, FGSO_EQ,
                        poFieldDefn->GetType(), &sValue);
                    if (poIter == nullptr)
                    {
                        delete poRet;
                        poRet = nullptr;
                        break;
                    }
                    if (poRet == nullptr)
                        poRet = poIter;
                    else
                        poRet = FileGDBIterator::BuildOr(poRet, poIter);
                }
                if (poRet != nullptr)
                {
                    m_bIteratorSufficientToEvaluateFilter = bIteratorSufficient;
                    return poRet;
                }
            }
        }
    }
    else if (poNode->eNodeType == SNT_OPERATION &&
             poNode->nOperation == SWQ_NOT && poNode->nSubExprCount == 1)
    {
        FileGDBIterator *poIter =
            BuildIteratorFromExprNode(poNode->papoSubExpr[0]);
        /* If we have an iterator that is only partial w.r.t the full clause */
        /* then we cannot do anything with it unfortunately */
        if (m_bIteratorSufficientToEvaluateFilter == FALSE)
        {
            if (poIter != nullptr)
                CPLDebug("OpenFileGDB", "Disabling use of indexes");
            delete poIter;
        }
        else if (poIter != nullptr)
        {
            return FileGDBIterator::BuildNot(poIter);
        }
    }

    if (m_bIteratorSufficientToEvaluateFilter == TRUE)
        CPLDebug("OpenFileGDB", "Disabling use of indexes");
    m_bIteratorSufficientToEvaluateFilter = FALSE;
    return nullptr;
}

/***********************************************************************/
/*                         SetAttributeFilter()                        */
/***********************************************************************/

OGRErr OGROpenFileGDBLayer::SetAttributeFilter(const char *pszFilter)
{
    if (!BuildLayerDefinition())
        return OGRERR_FAILURE;

    delete m_poAttributeIterator;
    m_poAttributeIterator = nullptr;
    delete m_poCombinedIterator;
    m_poCombinedIterator = nullptr;
    m_bIteratorSufficientToEvaluateFilter = FALSE;

    OGRErr eErr = OGRLayer::SetAttributeFilter(pszFilter);
    if (eErr != OGRERR_NONE ||
        !CPLTestBool(CPLGetConfigOption("OPENFILEGDB_USE_INDEX", "YES")))
        return eErr;

    if (m_poAttrQuery != nullptr && m_nFilteredFeatureCount < 0)
    {
        swq_expr_node *poNode =
            static_cast<swq_expr_node *>(m_poAttrQuery->GetSWQExpr());
        poNode->ReplaceBetweenByGEAndLERecurse();
        m_bIteratorSufficientToEvaluateFilter = -1;
        m_poAttributeIterator = BuildIteratorFromExprNode(poNode);
        if (m_poAttributeIterator != nullptr &&
            m_eSpatialIndexState == SPI_IN_BUILDING)
            m_eSpatialIndexState = SPI_INVALID;
        if (m_bIteratorSufficientToEvaluateFilter < 0)
            m_bIteratorSufficientToEvaluateFilter = FALSE;
    }

    BuildCombinedIterator();

    return eErr;
}

/***********************************************************************/
/*                       BuildCombinedIterator()                       */
/***********************************************************************/

void OGROpenFileGDBLayer::BuildCombinedIterator()
{
    delete m_poCombinedIterator;
    if (m_poAttributeIterator && m_poSpatialIndexIterator)
    {
        m_poCombinedIterator = FileGDBIterator::BuildAnd(
            m_poAttributeIterator, m_poSpatialIndexIterator, false);
    }
    else
    {
        m_poCombinedIterator = nullptr;
    }
}

/***********************************************************************/
/*                         GetCurrentFeature()                         */
/***********************************************************************/

OGRFeature *OGROpenFileGDBLayer::GetCurrentFeature()
{
    OGRFeature *poFeature = nullptr;
    int iOGRIdx = 0;
    int64_t iRow = m_poLyrTable->GetCurRow();
    for (int iGDBIdx = 0; iGDBIdx < m_poLyrTable->GetFieldCount(); iGDBIdx++)
    {
        if (iOGRIdx == m_iFIDAsRegularColumnIndex)
            iOGRIdx++;

        if (iGDBIdx == m_iGeomFieldIdx)
        {
            if (m_poFeatureDefn->GetGeomFieldDefn(0)->IsIgnored())
            {
                if (m_eSpatialIndexState == SPI_IN_BUILDING)
                    m_eSpatialIndexState = SPI_INVALID;
                continue;
            }

            const OGRField *psField = m_poLyrTable->GetFieldValue(iGDBIdx);
            if (psField != nullptr)
            {
                if (m_eSpatialIndexState == SPI_IN_BUILDING)
                {
                    OGREnvelope sFeatureEnvelope;
                    if (m_poLyrTable->GetFeatureExtent(psField,
                                                       &sFeatureEnvelope))
                    {
#if SIZEOF_VOIDP < 8
                        if (iRow > INT32_MAX)
                        {
                            // m_pQuadTree stores iRow values as void*
                            // This would overflow here.
                            m_eSpatialIndexState = SPI_INVALID;
                        }
                        else
#endif
                        {
                            CPLRectObj sBounds;
                            sBounds.minx = sFeatureEnvelope.MinX;
                            sBounds.miny = sFeatureEnvelope.MinY;
                            sBounds.maxx = sFeatureEnvelope.MaxX;
                            sBounds.maxy = sFeatureEnvelope.MaxY;
                            CPLQuadTreeInsertWithBounds(
                                m_pQuadTree,
                                reinterpret_cast<void *>(
                                    static_cast<uintptr_t>(iRow)),
                                &sBounds);
                        }
                    }
                }

                if (m_poFilterGeom != nullptr &&
                    m_eSpatialIndexState != SPI_COMPLETED &&
                    !m_poLyrTable->DoesGeometryIntersectsFilterEnvelope(
                        psField))
                {
                    delete poFeature;
                    return nullptr;
                }

                OGRGeometry *poGeom = m_poGeomConverter->GetAsGeometry(psField);
                if (poGeom != nullptr)
                {
                    OGRwkbGeometryType eFlattenType =
                        wkbFlatten(poGeom->getGeometryType());
                    if (eFlattenType == wkbPolygon)
                        poGeom =
                            OGRGeometryFactory::forceToMultiPolygon(poGeom);
                    else if (eFlattenType == wkbCurvePolygon)
                    {
                        OGRMultiSurface *poMS = new OGRMultiSurface();
                        poMS->addGeometryDirectly(poGeom);
                        poGeom = poMS;
                    }
                    else if (eFlattenType == wkbLineString)
                        poGeom =
                            OGRGeometryFactory::forceToMultiLineString(poGeom);
                    else if (eFlattenType == wkbCompoundCurve)
                    {
                        OGRMultiCurve *poMC = new OGRMultiCurve();
                        poMC->addGeometryDirectly(poGeom);
                        poGeom = poMC;
                    }

                    poGeom->assignSpatialReference(
                        m_poFeatureDefn->GetGeomFieldDefn(0)->GetSpatialRef());

                    if (poFeature == nullptr)
                        poFeature = new OGRFeature(m_poFeatureDefn);
                    poFeature->SetGeometryDirectly(poGeom);
                }
            }
        }
        else if (iGDBIdx != m_poLyrTable->GetObjectIdFieldIdx())
        {
            const OGRFieldDefn *poFieldDefn =
                m_poFeatureDefn->GetFieldDefn(iOGRIdx);
            if (!poFieldDefn->IsIgnored())
            {
                const OGRField *psField = m_poLyrTable->GetFieldValue(iGDBIdx);
                if (poFeature == nullptr)
                    poFeature = new OGRFeature(m_poFeatureDefn);
                if (psField == nullptr)
                {
                    poFeature->SetFieldNull(iOGRIdx);
                }
                else
                {

                    if (iGDBIdx == m_iFieldToReadAsBinary)
                        poFeature->SetField(iOGRIdx,
                                            reinterpret_cast<const char *>(
                                                psField->Binary.paData));
                    else if (poFieldDefn->GetType() == OFTDateTime)
                    {
                        OGRField sField = *psField;
                        if (m_poLyrTable->GetField(iGDBIdx)->GetType() ==
                            FGFT_DATETIME)
                        {
                            sField.Date.TZFlag = m_bTimeInUTC ? 100 : 0;
                        }
                        poFeature->SetField(iOGRIdx, &sField);
                    }
                    else
                        poFeature->SetField(iOGRIdx, psField);
                }
            }
            iOGRIdx++;
        }
    }

    if (poFeature == nullptr)
        poFeature = new OGRFeature(m_poFeatureDefn);

    if (m_poLyrTable->HasDeletedFeaturesListed())
    {
        poFeature->SetField(poFeature->GetFieldCount() - 1,
                            m_poLyrTable->IsCurRowDeleted());
    }

    poFeature->SetFID(iRow + 1);

    if (m_iFIDAsRegularColumnIndex >= 0)
        poFeature->SetField(m_iFIDAsRegularColumnIndex, poFeature->GetFID());

    return poFeature;
}

/***********************************************************************/
/*                         GetNextFeature()                            */
/***********************************************************************/

OGRFeature *OGROpenFileGDBLayer::GetNextFeature()
{
    if (!BuildLayerDefinition() || m_bEOF)
        return nullptr;

    FileGDBIterator *poIterator = m_poCombinedIterator ? m_poCombinedIterator
                                  : m_poSpatialIndexIterator
                                      ? m_poSpatialIndexIterator
                                      : m_poAttributeIterator;

    while (true)
    {
        OGRFeature *poFeature = nullptr;

        if (m_nFilteredFeatureCount >= 0)
        {
            while (true)
            {
                if (m_iCurFeat >= m_nFilteredFeatureCount)
                {
                    return nullptr;
                }
                const auto iRow =
                    static_cast<int64_t>(reinterpret_cast<GUIntptr_t>(
                        m_pahFilteredFeatures[m_iCurFeat++]));
                if (m_poLyrTable->SelectRow(iRow))
                {
                    poFeature = GetCurrentFeature();
                    if (poFeature)
                        break;
                }
                else if (m_poLyrTable->HasGotError())
                {
                    m_bEOF = TRUE;
                    return nullptr;
                }
            }
        }
        else if (poIterator != nullptr)
        {
            while (true)
            {
                const auto iRow = poIterator->GetNextRowSortedByFID();
                if (iRow < 0)
                    return nullptr;
                if (m_poLyrTable->SelectRow(iRow))
                {
                    poFeature = GetCurrentFeature();
                    if (poFeature)
                        break;
                }
                else if (m_poLyrTable->HasGotError())
                {
                    m_bEOF = TRUE;
                    return nullptr;
                }
            }
        }
        else
        {
            while (true)
            {
                if (m_iCurFeat == m_poLyrTable->GetTotalRecordCount())
                {
                    return nullptr;
                }
                m_iCurFeat =
                    m_poLyrTable->GetAndSelectNextNonEmptyRow(m_iCurFeat);
                if (m_iCurFeat < 0)
                {
                    m_bEOF = TRUE;
                    return nullptr;
                }
                else
                {
                    m_iCurFeat++;
                    poFeature = GetCurrentFeature();
                    if (m_eSpatialIndexState == SPI_IN_BUILDING &&
                        m_iCurFeat == m_poLyrTable->GetTotalRecordCount())
                    {
                        CPLDebug("OpenFileGDB", "SPI_COMPLETED");
                        m_eSpatialIndexState = SPI_COMPLETED;
                    }
                    if (poFeature)
                        break;
                }
            }
        }

        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr ||
             (m_poAttributeIterator != nullptr &&
              m_bIteratorSufficientToEvaluateFilter) ||
             m_poAttrQuery->Evaluate(poFeature)))
        {
            return poFeature;
        }

        delete poFeature;
    }
}

/***********************************************************************/
/*                          GetFeature()                               */
/***********************************************************************/

OGRFeature *OGROpenFileGDBLayer::GetFeature(GIntBig nFeatureId)
{
    if (!BuildLayerDefinition())
        return nullptr;

    if (nFeatureId < 1 || nFeatureId > m_poLyrTable->GetTotalRecordCount())
        return nullptr;
    if (!m_poLyrTable->SelectRow(nFeatureId - 1))
        return nullptr;

    /* Temporarily disable spatial filter */
    OGRGeometry *poOldSpatialFilter = m_poFilterGeom;
    m_poFilterGeom = nullptr;
    /* and also spatial index state to avoid features to be inserted */
    /* multiple times in spatial index */
    SPIState eOldState = m_eSpatialIndexState;
    m_eSpatialIndexState = SPI_INVALID;

    OGRFeature *poFeature = GetCurrentFeature();

    /* Set it back */
    m_poFilterGeom = poOldSpatialFilter;
    m_eSpatialIndexState = eOldState;

    return poFeature;
}

/***********************************************************************/
/*                         SetNextByIndex()                            */
/***********************************************************************/

OGRErr OGROpenFileGDBLayer::SetNextByIndex(GIntBig nIndex)
{
    if (m_poAttributeIterator != nullptr || m_poSpatialIndexIterator != nullptr)
        return OGRLayer::SetNextByIndex(nIndex);

    if (!BuildLayerDefinition())
        return OGRERR_FAILURE;

    if (m_eSpatialIndexState == SPI_IN_BUILDING)
        m_eSpatialIndexState = SPI_INVALID;

    if (m_nFilteredFeatureCount >= 0)
    {
        if (nIndex < 0 || nIndex >= m_nFilteredFeatureCount)
            return OGRERR_FAILURE;
        m_iCurFeat = nIndex;
        return OGRERR_NONE;
    }
    else if (m_poLyrTable->GetValidRecordCount() ==
             m_poLyrTable->GetTotalRecordCount())
    {
        if (nIndex < 0 || nIndex >= m_poLyrTable->GetValidRecordCount())
            return OGRERR_FAILURE;
        m_iCurFeat = nIndex;
        return OGRERR_NONE;
    }
    else
        return OGRLayer::SetNextByIndex(nIndex);
}

/***********************************************************************/
/*                          IGetExtent()                               */
/***********************************************************************/

OGRErr OGROpenFileGDBLayer::IGetExtent(int /* iGeomField */,
                                       OGREnvelope *psExtent, bool /* bForce */)
{
    if (!BuildLayerDefinition())
        return OGRERR_FAILURE;

    if (m_iGeomFieldIdx >= 0 && m_poLyrTable->GetValidRecordCount() > 0)
    {
        FileGDBGeomField *poGDBGeomField = reinterpret_cast<FileGDBGeomField *>(
            m_poLyrTable->GetField(m_iGeomFieldIdx));
        if (!std::isnan(poGDBGeomField->GetXMin()))
        {
            psExtent->MinX = poGDBGeomField->GetXMin();
            psExtent->MinY = poGDBGeomField->GetYMin();
            psExtent->MaxX = poGDBGeomField->GetXMax();
            psExtent->MaxY = poGDBGeomField->GetYMax();
            return OGRERR_NONE;
        }
    }

    return OGRERR_FAILURE;
}

/***********************************************************************/
/*                          IGetExtent3D()                             */
/***********************************************************************/

OGRErr OGROpenFileGDBLayer::IGetExtent3D(int iGeomField,
                                         OGREnvelope3D *psExtent, bool bForce)
{
    if (!BuildLayerDefinition())
        return OGRERR_FAILURE;

    if (m_poFilterGeom == nullptr && m_poAttrQuery == nullptr &&
        m_iGeomFieldIdx >= 0 && m_poLyrTable->GetValidRecordCount() > 0)
    {
        FileGDBGeomField *poGDBGeomField = reinterpret_cast<FileGDBGeomField *>(
            m_poLyrTable->GetField(m_iGeomFieldIdx));
        if (!std::isnan(poGDBGeomField->GetXMin()))
        {
            psExtent->MinX = poGDBGeomField->GetXMin();
            psExtent->MinY = poGDBGeomField->GetYMin();
            psExtent->MaxX = poGDBGeomField->GetXMax();
            psExtent->MaxY = poGDBGeomField->GetYMax();
            if (!std::isnan(poGDBGeomField->GetZMin()))
            {
                psExtent->MinZ = poGDBGeomField->GetZMin();
                psExtent->MaxZ = poGDBGeomField->GetZMax();
            }
            else
            {
                if (OGR_GT_HasZ(m_eGeomType))
                {
                    return OGRLayer::IGetExtent3D(iGeomField, psExtent, bForce);
                }
                psExtent->MinZ = std::numeric_limits<double>::infinity();
                psExtent->MaxZ = -std::numeric_limits<double>::infinity();
            }
            return OGRERR_NONE;
        }
    }

    return OGRLayer::IGetExtent3D(iGeomField, psExtent, bForce);
}

/***********************************************************************/
/*                         GetFeatureCount()                           */
/***********************************************************************/

GIntBig OGROpenFileGDBLayer::GetFeatureCount(int bForce)
{
    if (!BuildLayerDefinition())
        return 0;

    /* No filter */
    if ((m_poFilterGeom == nullptr || m_iGeomFieldIdx < 0) &&
        m_poAttrQuery == nullptr)
    {
        return m_poLyrTable->GetValidRecordCount();
    }
    else if (m_nFilteredFeatureCount >= 0 && m_poAttrQuery == nullptr)
    {
        return m_nFilteredFeatureCount;
    }

    /* Only geometry filter ? */
    if (m_poAttrQuery == nullptr && m_bFilterIsEnvelope)
    {
        if (m_poSpatialIndexIterator)
        {
            m_poSpatialIndexIterator->Reset();
            int nCount = 0;
            while (true)
            {
                const auto nRowIdx =
                    m_poSpatialIndexIterator->GetNextRowSortedByFID();
                if (nRowIdx < 0)
                    break;
                if (!m_poLyrTable->SelectRow(nRowIdx))
                {
                    if (m_poLyrTable->HasGotError())
                        break;
                    else
                        continue;
                }

                const OGRField *psField =
                    m_poLyrTable->GetFieldValue(m_iGeomFieldIdx);
                if (psField != nullptr)
                {
                    if (m_poLyrTable->DoesGeometryIntersectsFilterEnvelope(
                            psField))
                    {
                        OGRGeometry *poGeom =
                            m_poGeomConverter->GetAsGeometry(psField);
                        if (poGeom != nullptr && FilterGeometry(poGeom))
                        {
                            nCount++;
                        }
                        delete poGeom;
                    }
                }
            }
            return nCount;
        }

        int nCount = 0;
        if (m_eSpatialIndexState == SPI_IN_BUILDING && m_iCurFeat != 0)
            m_eSpatialIndexState = SPI_INVALID;

        int nFilteredFeatureCountAlloc = 0;
        if (m_eSpatialIndexState == SPI_IN_BUILDING)
        {
            CPLFree(m_pahFilteredFeatures);
            m_pahFilteredFeatures = nullptr;
            m_nFilteredFeatureCount = 0;
        }

        for (int64_t i = 0; i < m_poLyrTable->GetTotalRecordCount(); i++)
        {
            if (!m_poLyrTable->SelectRow(i))
            {
                if (m_poLyrTable->HasGotError())
                    break;
                else
                    continue;
            }
#if SIZEOF_VOIDP < 8
            if (i > INT32_MAX)
            {
                // CPLQuadTreeInsertWithBounds stores row index values as void*
                // This would overflow here.
                m_eSpatialIndexState = SPI_INVALID;
                break;
            }
#endif

            const OGRField *psField =
                m_poLyrTable->GetFieldValue(m_iGeomFieldIdx);
            if (psField != nullptr)
            {
                if (m_eSpatialIndexState == SPI_IN_BUILDING)
                {
                    OGREnvelope sFeatureEnvelope;
                    if (m_poLyrTable->GetFeatureExtent(psField,
                                                       &sFeatureEnvelope))
                    {
                        CPLRectObj sBounds;
                        sBounds.minx = sFeatureEnvelope.MinX;
                        sBounds.miny = sFeatureEnvelope.MinY;
                        sBounds.maxx = sFeatureEnvelope.MaxX;
                        sBounds.maxy = sFeatureEnvelope.MaxY;
                        CPLQuadTreeInsertWithBounds(
                            m_pQuadTree,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)),
                            &sBounds);
                    }
                }

                if (m_poLyrTable->DoesGeometryIntersectsFilterEnvelope(psField))
                {
                    OGRGeometry *poGeom =
                        m_poGeomConverter->GetAsGeometry(psField);
                    if (poGeom != nullptr && FilterGeometry(poGeom))
                    {
                        if (m_eSpatialIndexState == SPI_IN_BUILDING)
                        {
                            if (nCount == nFilteredFeatureCountAlloc)
                            {
                                nFilteredFeatureCountAlloc =
                                    4 * nFilteredFeatureCountAlloc / 3 + 1024;
                                m_pahFilteredFeatures = static_cast<void **>(
                                    CPLRealloc(m_pahFilteredFeatures,
                                               sizeof(void *) *
                                                   nFilteredFeatureCountAlloc));
                            }
                            m_pahFilteredFeatures[nCount] =
                                reinterpret_cast<void *>(
                                    static_cast<uintptr_t>(i));
                        }
                        nCount++;
                    }
                    delete poGeom;
                }
            }
        }
        if (m_eSpatialIndexState == SPI_IN_BUILDING)
        {
            m_nFilteredFeatureCount = nCount;
            m_eSpatialIndexState = SPI_COMPLETED;
        }

        return nCount;
    }
    /* Only simple attribute filter ? */
    else if (m_poFilterGeom == nullptr && m_poAttributeIterator != nullptr &&
             m_bIteratorSufficientToEvaluateFilter)
    {
        return m_poAttributeIterator->GetRowCount();
    }

    return OGRLayer::GetFeatureCount(bForce);
}

/***********************************************************************/
/*                         TestCapability()                            */
/***********************************************************************/

int OGROpenFileGDBLayer::TestCapability(const char *pszCap)
{
    if (!BuildLayerDefinition())
        return FALSE;

    if (EQUAL(pszCap, OLCCreateField) || EQUAL(pszCap, OLCDeleteField) ||
        EQUAL(pszCap, OLCAlterFieldDefn) ||
        EQUAL(pszCap, OLCAlterGeomFieldDefn) ||
        EQUAL(pszCap, OLCSequentialWrite) || EQUAL(pszCap, OLCRandomWrite) ||
        EQUAL(pszCap, OLCDeleteFeature) || EQUAL(pszCap, OLCRename))
    {
        return m_bEditable;
    }

    if (EQUAL(pszCap, OLCFastFeatureCount))
    {
        return ((m_poFilterGeom == nullptr || m_iGeomFieldIdx < 0) &&
                m_poAttrQuery == nullptr);
    }
    else if (EQUAL(pszCap, OLCFastSetNextByIndex))
    {
        return (m_poLyrTable->GetValidRecordCount() ==
                    m_poLyrTable->GetTotalRecordCount() &&
                m_poAttributeIterator == nullptr &&
                m_poSpatialIndexIterator == nullptr);
    }
    else if (EQUAL(pszCap, OLCRandomRead))
    {
        return TRUE;
    }
    else if (EQUAL(pszCap, OLCFastGetExtent))
    {
        return TRUE;
    }
    else if (EQUAL(pszCap, OLCFastGetExtent3D))
    {
        if (m_poFilterGeom == nullptr && m_poAttrQuery == nullptr &&
            m_iGeomFieldIdx >= 0 && m_poLyrTable->GetValidRecordCount() > 0)
        {
            FileGDBGeomField *poGDBGeomField =
                reinterpret_cast<FileGDBGeomField *>(
                    m_poLyrTable->GetField(m_iGeomFieldIdx));
            if (!std::isnan(poGDBGeomField->GetXMin()))
            {
                if (!std::isnan(poGDBGeomField->GetZMin()))
                {
                    return TRUE;
                }
                else
                {
                    return !OGR_GT_HasZ(m_eGeomType);
                }
            }
        }
        return FALSE;
    }
    else if (EQUAL(pszCap, OLCIgnoreFields))
    {
        return TRUE;
    }
    else if (EQUAL(pszCap, OLCStringsAsUTF8))
    {
        return TRUE; /* ? */
    }

    else if (EQUAL(pszCap, OLCMeasuredGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCCurveGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
    {
        return m_eSpatialIndexState == SPI_COMPLETED ||
               (m_poLyrTable->CanUseIndices() &&
                m_poLyrTable->HasSpatialIndex());
    }

    return FALSE;
}

/***********************************************************************/
/*                         HasIndexForField()                          */
/***********************************************************************/

bool OGROpenFileGDBLayer::HasIndexForField(const char *pszFieldName)
{
    if (!BuildLayerDefinition())
        return false;
    if (!m_poLyrTable->CanUseIndices())
        return false;
    int nTableColIdx = m_poLyrTable->GetFieldIdx(pszFieldName);
    return (nTableColIdx >= 0 &&
            m_poLyrTable->GetField(nTableColIdx)->HasIndex());
}

/***********************************************************************/
/*                             BuildIndex()                            */
/***********************************************************************/

FileGDBIterator *OGROpenFileGDBLayer::BuildIndex(const char *pszFieldName,
                                                 int bAscending, int op,
                                                 swq_expr_node *poValue)
{
    if (!BuildLayerDefinition())
        return nullptr;

    int idx = GetLayerDefn()->GetFieldIndex(pszFieldName);
    if (idx < 0)
        return nullptr;
    OGRFieldDefn *poFieldDefn = GetLayerDefn()->GetFieldDefn(idx);

    int nTableColIdx = m_poLyrTable->GetFieldIdx(pszFieldName);
    if (nTableColIdx >= 0 && m_poLyrTable->GetField(nTableColIdx)->HasIndex())
    {
        if (op < 0)
            return FileGDBIterator::BuildIsNotNull(m_poLyrTable, nTableColIdx,
                                                   bAscending);

        OGRField sValue;
        if (FillTargetValueFromSrcExpr(poFieldDefn, &sValue, poValue))
        {
            FileGDBSQLOp eOp;
            switch (op)
            {
                case SWQ_LE:
                    eOp = FGSO_LE;
                    break;
                case SWQ_LT:
                    eOp = FGSO_LT;
                    break;
                case SWQ_EQ:
                    eOp = FGSO_EQ;
                    break;
                case SWQ_GE:
                    eOp = FGSO_GE;
                    break;
                case SWQ_GT:
                    eOp = FGSO_GT;
                    break;
                default:
                    return nullptr;
            }

            return FileGDBIterator::Build(m_poLyrTable, nTableColIdx,
                                          bAscending, eOp,
                                          poFieldDefn->GetType(), &sValue);
        }
    }
    return nullptr;
}

/***********************************************************************/
/*                          GetMinMaxValue()                           */
/***********************************************************************/

const OGRField *OGROpenFileGDBLayer::GetMinMaxValue(OGRFieldDefn *poFieldDefn,
                                                    int bIsMin, int &eOutType)
{
    eOutType = -1;
    if (!BuildLayerDefinition())
        return nullptr;
    if (!m_poLyrTable->CanUseIndices())
        return nullptr;

    const int nTableColIdx =
        m_poLyrTable->GetFieldIdx(poFieldDefn->GetNameRef());
    if (nTableColIdx >= 0 && m_poLyrTable->GetField(nTableColIdx)->HasIndex())
    {
        delete m_poIterMinMax;
        m_poIterMinMax =
            FileGDBIterator::BuildIsNotNull(m_poLyrTable, nTableColIdx, TRUE);
        if (m_poIterMinMax != nullptr)
        {
            const OGRField *poRet = (bIsMin)
                                        ? m_poIterMinMax->GetMinValue(eOutType)
                                        : m_poIterMinMax->GetMaxValue(eOutType);
            if (poRet == nullptr)
                eOutType = poFieldDefn->GetType();
            return poRet;
        }
    }
    return nullptr;
}

/***********************************************************************/
/*                        GetMinMaxSumCount()                          */
/***********************************************************************/

int OGROpenFileGDBLayer::GetMinMaxSumCount(OGRFieldDefn *poFieldDefn,
                                           double &dfMin, double &dfMax,
                                           double &dfSum, int &nCount)
{
    dfMin = 0.0;
    dfMax = 0.0;
    dfSum = 0.0;
    nCount = 0;
    if (!BuildLayerDefinition())
        return false;
    if (!m_poLyrTable->CanUseIndices())
        return false;

    int nTableColIdx = m_poLyrTable->GetFieldIdx(poFieldDefn->GetNameRef());
    if (nTableColIdx >= 0 && m_poLyrTable->GetField(nTableColIdx)->HasIndex())
    {
        auto poIter = std::unique_ptr<FileGDBIterator>(
            FileGDBIterator::BuildIsNotNull(m_poLyrTable, nTableColIdx, TRUE));
        if (poIter)
        {
            return poIter->GetMinMaxSumCount(dfMin, dfMax, dfSum, nCount);
        }
    }
    return false;
}

/************************************************************************/
/*                             GetDataset()                             */
/************************************************************************/

GDALDataset *OGROpenFileGDBLayer::GetDataset()
{
    return m_poDS;
}
