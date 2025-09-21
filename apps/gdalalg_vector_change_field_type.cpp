/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  "change-field-type" step of "vector pipeline"
 * Author:   Alessandro Pasotti <elpaso at itopen dot it>
 *
 ******************************************************************************
 * Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "gdalalg_vector_change_field_type.h"
#include "gdal_priv.h"

//! @cond Doxygen_Suppress

#ifndef _
#define _(x) (x)
#endif

/************************************************************************/
/*                      GDALVectorChangeFieldTypeAlgorithm()            */
/************************************************************************/

GDALVectorChangeFieldTypeAlgorithm::GDALVectorChangeFieldTypeAlgorithm(
    bool standaloneStep)
    : GDALVectorPipelineStepAlgorithm(NAME, DESCRIPTION, HELP_URL,
                                      standaloneStep)
{
    auto &layerArg = AddActiveLayerArg(&m_activeLayer);
    auto &fieldNameArg = AddFieldNameArg(&m_fieldName)
                             .SetRequired()
                             .SetMutualExclusionGroup("name-or-type");
    SetAutoCompleteFunctionForFieldName(fieldNameArg, layerArg, m_inputDataset);
    AddFieldTypeSubtypeArg(&m_srcFieldType, &m_srcFieldSubType,
                           &m_srcFieldTypeSubTypeStr, "src-field-type",
                           _("Source field type or subtype"))
        .SetRequired()
        .SetMutualExclusionGroup("name-or-type");
    AddFieldTypeSubtypeArg(&m_newFieldType, &m_newFieldSubType,
                           &m_newFieldTypeSubTypeStr, std::string(),
                           _("Target field type or subtype"))
        .AddAlias("dst-field-type")
        .SetRequired();
    AddValidationAction(
        [this]
        {
            if (!m_inputDataset.empty())
            {
                auto mInDS = m_inputDataset[0].GetDatasetRef();
                auto layer = m_activeLayer.empty()
                                 ? mInDS->GetLayer(0)
                                 : mInDS->GetLayerByName(m_activeLayer.c_str());
                if (!layer)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Cannot find layer '%s'", m_activeLayer.c_str());
                    return false;
                }
                if (!m_fieldName.empty() &&
                    layer->GetLayerDefn()->GetFieldIndex(m_fieldName.c_str()) <
                        0)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Cannot find field '%s' in layer '%s'",
                             m_fieldName.c_str(), layer->GetName());
                    return false;
                }
                return true;
            }
            return false;
        });
}

/************************************************************************/
/*                   GDALVectorChangeFieldTypeAlgorithmLayer            */
/************************************************************************/

