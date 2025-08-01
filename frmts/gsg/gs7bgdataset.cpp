/****************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Implements the Golden Software Surfer 7 Binary Grid Format.
 * Author:   Adam Guernsey, adam@ctech.com
 *           (Based almost entirely on gsbgdataset.cpp by Kevin Locke)
 *           Create functions added by Russell Jurgensen.
 *
 ****************************************************************************
 * Copyright (c) 2007, Adam Guernsey <adam@ctech.com>
 * Copyright (c) 2009-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include <cassert>
#include <cfloat>
#include <climits>
#include <cmath>
#include <limits>

#include "gdal_frmts.h"
#include "gdal_pam.h"

/************************************************************************/
/* ==================================================================== */
/*                GS7BGDataset                */
/* ==================================================================== */
/************************************************************************/

class GS7BGRasterBand;

constexpr double dfDefaultNoDataValue = 1.701410009187828e+38f;

class GS7BGDataset final : public GDALPamDataset
{
    friend class GS7BGRasterBand;

    double dfNoData_Value;
    static const size_t nHEADER_SIZE;
    size_t nData_Position;

    static CPLErr WriteHeader(VSILFILE *fp, GInt32 nXSize, GInt32 nYSize,
                              double dfMinX, double dfMaxX, double dfMinY,
                              double dfMaxY, double dfMinZ, double dfMaxZ);

    VSILFILE *fp;

  public:
    GS7BGDataset()
        : /* NOTE:  This is not mentioned in the spec, but Surfer 8 uses this
             value */
          /* 0x7effffee (Little Endian: eeffff7e) */
          dfNoData_Value(dfDefaultNoDataValue), nData_Position(0), fp(nullptr)
    {
    }

    ~GS7BGDataset();

    static int Identify(GDALOpenInfo *);
    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBandsIn, GDALDataType eType,
                               char **papszParamList);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;
};

const size_t GS7BGDataset::nHEADER_SIZE = 100;

constexpr long nHEADER_TAG = 0x42525344;
constexpr long nGRID_TAG = 0x44495247;
constexpr long nDATA_TAG = 0x41544144;
#if 0 /* Unused */
const long  nFAULT_TAG = 0x49544c46;
#endif

/************************************************************************/
/* ==================================================================== */
/*                            GS7BGRasterBand                           */
/* ==================================================================== */
/************************************************************************/

class GS7BGRasterBand final : public GDALPamRasterBand
{
    friend class GS7BGDataset;

    double dfMinX;
    double dfMaxX;
    double dfMinY;
    double dfMaxY;
    double dfMinZ;
    double dfMaxZ;

    double *pafRowMinZ;
    double *pafRowMaxZ;
    int nMinZRow;
    int nMaxZRow;

    CPLErr ScanForMinMaxZ();

  public:
    GS7BGRasterBand(GS7BGDataset *, int);
    ~GS7BGRasterBand();

    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IWriteBlock(int, int, void *) override;
    double GetMinimum(int *pbSuccess = nullptr) override;
    double GetMaximum(int *pbSuccess = nullptr) override;

    double GetNoDataValue(int *pbSuccess = nullptr) override;
};

/************************************************************************/
/*                           GS7BGRasterBand()                          */
/************************************************************************/

GS7BGRasterBand::GS7BGRasterBand(GS7BGDataset *poDSIn, int nBandIn)
    : dfMinX(0.0), dfMaxX(0.0), dfMinY(0.0), dfMaxY(0.0), dfMinZ(0.0),
      dfMaxZ(0.0), pafRowMinZ(nullptr), pafRowMaxZ(nullptr), nMinZRow(-1),
      nMaxZRow(-1)

{
    poDS = poDSIn;
    nBand = nBandIn;

    eDataType = GDT_Float64;

    nBlockXSize = poDS->GetRasterXSize();
    nBlockYSize = 1;
}

/************************************************************************/
/*                           ~GSBGRasterBand()                          */
/************************************************************************/

GS7BGRasterBand::~GS7BGRasterBand()

{
    CPLFree(pafRowMinZ);
    CPLFree(pafRowMaxZ);
}

/************************************************************************/
/*                          ScanForMinMaxZ()                            */
/************************************************************************/

