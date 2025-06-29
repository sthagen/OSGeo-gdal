/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  JP2 Box Reader (and GMLJP2 Interpreter)
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef GDAL_JP2READER_H_INCLUDED
#define GDAL_JP2READER_H_INCLUDED

#ifndef DOXYGEN_SKIP

#include "cpl_conv.h"
#include "cpl_minixml.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_priv.h"

/************************************************************************/
/*                              GDALJP2Box                              */
/************************************************************************/

class CPL_DLL GDALJP2Box
{

    VSILFILE *fpVSIL = nullptr;

    char szBoxType[5]{0, 0, 0, 0, 0};

    GIntBig nBoxOffset = -1;
    GIntBig nBoxLength = 0;

    GIntBig nDataOffset = -1;

    GByte abyUUID[16]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    GByte *pabyData = nullptr;

    bool m_bAllowGetFileSize = true;

    CPL_DISALLOW_COPY_ASSIGN(GDALJP2Box)

  public:
    explicit GDALJP2Box(VSILFILE * = nullptr);
    ~GDALJP2Box();

    void SetAllowGetFileSize(bool b)
    {
        m_bAllowGetFileSize = b;
    }

    int SetOffset(GIntBig nNewOffset);
    int ReadBox();

    int ReadFirst();
    int ReadNext();

    int ReadFirstChild(GDALJP2Box *poSuperBox);
    int ReadNextChild(GDALJP2Box *poSuperBox);

    GIntBig GetBoxOffset() const
    {
        return nBoxOffset;
    }

    GIntBig GetBoxLength() const
    {
        return nBoxLength;
    }

    GIntBig GetDataOffset() const
    {
        return nDataOffset;
    }

    GIntBig GetDataLength() const;

    const char *GetType()
    {
        return szBoxType;
    }

    GByte *ReadBoxData();

    int IsSuperBox();

    int DumpReadable(FILE *, int nIndentLevel = 0);

    VSILFILE *GetFILE()
    {
        return fpVSIL;
    }

    const GByte *GetUUID()
    {
        return abyUUID;
    }

    // write support
    void SetType(const char *);
    void SetWritableData(int nLength, const GByte *pabyData);
    void AppendWritableData(int nLength, const void *pabyDataIn);
    void AppendUInt32(GUInt32 nVal);
    void AppendUInt16(GUInt16 nVal);
    void AppendUInt8(GByte nVal);

    const GByte *GetWritableData() const
    {
        return pabyData;
    }

    GByte *GetWritableBoxData() const;

    // factory methods.
    static GDALJP2Box *CreateSuperBox(const char *pszType, int nCount,
                                      const GDALJP2Box *const *papoBoxes);
    static GDALJP2Box *CreateAsocBox(int nCount,
                                     const GDALJP2Box *const *papoBoxes);
    static GDALJP2Box *CreateLblBox(const char *pszLabel);
    static GDALJP2Box *CreateLabelledXMLAssoc(const char *pszLabel,
                                              const char *pszXML);
    static GDALJP2Box *CreateUUIDBox(const GByte *pabyUUID, int nDataSize,
                                     const GByte *pabyData);

    // JUMBF boxes (ISO/IEC 19566-5:2019)
    static GDALJP2Box *CreateJUMBFDescriptionBox(const GByte *pabyUUIDType,
                                                 const char *pszLabel);
    static GDALJP2Box *CreateJUMBFBox(const GDALJP2Box *poJUMBFDescriptionBox,
                                      int nCount,
                                      const GDALJP2Box *const *papoBoxes);
};

/************************************************************************/
/*                           GDALJP2Metadata                            */
/************************************************************************/

typedef struct _GDALJP2GeoTIFFBox GDALJP2GeoTIFFBox;

class CPL_DLL GDALJP2Metadata

{
  private:
    void CollectGMLData(GDALJP2Box *);
    int GMLSRSLookup(const char *pszURN);

    int nGeoTIFFBoxesCount;
    GDALJP2GeoTIFFBox *pasGeoTIFFBoxes;

    int nMSIGSize;
    GByte *pabyMSIGData;

    void GetGMLJP2GeoreferencingInfo(int &nEPSGCode, double adfOrigin[2],
                                     double adfXVector[2], double adfYVector[2],
                                     const char *&pszComment,
                                     CPLString &osDictBox, bool &bNeedAxisFlip);
    static CPLXMLNode *CreateGDALMultiDomainMetadataXML(GDALDataset *poSrcDS,
                                                        int bMainMDDomainOnly);

    CPL_DISALLOW_COPY_ASSIGN(GDALJP2Metadata)

  public:
    char **papszGMLMetadata;

    bool m_bHaveGeoTransform{};
    GDALGeoTransform m_gt{};
    bool bPixelIsPoint;

    OGRSpatialReference m_oSRS{};

    int nGCPCount;
    GDAL_GCP *pasGCPList;

    char **papszRPCMD;

    char **papszMetadata; /* TIFFTAG_?RESOLUTION* for now from resd box */
    char *pszXMPMetadata;
    char *pszGDALMultiDomainMetadata; /* as serialized XML */
    char *pszXMLIPR; /* if an IPR box with XML content has been found */

    void ReadBox(VSILFILE *fpVSIL, GDALJP2Box &oBox, int &iBox);

  public:
    GDALJP2Metadata();
    ~GDALJP2Metadata();

    int ReadBoxes(VSILFILE *fpVSIL);

    int ParseJP2GeoTIFF();
    int ParseMSIG();
    int ParseGMLCoverageDesc();

    int ReadAndParse(VSILFILE *fpVSIL, int nGEOJP2Index = 0,
                     int nGMLJP2Index = 1, int nMSIGIndex = 2,
                     int *pnIndexUsed = nullptr);
    int ReadAndParse(const char *pszFilename, int nGEOJP2Index = 0,
                     int nGMLJP2Index = 1, int nMSIGIndex = 2,
                     int nWorldFileIndex = 3, int *pnIndexUsed = nullptr);

    // Write oriented.
    void SetSpatialRef(const OGRSpatialReference *poSRS);
    void SetGeoTransform(const GDALGeoTransform &gt);
    void SetGCPs(int, const GDAL_GCP *);
    void SetRPCMD(char **papszRPCMDIn);

    GDALJP2Box *CreateJP2GeoTIFF();
    GDALJP2Box *CreateGMLJP2(int nXSize, int nYSize);
    GDALJP2Box *CreateGMLJP2V2(int nXSize, int nYSize,
                               const char *pszDefFilename,
                               GDALDataset *poSrcDS);

    static GDALJP2Box *
    CreateGDALMultiDomainMetadataXMLBox(GDALDataset *poSrcDS,
                                        int bMainMDDomainOnly);
    static GDALJP2Box **CreateXMLBoxes(GDALDataset *poSrcDS, int *pnBoxes);
    static GDALJP2Box *CreateXMPBox(GDALDataset *poSrcDS);
    static GDALJP2Box *CreateIPRBox(GDALDataset *poSrcDS);
    static int IsUUID_MSI(const GByte *abyUUID);
    static int IsUUID_XMP(const GByte *abyUUID);

    static bool IsSRSCompatible(const OGRSpatialReference *poSRS);
};

CPLXMLNode *GDALGetJPEG2000Structure(const char *pszFilename, VSILFILE *fp,
                                     CSLConstList papszOptions);

const char CPL_DLL *GDALGetJPEG2000Reversibility(const char *pszFilename,
                                                 VSILFILE *fp);
#endif /* #ifndef DOXYGEN_SKIP */

#endif /* ndef GDAL_JP2READER_H_INCLUDED */