namespace
{
class GDALVectorChangeFieldTypeAlgorithmLayer final
    : public GDALVectorPipelineOutputLayer
{
  public:
    GDALVectorChangeFieldTypeAlgorithmLayer(
        OGRLayer &oSrcLayer, const std::string &activeLayer,
        const std::string &fieldName, const OGRFieldType srcFieldType,
        const OGRFieldSubType srcFieldSubType, const OGRFieldType newFieldType,
        const OGRFieldSubType newFieldSubType)
        : GDALVectorPipelineOutputLayer(oSrcLayer)
    {

        m_poFeatureDefn = oSrcLayer.GetLayerDefn()->Clone();
        m_poFeatureDefn->Reference();

        if (activeLayer.empty() || activeLayer == GetDescription())
        {
            if (!fieldName.empty())
            {
                m_fieldIndex =
                    m_poFeatureDefn->GetFieldIndex(fieldName.c_str());
                if (m_fieldIndex >= 0)
                {
                    auto poFieldDefn =
                        m_poFeatureDefn->GetFieldDefn(m_fieldIndex);
                    if (poFieldDefn->GetType() != newFieldType)
                    {
                        m_passThrough = false;
                    }

                    // Set to OFSTNone to bypass the check that prevents changing the type
                    poFieldDefn->SetSubType(OFSTNone);
                    poFieldDefn->SetType(newFieldType);
                    poFieldDefn->SetSubType(newFieldSubType);
                }
            }
            else
            {
                for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); ++i)
                {
                    auto poFieldDefn = m_poFeatureDefn->GetFieldDefn(i);
                    if (poFieldDefn->GetType() == srcFieldType &&
                        poFieldDefn->GetSubType() == srcFieldSubType)
                    {
                        m_passThrough = false;

                        // Set to OFSTNone to bypass the check that prevents changing the type
                        poFieldDefn->SetSubType(OFSTNone);
                        poFieldDefn->SetType(newFieldType);
                        poFieldDefn->SetSubType(newFieldSubType);
                    }
                }
            }

            for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
            {
                m_identityMap.push_back(i);
            }
        }
    }

    ~GDALVectorChangeFieldTypeAlgorithmLayer() override
    {
        m_poFeatureDefn->Release();
    }

    const OGRFeatureDefn *GetLayerDefn() const override
    {
        return m_poFeatureDefn;
    }

    void TranslateFeature(
        std::unique_ptr<OGRFeature> poSrcFeature,
        std::vector<std::unique_ptr<OGRFeature>> &apoOutFeatures) override
    {
        if (m_passThrough)
        {
            apoOutFeatures.push_back(std::move(poSrcFeature));
        }
        else
        {
            auto poDstFeature = std::make_unique<OGRFeature>(m_poFeatureDefn);
            const auto result{poDstFeature->SetFrom(
                poSrcFeature.get(), m_identityMap.data(), false, true)};
            if (result != OGRERR_NONE)
            {
                if (m_fieldIndex >= 0)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Cannot convert field '%s' to new type, setting "
                             "it to NULL",
                             m_poFeatureDefn->GetFieldDefn(m_fieldIndex)
                                 ->GetNameRef());
                }
            }
            else
            {
                poDstFeature->SetFID(poSrcFeature->GetFID());
                apoOutFeatures.push_back(std::move(poDstFeature));
            }
        }
    }

    int TestCapability(const char *pszCap) const override
    {
        if (EQUAL(pszCap, OLCStringsAsUTF8) ||
            EQUAL(pszCap, OLCCurveGeometries) ||
            EQUAL(pszCap, OLCZGeometries) ||
            EQUAL(pszCap, OLCMeasuredGeometries))
            return m_srcLayer.TestCapability(pszCap);
        return false;
    }

  private:
    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    int m_fieldIndex{-1};
    bool m_passThrough = true;
    std::vector<int> m_identityMap{};

    CPL_DISALLOW_COPY_ASSIGN(GDALVectorChangeFieldTypeAlgorithmLayer)
};

}  // namespace

/************************************************************************/
/*                GDALVectorChangeFieldTypeAlgorithm::RunStep()                    */
/************************************************************************/

bool GDALVectorChangeFieldTypeAlgorithm::RunStep(GDALPipelineStepRunContext &)
{
    auto poSrcDS = m_inputDataset[0].GetDatasetRef();
    CPLAssert(poSrcDS);

    CPLAssert(m_outputDataset.GetName().empty());
    CPLAssert(!m_outputDataset.GetDatasetRef());

    const int nLayerCount = poSrcDS->GetLayerCount();

    auto outDS = std::make_unique<GDALVectorPipelineOutputDataset>(*poSrcDS);

    bool ret = true;
    for (int i = 0; ret && i < nLayerCount; ++i)
    {
        auto poSrcLayer = poSrcDS->GetLayer(i);
        ret = (poSrcLayer != nullptr);
        if (ret)
        {
            outDS->AddLayer(
                *poSrcLayer,
                std::make_unique<GDALVectorChangeFieldTypeAlgorithmLayer>(
                    *poSrcLayer, m_activeLayer, m_fieldName, m_srcFieldType,
                    m_srcFieldSubType, m_newFieldType, m_newFieldSubType));
        }
    }

    if (ret)
        m_outputDataset.Set(std::move(outDS));

    return ret;
}

GDALVectorChangeFieldTypeAlgorithmStandalone::
    ~GDALVectorChangeFieldTypeAlgorithmStandalone() = default;

//! @endcond