CPLErr GS7BGRasterBand::ScanForMinMaxZ()

{
    GS7BGDataset *poGDS = cpl::down_cast<GS7BGDataset *>(poDS);
    double *pafRowVals =
        (double *)VSI_MALLOC2_VERBOSE(nRasterXSize, sizeof(double));

    if (pafRowVals == nullptr)
    {
        return CE_Failure;
    }

    double dfNewMinZ = std::numeric_limits<double>::max();
    double dfNewMaxZ = std::numeric_limits<double>::lowest();
    int nNewMinZRow = 0;
    int nNewMaxZRow = 0;

    /* Since we have to scan, lets calc. statistics too */
    double dfSum = 0.0;
    double dfSum2 = 0.0;
    unsigned long nValuesRead = 0;
    for (int iRow = 0; iRow < nRasterYSize; iRow++)
    {
        CPLErr eErr = IReadBlock(0, iRow, pafRowVals);
        if (eErr != CE_None)
        {
            VSIFree(pafRowVals);
            return CE_Failure;
        }

        pafRowMinZ[iRow] = std::numeric_limits<float>::max();
        pafRowMaxZ[iRow] = std::numeric_limits<float>::lowest();
        for (int iCol = 0; iCol < nRasterXSize; iCol++)
        {
            if (pafRowVals[iCol] == poGDS->dfNoData_Value)
                continue;

            if (pafRowVals[iCol] < pafRowMinZ[iRow])
                pafRowMinZ[iRow] = pafRowVals[iCol];

            if (pafRowVals[iCol] > pafRowMinZ[iRow])
                pafRowMaxZ[iRow] = pafRowVals[iCol];

            dfSum += pafRowVals[iCol];
            dfSum2 += pafRowVals[iCol] * pafRowVals[iCol];
            nValuesRead++;
        }

        if (pafRowMinZ[iRow] < dfNewMinZ)
        {
            dfNewMinZ = pafRowMinZ[iRow];
            nNewMinZRow = iRow;
        }

        if (pafRowMaxZ[iRow] > dfNewMaxZ)
        {
            dfNewMaxZ = pafRowMaxZ[iRow];
            nNewMaxZRow = iRow;
        }
    }

    VSIFree(pafRowVals);

    if (nValuesRead == 0)
    {
        dfMinZ = 0.0;
        dfMaxZ = 0.0;
        nMinZRow = 0;
        nMaxZRow = 0;
        return CE_None;
    }

    dfMinZ = dfNewMinZ;
    dfMaxZ = dfNewMaxZ;
    nMinZRow = nNewMinZRow;
    nMaxZRow = nNewMaxZRow;

    double dfMean = dfSum / nValuesRead;
    double dfStdDev = sqrt((dfSum2 / nValuesRead) - (dfMean * dfMean));
    SetStatistics(dfMinZ, dfMaxZ, dfMean, dfStdDev);

    return CE_None;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr GS7BGRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)

{
    if (nBlockYOff < 0 || nBlockYOff > nRasterYSize - 1 || nBlockXOff != 0)
        return CE_Failure;

    GS7BGDataset *poGDS = cpl::down_cast<GS7BGDataset *>(poDS);

    if (VSIFSeekL(poGDS->fp,
                  (poGDS->nData_Position +
                   sizeof(double) * static_cast<vsi_l_offset>(nRasterXSize) *
                       (nRasterYSize - nBlockYOff - 1)),
                  SEEK_SET) != 0)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to seek to beginning of grid row.\n");
        return CE_Failure;
    }

    if (VSIFReadL(pImage, sizeof(double), nBlockXSize, poGDS->fp) !=
        static_cast<unsigned>(nBlockXSize))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to read block from grid file.\n");
        return CE_Failure;
    }

#ifdef CPL_MSB
    double *pfImage = (double *)pImage;
    for (int iPixel = 0; iPixel < nBlockXSize; iPixel++)
        CPL_LSBPTR64(pfImage + iPixel);
#endif

    return CE_None;
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr GS7BGRasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff,
                                    void *pImage)

{
    if (eAccess == GA_ReadOnly)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Unable to write block, dataset opened read only.\n");
        return CE_Failure;
    }

    if (nBlockYOff < 0 || nBlockYOff > nRasterYSize - 1 || nBlockXOff != 0)
        return CE_Failure;

    GS7BGDataset *poGDS = cpl::down_cast<GS7BGDataset *>(poDS);

    if (pafRowMinZ == nullptr || pafRowMaxZ == nullptr || nMinZRow < 0 ||
        nMaxZRow < 0)
    {
        pafRowMinZ =
            (double *)VSI_MALLOC2_VERBOSE(nRasterYSize, sizeof(double));
        if (pafRowMinZ == nullptr)
        {
            return CE_Failure;
        }

        pafRowMaxZ =
            (double *)VSI_MALLOC2_VERBOSE(nRasterYSize, sizeof(double));
        if (pafRowMaxZ == nullptr)
        {
            VSIFree(pafRowMinZ);
            pafRowMinZ = nullptr;
            return CE_Failure;
        }

        CPLErr eErr = ScanForMinMaxZ();
        if (eErr != CE_None)
            return eErr;
    }

    if (VSIFSeekL(poGDS->fp,
                  GS7BGDataset::nHEADER_SIZE +
                      sizeof(double) * nRasterXSize *
                          (nRasterYSize - nBlockYOff - 1),
                  SEEK_SET) != 0)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to seek to beginning of grid row.\n");
        return CE_Failure;
    }

    double *pdfImage = (double *)pImage;
    pafRowMinZ[nBlockYOff] = std::numeric_limits<double>::max();
    pafRowMaxZ[nBlockYOff] = std::numeric_limits<double>::lowest();
    for (int iPixel = 0; iPixel < nBlockXSize; iPixel++)
    {
        if (pdfImage[iPixel] != poGDS->dfNoData_Value)
        {
            if (pdfImage[iPixel] < pafRowMinZ[nBlockYOff])
                pafRowMinZ[nBlockYOff] = pdfImage[iPixel];

            if (pdfImage[iPixel] > pafRowMaxZ[nBlockYOff])
                pafRowMaxZ[nBlockYOff] = pdfImage[iPixel];
        }

        CPL_LSBPTR64(pdfImage + iPixel);
    }

    if (VSIFWriteL(pImage, sizeof(double), nBlockXSize, poGDS->fp) !=
        static_cast<unsigned>(nBlockXSize))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write block to grid file.\n");
        return CE_Failure;
    }

    /* Update min/max Z values as appropriate */
    bool bHeaderNeedsUpdate = false;
    if (nMinZRow == nBlockYOff && pafRowMinZ[nBlockYOff] > dfMinZ)
    {
        double dfNewMinZ = std::numeric_limits<double>::max();
        for (int iRow = 0; iRow < nRasterYSize; iRow++)
        {
            if (pafRowMinZ[iRow] < dfNewMinZ)
            {
                dfNewMinZ = pafRowMinZ[iRow];
                nMinZRow = iRow;
            }
        }

        if (dfNewMinZ != dfMinZ)
        {
            dfMinZ = dfNewMinZ;
            bHeaderNeedsUpdate = true;
        }
    }

    if (nMaxZRow == nBlockYOff && pafRowMaxZ[nBlockYOff] < dfMaxZ)
    {
        double dfNewMaxZ = std::numeric_limits<double>::lowest();
        for (int iRow = 0; iRow < nRasterYSize; iRow++)
        {
            if (pafRowMaxZ[iRow] > dfNewMaxZ)
            {
                dfNewMaxZ = pafRowMaxZ[iRow];
                nMaxZRow = iRow;
            }
        }

        if (dfNewMaxZ != dfMaxZ)
        {
            dfMaxZ = dfNewMaxZ;
            bHeaderNeedsUpdate = true;
        }
    }

    if (pafRowMinZ[nBlockYOff] < dfMinZ || pafRowMaxZ[nBlockYOff] > dfMaxZ)
    {
        if (pafRowMinZ[nBlockYOff] < dfMinZ)
        {
            dfMinZ = pafRowMinZ[nBlockYOff];
            nMinZRow = nBlockYOff;
        }

        if (pafRowMaxZ[nBlockYOff] > dfMaxZ)
        {
            dfMaxZ = pafRowMaxZ[nBlockYOff];
            nMaxZRow = nBlockYOff;
        }

        bHeaderNeedsUpdate = true;
    }

    if (bHeaderNeedsUpdate && dfMaxZ > dfMinZ)
    {
        CPLErr eErr =
            poGDS->WriteHeader(poGDS->fp, nRasterXSize, nRasterYSize, dfMinX,
                               dfMaxX, dfMinY, dfMaxY, dfMinZ, dfMaxZ);
        return eErr;
    }

    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double GS7BGRasterBand::GetNoDataValue(int *pbSuccess)
{
    GS7BGDataset *poGDS = cpl::down_cast<GS7BGDataset *>(poDS);
    if (pbSuccess)
        *pbSuccess = TRUE;

    return poGDS->dfNoData_Value;
}

/************************************************************************/
/*                             GetMinimum()                             */
/************************************************************************/

double GS7BGRasterBand::GetMinimum(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = TRUE;

    return dfMinZ;
}

/************************************************************************/
/*                             GetMaximum()                             */
/************************************************************************/

double GS7BGRasterBand::GetMaximum(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = TRUE;

    return dfMaxZ;
}

/************************************************************************/
/* ==================================================================== */
/*                            GS7BGDataset                              */
/* ==================================================================== */
/************************************************************************/

GS7BGDataset::~GS7BGDataset()

{
    FlushCache(true);
    if (fp != nullptr)
        VSIFCloseL(fp);
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int GS7BGDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    /* Check for signature - for GS7BG the signature is the */
    /* nHEADER_TAG with reverse byte order.                 */
    if (poOpenInfo->nHeaderBytes < 4 ||
        !STARTS_WITH_CI((const char *)poOpenInfo->pabyHeader, "DSRB"))
    {
        return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *GS7BGDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!Identify(poOpenInfo) || poOpenInfo->fpL == nullptr)
    {
        return nullptr;
    }

    /* ------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                            */
    /* ------------------------------------------------------------------- */
    GS7BGDataset *poDS = new GS7BGDataset();
    poDS->eAccess = poOpenInfo->eAccess;
    poDS->fp = poOpenInfo->fpL;
    poOpenInfo->fpL = nullptr;

    /* ------------------------------------------------------------------- */
    /*      Read the header. The Header section must be the first section  */
    /*      in the file.                                                   */
    /* ------------------------------------------------------------------- */
    if (VSIFSeekL(poDS->fp, 0, SEEK_SET) != 0)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to seek to start of grid file header.\n");
        return nullptr;
    }

    GInt32 nTag;
    if (VSIFReadL((void *)&nTag, sizeof(GInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read Tag.\n");
        return nullptr;
    }

    CPL_LSBPTR32(&nTag);

    if (nTag != nHEADER_TAG)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Header tag not found.\n");
        return nullptr;
    }

    GUInt32 nSize;
    if (VSIFReadL((void *)&nSize, sizeof(GUInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to read file section size.\n");
        return nullptr;
    }

    CPL_LSBPTR32(&nSize);

    GInt32 nVersion;
    if (VSIFReadL((void *)&nVersion, sizeof(GInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read file version.\n");
        return nullptr;
    }

    CPL_LSBPTR32(&nVersion);

    if (nVersion != 1 && nVersion != 2)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Incorrect file version (%d).",
                 nVersion);
        return nullptr;
    }

    // advance until the grid tag is found
    while (nTag != nGRID_TAG)
    {
        if (VSIFReadL((void *)&nTag, sizeof(GInt32), 1, poDS->fp) != 1)
        {
            delete poDS;
            CPLError(CE_Failure, CPLE_FileIO, "Unable to read Tag.\n");
            return nullptr;
        }

        CPL_LSBPTR32(&nTag);

        if (VSIFReadL((void *)&nSize, sizeof(GUInt32), 1, poDS->fp) != 1)
        {
            delete poDS;
            CPLError(CE_Failure, CPLE_FileIO,
                     "Unable to read file section size.\n");
            return nullptr;
        }

        CPL_LSBPTR32(&nSize);

        if (nTag != nGRID_TAG)
        {
            if (VSIFSeekL(poDS->fp, nSize, SEEK_CUR) != 0)
            {
                delete poDS;
                CPLError(CE_Failure, CPLE_FileIO,
                         "Unable to seek to end of file section.\n");
                return nullptr;
            }
        }
    }

    /* --------------------------------------------------------------------*/
    /*      Read the grid.                                                 */
    /* --------------------------------------------------------------------*/
    /* Parse number of Y axis grid rows */
    GInt32 nRows;
    if (VSIFReadL((void *)&nRows, sizeof(GInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read raster Y size.\n");
        return nullptr;
    }
    CPL_LSBPTR32(&nRows);
    poDS->nRasterYSize = nRows;

    /* Parse number of X axis grid columns */
    GInt32 nCols;
    if (VSIFReadL((void *)&nCols, sizeof(GInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read raster X size.\n");
        return nullptr;
    }
    CPL_LSBPTR32(&nCols);
    poDS->nRasterXSize = nCols;

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize))
    {
        delete poDS;
        return nullptr;
    }

    /* --------------------------------------------------------------------*/
    /*      Create band information objects.                               */
    /* --------------------------------------------------------------------*/
    GS7BGRasterBand *poBand = new GS7BGRasterBand(poDS, 1);
    poDS->SetBand(1, poBand);

    // find the min X Value of the grid
    double dfTemp;
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read minimum X value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poBand->dfMinX = dfTemp;

    // find the min Y value of the grid
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read minimum X value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poBand->dfMinY = dfTemp;

    // find the spacing between adjacent nodes in the X direction
    // (between columns)
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to read spacing in X value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poBand->dfMaxX = poBand->dfMinX + (dfTemp * (nCols - 1));

    // find the spacing between adjacent nodes in the Y direction
    // (between rows)
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to read spacing in Y value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poBand->dfMaxY = poBand->dfMinY + (dfTemp * (nRows - 1));

    // set the z min
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read Z min value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poBand->dfMinZ = dfTemp;

    // set the z max
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read Z max value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poBand->dfMaxZ = dfTemp;

    // read and ignore the rotation value
    //(This is not used in the current version).
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read rotation value.\n");
        return nullptr;
    }

    // read and set the cell blank value
    if (VSIFReadL((void *)&dfTemp, sizeof(double), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to Blank value.\n");
        return nullptr;
    }
    CPL_LSBPTR64(&dfTemp);
    poDS->dfNoData_Value = dfTemp;

    /* --------------------------------------------------------------------*/
    /*      Set the current offset of the grid data.                       */
    /* --------------------------------------------------------------------*/
    if (VSIFReadL((void *)&nTag, sizeof(GInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to read Tag.\n");
        return nullptr;
    }

    CPL_LSBPTR32(&nTag);
    if (nTag != nDATA_TAG)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Data tag not found.\n");
        return nullptr;
    }

    if (VSIFReadL((void *)&nSize, sizeof(GInt32), 1, poDS->fp) != 1)
    {
        delete poDS;
        CPLError(CE_Failure, CPLE_FileIO, "Unable to data section size.\n");
        return nullptr;
    }

    poDS->nData_Position = (size_t)VSIFTellL(poDS->fp);

    /* --------------------------------------------------------------------*/
    /*      Initialize any PAM information.                                */
    /* --------------------------------------------------------------------*/
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for external overviews.                                   */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename,
                                poOpenInfo->GetSiblingFiles());

    return poDS;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr GS7BGDataset::GetGeoTransform(GDALGeoTransform &gt) const
{
    const GS7BGRasterBand *poGRB =
        cpl::down_cast<const GS7BGRasterBand *>(GetRasterBand(1));

    if (poGRB == nullptr)
    {
        gt = GDALGeoTransform();
        return CE_Failure;
    }

    /* check if we have a PAM GeoTransform stored */
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErr eErr = GDALPamDataset::GetGeoTransform(gt);
    CPLPopErrorHandler();

    if (eErr == CE_None)
        return CE_None;

    if (nRasterXSize == 1 || nRasterYSize == 1)
        return CE_Failure;

    /* calculate pixel size first */
    gt[1] = (poGRB->dfMaxX - poGRB->dfMinX) / (nRasterXSize - 1);
    gt[5] = (poGRB->dfMinY - poGRB->dfMaxY) / (nRasterYSize - 1);

    /* then calculate image origin */
    gt[0] = poGRB->dfMinX - gt[1] / 2;
    gt[3] = poGRB->dfMaxY - gt[5] / 2;

    /* tilt/rotation does not supported by the GS grids */
    gt[4] = 0.0;
    gt[2] = 0.0;

    return CE_None;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr GS7BGDataset::SetGeoTransform(const GDALGeoTransform &gt)
{
    if (eAccess == GA_ReadOnly)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Unable to set GeoTransform, dataset opened read only.\n");
        return CE_Failure;
    }

    GS7BGRasterBand *poGRB =
        cpl::down_cast<GS7BGRasterBand *>(GetRasterBand(1));

    /* non-zero transform 2 or 4 or negative 1 or 5 not supported natively */
    /*if( gt[2] != 0.0 || gt[4] != 0.0
    || gt[1] < 0.0 || gt[5] < 0.0 )
    eErr = GDALPamDataset::SetGeoTransform( gt );

    if( eErr != CE_None )
    return eErr;*/

    double dfMinX = gt[0] + gt[1] / 2;
    double dfMaxX = gt[1] * (nRasterXSize - 0.5) + gt[0];
    double dfMinY = gt[5] * (nRasterYSize - 0.5) + gt[3];
    double dfMaxY = gt[3] + gt[5] / 2;

    CPLErr eErr =
        WriteHeader(fp, poGRB->nRasterXSize, poGRB->nRasterYSize, dfMinX,
                    dfMaxX, dfMinY, dfMaxY, poGRB->dfMinZ, poGRB->dfMaxZ);

    if (eErr == CE_None)
    {
        poGRB->dfMinX = dfMinX;
        poGRB->dfMaxX = dfMaxX;
        poGRB->dfMinY = dfMinY;
        poGRB->dfMaxY = dfMaxY;
    }

    return eErr;
}

/************************************************************************/
/*                             WriteHeader()                            */
/************************************************************************/

CPLErr GS7BGDataset::WriteHeader(VSILFILE *fp, GInt32 nXSize, GInt32 nYSize,
                                 double dfMinX, double dfMaxX, double dfMinY,
                                 double dfMaxY, double dfMinZ, double dfMaxZ)

{
    if (VSIFSeekL(fp, 0, SEEK_SET) != 0)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to seek to start of grid file.\n");
        return CE_Failure;
    }

    GInt32 nTemp = CPL_LSBWORD32(nHEADER_TAG);
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write header tag to grid file.\n");
        return CE_Failure;
    }

    nTemp = CPL_LSBWORD32(sizeof(GInt32));  // Size of version section.
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write size to grid file.\n");
        return CE_Failure;
    }

    nTemp = CPL_LSBWORD32(1);  // Version
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write size to grid file.\n");
        return CE_Failure;
    }

    nTemp = CPL_LSBWORD32(nGRID_TAG);  // Mark start of grid
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write size to grid file.\n");
        return CE_Failure;
    }

    nTemp = CPL_LSBWORD32(72);  // Grid info size (the remainder of the header)
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write size to grid file.\n");
        return CE_Failure;
    }

    nTemp = CPL_LSBWORD32(nYSize);
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write Y size to grid file.\n");
        return CE_Failure;
    }

    nTemp = CPL_LSBWORD32(nXSize);
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write X size to grid file.\n");
        return CE_Failure;
    }

    double dfTemp = dfMinX;
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write minimum X value to grid file.\n");
        return CE_Failure;
    }

    dfTemp = dfMinY;
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write minimum Y value to grid file.\n");
        return CE_Failure;
    }

    // Write node spacing in x direction
    dfTemp = (dfMaxX - dfMinX) / (nXSize - 1);
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write spacing in X value.\n");
        return CE_Failure;
    }

    // Write node spacing in y direction
    dfTemp = (dfMaxY - dfMinY) / (nYSize - 1);
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write spacing in Y value.\n");
        return CE_Failure;
    }

    dfTemp = dfMinZ;
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write minimum Z value to grid file.\n");
        return CE_Failure;
    }

    dfTemp = dfMaxZ;
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write maximum Z value to grid file.\n");
        return CE_Failure;
    }

    dfTemp = 0;  // Rotation value is zero
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write rotation value to grid file.\n");
        return CE_Failure;
    }

    dfTemp = dfDefaultNoDataValue;
    CPL_LSBPTR64(&dfTemp);
    if (VSIFWriteL((void *)&dfTemp, sizeof(double), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write cell blank value to grid file.\n");
        return CE_Failure;
    }

    // Only supports 1 band so go ahead and write band info here
    nTemp = CPL_LSBWORD32(nDATA_TAG);  // Mark start of data
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Unable to data tag to grid file.\n");
        return CE_Failure;
    }

    int nSize = nXSize * nYSize * (int)sizeof(double);
    nTemp = CPL_LSBWORD32(nSize);  // Mark size of data
    if (VSIFWriteL((void *)&nTemp, sizeof(GInt32), 1, fp) != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Unable to write data size to grid file.\n");
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                      GS7BGCreateCheckDims()                          */
/************************************************************************/

static bool GS7BGCreateCheckDims(int nXSize, int nYSize)
{
    if (nXSize <= 1 || nYSize <= 1)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "Unable to create grid, both X and Y size must be "
                 "larger or equal to 2.");
        return false;
    }
    if (nXSize > INT_MAX / nYSize / static_cast<int>(sizeof(double)))
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "Unable to create grid, too large X and Y size.");
        return false;
    }
    return true;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

GDALDataset *GS7BGDataset::Create(const char *pszFilename, int nXSize,
                                  int nYSize, int nBandsIn, GDALDataType eType,
                                  char ** /* papszParamList*/)

{
    if (!GS7BGCreateCheckDims(nXSize, nYSize))
    {
        return nullptr;
    }

    if (eType != GDT_Byte && eType != GDT_Float32 && eType != GDT_UInt16 &&
        eType != GDT_Int16 && eType != GDT_Float64)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "GS7BG Grid only supports Byte, Int16, "
            "Uint16, Float32, and Float64 datatypes.  Unable to create with "
            "type %s.\n",
            GDALGetDataTypeName(eType));

        return nullptr;
    }

    if (nBandsIn > 1)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unable to create copy, "
                 "format only supports one raster band.\n");
        return nullptr;
    }

    VSILFILE *fp = VSIFOpenL(pszFilename, "w+b");

    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Attempt to create file '%s' failed.\n", pszFilename);
        return nullptr;
    }

    CPLErr eErr =
        WriteHeader(fp, nXSize, nYSize, 0.0, nXSize, 0.0, nYSize, 0.0, 0.0);
    if (eErr != CE_None)
    {
        VSIFCloseL(fp);
        return nullptr;
    }

    double dfVal = dfDefaultNoDataValue;
    CPL_LSBPTR64(&dfVal);
    for (int iRow = 0; iRow < nYSize; iRow++)
    {
        for (int iCol = 0; iCol < nXSize; iCol++)
        {
            if (VSIFWriteL((void *)&dfVal, sizeof(double), 1, fp) != 1)
            {
                VSIFCloseL(fp);
                CPLError(CE_Failure, CPLE_FileIO,
                         "Unable to write grid cell.  Disk full?\n");
                return nullptr;
            }
        }
    }

    VSIFCloseL(fp);

    return (GDALDataset *)GDALOpen(pszFilename, GA_Update);
}

/************************************************************************/
/*                             CreateCopy()                             */
/************************************************************************/

GDALDataset *GS7BGDataset::CreateCopy(const char *pszFilename,
                                      GDALDataset *poSrcDS, int bStrict,
                                      char ** /*papszOptions*/,
                                      GDALProgressFunc pfnProgress,
                                      void *pProgressData)
{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    int nBands = poSrcDS->GetRasterCount();
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Driver does not support source dataset with zero band.\n");
        return nullptr;
    }
    else if (nBands > 1)
    {
        if (bStrict)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Unable to create copy, "
                     "format only supports one raster band.\n");
            return nullptr;
        }
        else
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Format only supports one "
                     "raster band, first band will be copied.\n");
    }

    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterXSize();
    if (!GS7BGCreateCheckDims(nXSize, nYSize))
    {
        return nullptr;
    }

    GDALRasterBand *poSrcBand = poSrcDS->GetRasterBand(1);

    if (!pfnProgress(0.0, nullptr, pProgressData))
    {
        CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated\n");
        return nullptr;
    }

    VSILFILE *fp = VSIFOpenL(pszFilename, "w+b");

    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Attempt to create file '%s' failed.\n", pszFilename);
        return nullptr;
    }

    GDALGeoTransform gt;
    poSrcDS->GetGeoTransform(gt);

    double dfMinX = gt[0] + gt[1] / 2;
    double dfMaxX = gt[1] * (nXSize - 0.5) + gt[0];
    double dfMinY = gt[5] * (nYSize - 0.5) + gt[3];
    double dfMaxY = gt[3] + gt[5] / 2;
    CPLErr eErr = WriteHeader(fp, nXSize, nYSize, dfMinX, dfMaxX, dfMinY,
                              dfMaxY, 0.0, 0.0);

    if (eErr != CE_None)
    {
        VSIFCloseL(fp);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Copy band data.                                                 */
    /* -------------------------------------------------------------------- */
    double *pfData = (double *)VSI_MALLOC2_VERBOSE(nXSize, sizeof(double));
    if (pfData == nullptr)
    {
        VSIFCloseL(fp);
        return nullptr;
    }

    int bSrcHasNDValue;
    double dfSrcNoDataValue = poSrcBand->GetNoDataValue(&bSrcHasNDValue);
    double dfMinZ = std::numeric_limits<double>::max();
    double dfMaxZ = std::numeric_limits<double>::lowest();
    for (GInt32 iRow = nYSize - 1; iRow >= 0; iRow--)
    {
        eErr = poSrcBand->RasterIO(GF_Read, 0, iRow, nXSize, 1, pfData, nXSize,
                                   1, GDT_Float64, 0, 0, nullptr);

        if (eErr != CE_None)
        {
            VSIFCloseL(fp);
            VSIFree(pfData);
            return nullptr;
        }

        for (int iCol = 0; iCol < nXSize; iCol++)
        {
            if (bSrcHasNDValue && pfData[iCol] == dfSrcNoDataValue)
            {
                pfData[iCol] = dfDefaultNoDataValue;
            }
            else
            {
                if (pfData[iCol] > dfMaxZ)
                    dfMaxZ = pfData[iCol];

                if (pfData[iCol] < dfMinZ)
                    dfMinZ = pfData[iCol];
            }

            CPL_LSBPTR64(pfData + iCol);
        }

        if (VSIFWriteL((void *)pfData, sizeof(double), nXSize, fp) !=
            static_cast<unsigned>(nXSize))
        {
            VSIFCloseL(fp);
            VSIFree(pfData);
            CPLError(CE_Failure, CPLE_FileIO,
                     "Unable to write grid row. Disk full?\n");
            return nullptr;
        }

        if (!pfnProgress(static_cast<double>(nYSize - iRow) / nYSize, nullptr,
                         pProgressData))
        {
            VSIFCloseL(fp);
            VSIFree(pfData);
            CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
            return nullptr;
        }
    }

    VSIFree(pfData);

    /* write out the min and max values */
    eErr = WriteHeader(fp, nXSize, nYSize, dfMinX, dfMaxX, dfMinY, dfMaxY,
                       dfMinZ, dfMaxZ);

    if (eErr != CE_None)
    {
        VSIFCloseL(fp);
        return nullptr;
    }

    VSIFCloseL(fp);

    GDALPamDataset *poDS = (GDALPamDataset *)GDALOpen(pszFilename, GA_Update);
    if (poDS)
    {
        poDS->CloneInfo(poSrcDS, GCIF_PAM_DEFAULT);
    }

    return poDS;
}

/************************************************************************/
/*                          GDALRegister_GS7BG()                        */
/************************************************************************/
void GDALRegister_GS7BG()

{
    if (GDALGetDriverByName("GS7BG") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("GS7BG");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "Golden Software 7 Binary Grid (.grd)");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/gs7bg.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "grd");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES,
                              "Byte Int16 UInt16 Float32 Float64");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnIdentify = GS7BGDataset::Identify;
    poDriver->pfnOpen = GS7BGDataset::Open;
    poDriver->pfnCreate = GS7BGDataset::Create;
    poDriver->pfnCreateCopy = GS7BGDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
