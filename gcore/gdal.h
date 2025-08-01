/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  GDAL Core C/Public declarations.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1998, 2002 Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef GDAL_H_INCLUDED
#define GDAL_H_INCLUDED

/**
 * \file gdal.h
 *
 * Public (C callable) GDAL entry points.
 */

#ifndef DOXYGEN_SKIP
#if defined(GDAL_COMPILATION)
#define DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "gdal_fwd.h"
#include "gdal_version.h"
#include "cpl_port.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_virtualmem.h"
#include "cpl_minixml.h"
#include "ogr_api.h"
#endif

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------- */
/*      Significant constants.                                          */
/* -------------------------------------------------------------------- */

CPL_C_START

/*! Pixel data types */
typedef enum
{
    /*! Unknown or unspecified type */ GDT_Unknown = 0,
    /*! Eight bit unsigned integer */ GDT_Byte = 1,
    /*! 8-bit signed integer (GDAL >= 3.7) */ GDT_Int8 = 14,
    /*! Sixteen bit unsigned integer */ GDT_UInt16 = 2,
    /*! Sixteen bit signed integer */ GDT_Int16 = 3,
    /*! Thirty two bit unsigned integer */ GDT_UInt32 = 4,
    /*! Thirty two bit signed integer */ GDT_Int32 = 5,
    /*! 64 bit unsigned integer (GDAL >= 3.5)*/ GDT_UInt64 = 12,
    /*! 64 bit signed integer  (GDAL >= 3.5)*/ GDT_Int64 = 13,
    /*! Sixteen bit floating point */ GDT_Float16 = 15,
    /*! Thirty two bit floating point */ GDT_Float32 = 6,
    /*! Sixty four bit floating point */ GDT_Float64 = 7,
    /*! Complex Int16 */ GDT_CInt16 = 8,
    /*! Complex Int32 */ GDT_CInt32 = 9,
    /* TODO?(#6879): GDT_CInt64 */
    /*! Complex Float16 */ GDT_CFloat16 = 16,
    /*! Complex Float32 */ GDT_CFloat32 = 10,
    /*! Complex Float64 */ GDT_CFloat64 = 11,
    GDT_TypeCount = 17 /* maximum type # + 1 */
} GDALDataType;

int CPL_DLL CPL_STDCALL GDALGetDataTypeSize(GDALDataType)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use GDALGetDataTypeSizeBits() or "
                        "GDALGetDataTypeSizeBytes() * 8 instead")
    /*! @endcond */
    ;
int CPL_DLL CPL_STDCALL GDALGetDataTypeSizeBits(GDALDataType eDataType);
int CPL_DLL CPL_STDCALL GDALGetDataTypeSizeBytes(GDALDataType);
int CPL_DLL CPL_STDCALL GDALDataTypeIsComplex(GDALDataType);
int CPL_DLL CPL_STDCALL GDALDataTypeIsInteger(GDALDataType);
int CPL_DLL CPL_STDCALL GDALDataTypeIsFloating(GDALDataType);
int CPL_DLL CPL_STDCALL GDALDataTypeIsSigned(GDALDataType);
const char CPL_DLL *CPL_STDCALL GDALGetDataTypeName(GDALDataType);
GDALDataType CPL_DLL CPL_STDCALL GDALGetDataTypeByName(const char *);
GDALDataType CPL_DLL CPL_STDCALL GDALDataTypeUnion(GDALDataType, GDALDataType);
GDALDataType CPL_DLL CPL_STDCALL GDALDataTypeUnionWithValue(GDALDataType eDT,
                                                            double dValue,
                                                            int bComplex);
GDALDataType CPL_DLL CPL_STDCALL GDALFindDataType(int nBits, int bSigned,
                                                  int bFloating, int bComplex);
GDALDataType CPL_DLL CPL_STDCALL GDALFindDataTypeForValue(double dValue,
                                                          int bComplex);
double CPL_DLL GDALAdjustValueToDataType(GDALDataType eDT, double dfValue,
                                         int *pbClamped, int *pbRounded);
bool CPL_DLL GDALIsValueExactAs(double dfValue, GDALDataType eDT);
bool CPL_DLL GDALIsValueInRangeOf(double dfValue, GDALDataType eDT);
GDALDataType CPL_DLL CPL_STDCALL GDALGetNonComplexDataType(GDALDataType);
int CPL_DLL CPL_STDCALL GDALDataTypeIsConversionLossy(GDALDataType eTypeFrom,
                                                      GDALDataType eTypeTo);

/**
 * status of the asynchronous stream
 */
typedef enum
{
    GARIO_PENDING = 0,
    GARIO_UPDATE = 1,
    GARIO_ERROR = 2,
    GARIO_COMPLETE = 3,
    GARIO_TypeCount = 4
} GDALAsyncStatusType;

const char CPL_DLL *CPL_STDCALL GDALGetAsyncStatusTypeName(GDALAsyncStatusType);
GDALAsyncStatusType CPL_DLL CPL_STDCALL
GDALGetAsyncStatusTypeByName(const char *);

/*! Flag indicating read/write, or read-only access to data. */
typedef enum
{
    /*! Read only (no update) access */ GA_ReadOnly = 0,
    /*! Read/write access. */ GA_Update = 1
} GDALAccess;

/*! Read/Write flag for RasterIO() method */
typedef enum
{
    /*! Read data */ GF_Read = 0,
    /*! Write data */ GF_Write = 1
} GDALRWFlag;

/* NOTE: values are selected to be consistent with GDALResampleAlg of
 * alg/gdalwarper.h */
/** RasterIO() resampling method.
 * @since GDAL 2.0
 */
typedef enum
{
    /*! Nearest neighbour */ GRIORA_NearestNeighbour = 0,
    /*! Bilinear (2x2 kernel) */ GRIORA_Bilinear = 1,
    /*! Cubic Convolution Approximation (4x4 kernel) */ GRIORA_Cubic = 2,
    /*! Cubic B-Spline Approximation (4x4 kernel) */ GRIORA_CubicSpline = 3,
    /*! Lanczos windowed sinc interpolation (6x6 kernel) */ GRIORA_Lanczos = 4,
    /*! Average */ GRIORA_Average = 5,
    /*! Mode (selects the value which appears most often of all the sampled
       points) */
    GRIORA_Mode = 6,
    /*! Gauss blurring */ GRIORA_Gauss = 7,
    /* NOTE: values 8 to 13 are reserved for max,min,med,Q1,Q3,sum */
    /*! @cond Doxygen_Suppress */
    GRIORA_RESERVED_START = 8,
    GRIORA_RESERVED_END = 13,
    /*! @endcond */
    /** RMS: Root Mean Square / Quadratic Mean.
     * For complex numbers, applies on the real and imaginary part
     * independently.
     */
    GRIORA_RMS = 14,
    /*! @cond Doxygen_Suppress */
    GRIORA_LAST = GRIORA_RMS
    /*! @endcond */
} GDALRIOResampleAlg;

/* NOTE to developers: if required, only add members at the end of the
 * structure, and when doing so increase RASTERIO_EXTRA_ARG_CURRENT_VERSION
 */
/** Structure to pass extra arguments to RasterIO() method,
 * must be initialized with INIT_RASTERIO_EXTRA_ARG
 * @since GDAL 2.0
 */
typedef struct
{
    /*! Version of structure (to allow future extensions of the structure) */
    int nVersion;

    /*! Resampling algorithm */
    GDALRIOResampleAlg eResampleAlg;

    /*! Progress callback */
    GDALProgressFunc pfnProgress;
    /*! Progress callback user data */
    void *pProgressData;

    /*! Indicate if dfXOff, dfYOff, dfXSize and dfYSize are set.
        Mostly reserved from the VRT driver to communicate a more precise
        source window. Must be such that dfXOff - nXOff < 1.0 and
        dfYOff - nYOff < 1.0 and nXSize - dfXSize < 1.0 and nYSize - dfYSize
       < 1.0 */
    int bFloatingPointWindowValidity;
    /*! Pixel offset to the top left corner. Only valid if
     * bFloatingPointWindowValidity = TRUE */
    double dfXOff;
    /*! Line offset to the top left corner. Only valid if
     * bFloatingPointWindowValidity = TRUE */
    double dfYOff;
    /*! Width in pixels of the area of interest. Only valid if
     * bFloatingPointWindowValidity = TRUE */
    double dfXSize;
    /*! Height in pixels of the area of interest. Only valid if
     * bFloatingPointWindowValidity = TRUE */
    double dfYSize;
    /*! Indicate if overviews should be considered. Tested in
        GDALBandGetBestOverviewLevel(), mostly reserved for use by
        GDALRegenerateOverviewsMultiBand()
        Only available if RASTERIO_EXTRA_ARG_CURRENT_VERSION >= 2
    */
    int bUseOnlyThisScale;
} GDALRasterIOExtraArg;

#ifndef DOXYGEN_SKIP
#define RASTERIO_EXTRA_ARG_CURRENT_VERSION 2
#endif

/** Macro to initialize an instance of GDALRasterIOExtraArg structure.
 * @since GDAL 2.0
 */
#define INIT_RASTERIO_EXTRA_ARG(s)                                             \
    do                                                                         \
    {                                                                          \
        (s).nVersion = RASTERIO_EXTRA_ARG_CURRENT_VERSION;                     \
        (s).eResampleAlg = GRIORA_NearestNeighbour;                            \
        (s).pfnProgress = CPL_NULLPTR;                                         \
        (s).pProgressData = CPL_NULLPTR;                                       \
        (s).bFloatingPointWindowValidity = FALSE;                              \
        (s).bUseOnlyThisScale = FALSE;                                         \
    } while (0)

/** Value indicating the start of the range for color interpretations belonging
 * to the InfraRed (IR) domain. All constants of the GDALColorInterp enumeration
 * in the IR domain are in the [GCI_IR_Start, GCI_IR_End] range.
 *
 * @since 3.10
 */
#define GCI_IR_Start 20

/** Value indicating the end of the range for color interpretations belonging
 * to the InfraRed (IR) domain. All constants of the GDALColorInterp enumeration
 * in the IR domain are in the [GCI_IR_Start, GCI_IR_End] range.
 *
 * @since 3.10
 */
#define GCI_IR_End 29

/** Value indicating the start of the range for color interpretations belonging
 * to the Synthetic Aperture Radar (SAR) domain.
 * All constants of the GDALColorInterp enumeration
 * in the SAR domain are in the [GCI_SAR_Start, GCI_SAR_End] range.
 *
 * @since 3.10
 */
#define GCI_SAR_Start 30

/** Value indicating the end of the range for color interpretations belonging
 * to the Synthetic Aperture Radar (SAR) domain.
 * All constants of the GDALColorInterp enumeration
 * in the SAR domain are in the [GCI_SAR_Start, GCI_SAR_End] range.
 *
 * @since 3.10
 */
#define GCI_SAR_End 39

/** Types of color interpretation for raster bands.
 *
 * For spectral bands, the wavelength ranges are indicative only, and may vary
 * depending on sensors. The CENTRAL_WAVELENGTH_UM and FWHM_UM metadata
 * items in the IMAGERY metadata domain of the raster band, when present, will
 * give more accurate characteristics.
 *
 * Values belonging to the IR domain are in the [GCI_IR_Start, GCI_IR_End] range.
 * Values belonging to the SAR domain are in the [GCI_SAR_Start, GCI_SAR_End] range.
 *
 * Values between GCI_PanBand to GCI_SAR_Reserved_2 have been added in GDAL 3.10.
 */
typedef enum
{
    /*! Undefined */ GCI_Undefined = 0,
    /*! Greyscale */ GCI_GrayIndex = 1,
    /*! Paletted (see associated color table) */ GCI_PaletteIndex = 2,
    /*! Red band of RGBA image, or red spectral band [0.62 - 0.69 um]*/
    GCI_RedBand = 3,
    /*! Green band of RGBA image, or green spectral band [0.51 - 0.60 um]*/
    GCI_GreenBand = 4,
    /*! Blue band of RGBA image, or blue spectral band [0.45 - 0.53 um] */
    GCI_BlueBand = 5,
    /*! Alpha (0=transparent, 255=opaque) */ GCI_AlphaBand = 6,
    /*! Hue band of HLS image */ GCI_HueBand = 7,
    /*! Saturation band of HLS image */ GCI_SaturationBand = 8,
    /*! Lightness band of HLS image */ GCI_LightnessBand = 9,
    /*! Cyan band of CMYK image */ GCI_CyanBand = 10,
    /*! Magenta band of CMYK image */ GCI_MagentaBand = 11,
    /*! Yellow band of CMYK image, or yellow spectral band [0.58 - 0.62 um] */
    GCI_YellowBand = 12,
    /*! Black band of CMYK image */ GCI_BlackBand = 13,
    /*! Y Luminance */ GCI_YCbCr_YBand = 14,
    /*! Cb Chroma */ GCI_YCbCr_CbBand = 15,
    /*! Cr Chroma */ GCI_YCbCr_CrBand = 16,

    /* GDAL 3.10 addition: begin */
    /*! Panchromatic band [0.40 - 1.00 um] */ GCI_PanBand = 17,
    /*! Coastal band [0.40 - 0.45 um] */ GCI_CoastalBand = 18,
    /*! Red-edge band [0.69 - 0.79 um] */ GCI_RedEdgeBand = 19,

    /*! Near-InfraRed (NIR) band [0.75 - 1.40 um] */ GCI_NIRBand =
        GCI_IR_Start + 0,
    /*! Short-Wavelength InfraRed (SWIR) band [1.40 - 3.00 um] */ GCI_SWIRBand =
        GCI_IR_Start + 1,
    /*! Mid-Wavelength InfraRed (MWIR) band [3.00 - 8.00 um] */ GCI_MWIRBand =
        GCI_IR_Start + 2,
    /*! Long-Wavelength InfraRed (LWIR) band [8.00 - 15 um] */ GCI_LWIRBand =
        GCI_IR_Start + 3,
    /*! Thermal InfraRed (TIR) band (MWIR or LWIR) [3 - 15 um] */ GCI_TIRBand =
        GCI_IR_Start + 4,
    /*! Other infrared band [0.75 - 1000 um] */ GCI_OtherIRBand =
        GCI_IR_Start + 5,
    /*! Reserved value. Do not set it ! */
    GCI_IR_Reserved_1 = GCI_IR_Start + 6,
    /*! Reserved value. Do not set it ! */
    GCI_IR_Reserved_2 = GCI_IR_Start + 7,
    /*! Reserved value. Do not set it ! */
    GCI_IR_Reserved_3 = GCI_IR_Start + 8,
    /*! Reserved value. Do not set it ! */
    GCI_IR_Reserved_4 = GCI_IR_Start + 9,

    /*! Synthetic Aperture Radar (SAR) Ka band [0.8 - 1.1 cm / 27 - 40 GHz] */
    GCI_SAR_Ka_Band = GCI_SAR_Start + 0,
    /*! Synthetic Aperture Radar (SAR) K band [1.1 - 1.7 cm / 18 - 27 GHz] */
    GCI_SAR_K_Band = GCI_SAR_Start + 1,
    /*! Synthetic Aperture Radar (SAR) Ku band [1.7 - 2.4 cm / 12 - 18 GHz] */
    GCI_SAR_Ku_Band = GCI_SAR_Start + 2,
    /*! Synthetic Aperture Radar (SAR) X band [2.4 - 3.8 cm / 8 - 12 GHz] */
    GCI_SAR_X_Band = GCI_SAR_Start + 3,
    /*! Synthetic Aperture Radar (SAR) C band [3.8 - 7.5 cm / 4 - 8 GHz] */
    GCI_SAR_C_Band = GCI_SAR_Start + 4,
    /*! Synthetic Aperture Radar (SAR) S band [7.5 - 15 cm / 2 - 4 GHz] */
    GCI_SAR_S_Band = GCI_SAR_Start + 5,
    /*! Synthetic Aperture Radar (SAR) L band [15 - 30 cm / 1 - 2 GHz] */
    GCI_SAR_L_Band = GCI_SAR_Start + 6,
    /*! Synthetic Aperture Radar (SAR) P band [30 - 100 cm / 0.3 - 1 GHz] */
    GCI_SAR_P_Band = GCI_SAR_Start + 7,
    /*! Reserved value. Do not set it ! */
    GCI_SAR_Reserved_1 = GCI_SAR_Start + 8,
    /*! Reserved value. Do not set it ! */
    GCI_SAR_Reserved_2 = GCI_SAR_Start + 9,

    /* GDAL 3.10 addition: end */

    /*! Max current value (equals to GCI_SAR_Reserved_2 currently) */ GCI_Max =
        GCI_SAR_Reserved_2
} GDALColorInterp;

const char CPL_DLL *GDALGetColorInterpretationName(GDALColorInterp);
GDALColorInterp CPL_DLL GDALGetColorInterpretationByName(const char *pszName);

/*! Types of color interpretations for a GDALColorTable. */
typedef enum
{
    /*! Grayscale (in GDALColorEntry.c1) */ GPI_Gray = 0,
    /*! Red, Green, Blue and Alpha in (in c1, c2, c3 and c4) */ GPI_RGB = 1,
    /*! Cyan, Magenta, Yellow and Black (in c1, c2, c3 and c4)*/ GPI_CMYK = 2,
    /*! Hue, Lightness and Saturation (in c1, c2, and c3) */ GPI_HLS = 3
} GDALPaletteInterp;

const char CPL_DLL *GDALGetPaletteInterpretationName(GDALPaletteInterp);

/* "well known" metadata items. */

/** Metadata item for dataset that indicates the spatial interpretation of a
 *  pixel */
#define GDALMD_AREA_OR_POINT "AREA_OR_POINT"
/** Value for GDALMD_AREA_OR_POINT that indicates that a pixel represents an
 * area */
#define GDALMD_AOP_AREA "Area"
/** Value for GDALMD_AREA_OR_POINT that indicates that a pixel represents a
 * point */
#define GDALMD_AOP_POINT "Point"

/* -------------------------------------------------------------------- */
/*      GDAL Specific error codes.                                      */
/*                                                                      */
/*      error codes 100 to 299 reserved for GDAL.                       */
/* -------------------------------------------------------------------- */
#ifndef DOXYGEN_SKIP
#define CPLE_WrongFormat CPL_STATIC_CAST(CPLErrorNum, 200)
#endif

/* -------------------------------------------------------------------- */
/*      Types, enumerations.                                            */
/* -------------------------------------------------------------------- */

/** Type to express pixel, line or band spacing. Signed 64 bit integer. */
typedef GIntBig GSpacing;

/** Enumeration giving the class of a GDALExtendedDataType.
 * @since GDAL 3.1
 */
typedef enum
{
    /** Numeric value. Based on GDALDataType enumeration */
    GEDTC_NUMERIC,
    /** String value. */
    GEDTC_STRING,
    /** Compound data type. */
    GEDTC_COMPOUND
} GDALExtendedDataTypeClass;

/** Enumeration giving the subtype of a GDALExtendedDataType.
 * @since GDAL 3.4
 */
typedef enum
{
    /** None. */
    GEDTST_NONE,
    /** JSon. Only applies to GEDTC_STRING */
    GEDTST_JSON
} GDALExtendedDataTypeSubType;

/* ==================================================================== */
/*      Registration/driver related.                                    */
/* ==================================================================== */

/** Long name of the driver */
#define GDAL_DMD_LONGNAME "DMD_LONGNAME"

/** URL (relative to http://gdal.org/) to the help page of the driver */
#define GDAL_DMD_HELPTOPIC "DMD_HELPTOPIC"

/** MIME type handled by the driver. */
#define GDAL_DMD_MIMETYPE "DMD_MIMETYPE"

/** Extension handled by the driver. */
#define GDAL_DMD_EXTENSION "DMD_EXTENSION"

/** Connection prefix to provide as the file name of the open function.
 * Typically set for non-file based drivers. Generally used with open options.
 * @since GDAL 2.0
 */
#define GDAL_DMD_CONNECTION_PREFIX "DMD_CONNECTION_PREFIX"

/** List of (space separated) extensions handled by the driver.
 * @since GDAL 2.0
 */
#define GDAL_DMD_EXTENSIONS "DMD_EXTENSIONS"

/** XML snippet with creation options. */
#define GDAL_DMD_CREATIONOPTIONLIST "DMD_CREATIONOPTIONLIST"

/** XML snippet with multidimensional dataset creation options.
 * @since GDAL 3.1
 */
#define GDAL_DMD_MULTIDIM_DATASET_CREATIONOPTIONLIST                           \
    "DMD_MULTIDIM_DATASET_CREATIONOPTIONLIST"

/** XML snippet with multidimensional group creation options.
 * @since GDAL 3.1
 */
#define GDAL_DMD_MULTIDIM_GROUP_CREATIONOPTIONLIST                             \
    "DMD_MULTIDIM_GROUP_CREATIONOPTIONLIST"

/** XML snippet with multidimensional dimension creation options.
 * @since GDAL 3.1
 */
#define GDAL_DMD_MULTIDIM_DIMENSION_CREATIONOPTIONLIST                         \
    "DMD_MULTIDIM_DIMENSION_CREATIONOPTIONLIST"

/** XML snippet with multidimensional array creation options.
 * @since GDAL 3.1
 */
#define GDAL_DMD_MULTIDIM_ARRAY_CREATIONOPTIONLIST                             \
    "DMD_MULTIDIM_ARRAY_CREATIONOPTIONLIST"

/** XML snippet with multidimensional array open options.
 * @since GDAL 3.6
 */
#define GDAL_DMD_MULTIDIM_ARRAY_OPENOPTIONLIST                                 \
    "DMD_MULTIDIM_ARRAY_OPENOPTIONLIST"

/** XML snippet with multidimensional attribute creation options.
 * @since GDAL 3.1
 */
#define GDAL_DMD_MULTIDIM_ATTRIBUTE_CREATIONOPTIONLIST                         \
    "DMD_MULTIDIM_ATTRIBUTE_CREATIONOPTIONLIST"

/** XML snippet with open options.
 * @since GDAL 2.0
 */
#define GDAL_DMD_OPENOPTIONLIST "DMD_OPENOPTIONLIST"

/** List of (space separated) raster data types supported by the
 * Create()/CreateCopy() API. */
#define GDAL_DMD_CREATIONDATATYPES "DMD_CREATIONDATATYPES"

/** List of (space separated) vector field types supported by the CreateField()
 * API.
 * @since GDAL 2.0
 * */
#define GDAL_DMD_CREATIONFIELDDATATYPES "DMD_CREATIONFIELDDATATYPES"

/** List of (space separated) vector field sub-types supported by the
 * CreateField() API.
 * @since GDAL 2.3
 * */
#define GDAL_DMD_CREATIONFIELDDATASUBTYPES "DMD_CREATIONFIELDDATASUBTYPES"

/** Maximum size of a String field that can be created (OGRFieldDefn.GetWidth()).
 *
 * It is undefined whether this is a number of bytes or Unicode character count.
 * Most of the time, this will be a number of bytes, so a Unicode string whose
 * character count is the maximum size could not fit.
 *
 * This metadata item is set only on a small number of drivers, in particular
 * "ESRI Shapefile" and "MapInfo File", which use fixed-width storage of strings.
 *
 * @since GDAL 3.12
 */
#define GDAL_DMD_MAX_STRING_LENGTH "DMD_MAX_STRING_LENGTH"

/** List of (space separated) capability flags supported by the CreateField() API.
 *
 * Supported values are:
 *
 * - "WidthPrecision": field width and precision is supported.
 * - "Nullable": field (non-)nullable status is supported.
 * - "Unique": field unique constraint is supported.
 * - "Default": field default value is supported.
 * - "AlternativeName": field alternative name is supported.
 * - "Comment": field comment is supported.
 * - "Domain": field can be associated with a domain.
 *
 * @see GDAL_DMD_ALTER_FIELD_DEFN_FLAGS for capabilities supported when altering
 * existing fields.
 *
 * @since GDAL 3.7
 */
#define GDAL_DMD_CREATION_FIELD_DEFN_FLAGS "DMD_CREATION_FIELD_DEFN_FLAGS"

/** Capability set by a driver that exposes Subdatasets.
 *
 * This capability reflects that a raster driver supports child layers, such as
 * NetCDF or multi-table raster Geopackages.
 *
 * See GDAL_DCAP_MULTIPLE_VECTOR_LAYERS for a similar capability flag
 * for vector drivers.
 */
#define GDAL_DMD_SUBDATASETS "DMD_SUBDATASETS"

/** Capability set by a driver that can create subdatasets with the
 * APPEND_SUBDATASET=YES creation option.
 *
 * @since 3.11
 */
#define GDAL_DCAP_CREATE_SUBDATASETS "DCAP_CREATE_SUBDATASETS"

/** Capability set by a vector driver that supports field width and precision.
 *
 * This capability reflects that a vector driver includes the decimal separator
 * in the field width of fields of type OFTReal.
 *
 * See GDAL_DMD_NUMERIC_FIELD_WIDTH_INCLUDES_SIGN for a related capability flag.
 * @since GDAL 3.7
 */
#define GDAL_DMD_NUMERIC_FIELD_WIDTH_INCLUDES_DECIMAL_SEPARATOR                \
    "DMD_NUMERIC_FIELD_WIDTH_INCLUDES_DECIMAL_SEPARATOR"

/** Capability set by a vector driver that supports field width and precision.
 *
 * This capability reflects that a vector driver includes the sign
 * in the field width of fields of type OFTReal.
 *
 * See GDAL_DMD_NUMERIC_FIELD_WIDTH_INCLUDES_DECIMAL_SEPARATOR for a related capability flag.
 * @since GDAL 3.7
 */
#define GDAL_DMD_NUMERIC_FIELD_WIDTH_INCLUDES_SIGN                             \
    "DMD_NUMERIC_FIELD_WIDTH_INCLUDES_SIGN"

/** Capability set by a driver that implements the Open() API. */
#define GDAL_DCAP_OPEN "DCAP_OPEN"

/** Capability set by a driver that implements the Create() API.
 *
 * If GDAL_DCAP_CREATE is set, but GDAL_DCAP_CREATECOPY not, a generic
 * CreateCopy() implementation is available and will use the Create() API of
 * the driver.
 * So to test if some CreateCopy() implementation is available, generic or
 * specialize, test for both GDAL_DCAP_CREATE and GDAL_DCAP_CREATECOPY.
 */
#define GDAL_DCAP_CREATE "DCAP_CREATE"

/** Capability set by a driver that implements the CreateMultiDimensional() API.
 *
 * @since GDAL 3.1
 */
#define GDAL_DCAP_CREATE_MULTIDIMENSIONAL "DCAP_CREATE_MULTIDIMENSIONAL"

/** Capability set by a driver that implements the CreateCopy() API.
 *
 * If GDAL_DCAP_CREATECOPY is not defined, but GDAL_DCAP_CREATE is set, a
 * generic CreateCopy() implementation is available and will use the Create()
 * API of the driver. So to test if some CreateCopy() implementation is
 * available, generic or specialize, test for both GDAL_DCAP_CREATE and
 * GDAL_DCAP_CREATECOPY.
 */
#define GDAL_DCAP_CREATECOPY "DCAP_CREATECOPY"

/** Capability set by a driver that supports the \@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME
 * hidden creation option.
 *
 * @since GDAL 3.12
 */
#define GDAL_DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME                            \
    "DCAP_CREATE_ONLY_VISIBLE_AT_CLOSE_TIME"

/** Capability set by a driver that implements the VectorTranslateFrom() API.
 *
 * @since GDAL 3.8
 */
#define GDAL_DCAP_VECTOR_TRANSLATE_FROM "DCAP_VECTOR_TRANSLATE_FROM"

/** Capability set by a driver that implements the CreateCopy() API, but with
 * multidimensional raster as input and output.
 *
 * @since GDAL 3.1
 */
#define GDAL_DCAP_CREATECOPY_MULTIDIMENSIONAL "DCAP_CREATECOPY_MULTIDIMENSIONAL"

/** Capability set by a driver that supports multidimensional data.
 * @since GDAL 3.1
 */
#define GDAL_DCAP_MULTIDIM_RASTER "DCAP_MULTIDIM_RASTER"

/** Capability set by a driver that can copy over subdatasets. */
#define GDAL_DCAP_SUBCREATECOPY "DCAP_SUBCREATECOPY"

/** Capability set by a driver that supports the GDAL_OF_UPDATE flag and offers
 * at least some update capabilities.
 * Exact update capabilities can be determined by the GDAL_DMD_UPDATE_ITEMS
 * metadata item
 * @since GDAL 3.11
 */
#define GDAL_DCAP_UPDATE "DCAP_UPDATE"

/** Capability set by a driver that can read/create datasets through the VSI*L
 * API. */
#define GDAL_DCAP_VIRTUALIO "DCAP_VIRTUALIO"

/** Capability set by a driver having raster capability.
 * @since GDAL 2.0
 */
#define GDAL_DCAP_RASTER "DCAP_RASTER"

/** Capability set by a driver having vector capability.
 * @since GDAL 2.0
 */
#define GDAL_DCAP_VECTOR "DCAP_VECTOR"

/** Capability set by a driver having geographical network model capability.
 * @since GDAL 2.1
 */
#define GDAL_DCAP_GNM "DCAP_GNM"

/** Capability set by a driver that can create layers.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_CREATE_LAYER "DCAP_CREATE_LAYER"

/** Capability set by a driver that can delete layers.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_DELETE_LAYER "DCAP_DELETE_LAYER"

/** Capability set by a driver that can create fields.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_CREATE_FIELD "DCAP_CREATE_FIELD"

/** Capability set by a driver that can delete fields.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_DELETE_FIELD "DCAP_DELETE_FIELD"

/** Capability set by a driver that can reorder fields.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_REORDER_FIELDS "DCAP_REORDER_FIELDS"

/** List of (space separated) flags supported by the OGRLayer::AlterFieldDefn()
 * API.
 *
 * Supported values are "Name", "Type", "WidthPrecision", "Nullable", "Default",
 * "Unique", "Domain", "AlternativeName" and "Comment", corresponding respectively
 * to the ALTER_NAME_FLAG, ALTER_TYPE_FLAG, ALTER_WIDTH_PRECISION_FLAG, ALTER_NULLABLE_FLAG,
 * ALTER_DEFAULT_FLAG, ALTER_UNIQUE_FLAG, ALTER_DOMAIN_FLAG,
 * ALTER_ALTERNATIVE_NAME_FLAG and ALTER_COMMENT_FLAG flags.
 *
 * Note that advertizing one of these flags doesn't necessarily mean that
 * all modifications of the corresponding property can be made. For example,
 * altering the field type may be restricted by the current type of the field,
 * etc.
 *
 * @see GDAL_DMD_CREATION_FIELD_DEFN_FLAGS for capabilities supported
 * when creating new fields.
 *
 * @since GDAL 3.6
 */
#define GDAL_DMD_ALTER_FIELD_DEFN_FLAGS "GDAL_DMD_ALTER_FIELD_DEFN_FLAGS"

/** List of (space separated) field names which are considered illegal by the
 * driver and should not be used when creating/altering fields.
 *
 * @since GDAL 3.7
 */
#define GDAL_DMD_ILLEGAL_FIELD_NAMES "GDAL_DMD_ILLEGAL_FIELD_NAMES"

/** Capability set by a driver that can create fields with NOT NULL constraint.
 * @since GDAL 2.0
 */
#define GDAL_DCAP_NOTNULL_FIELDS "DCAP_NOTNULL_FIELDS"

/** Capability set by a driver that can create fields with UNIQUE constraint.
 * @since GDAL 3.2
 */
#define GDAL_DCAP_UNIQUE_FIELDS "DCAP_UNIQUE_FIELDS"

/** Capability set by a driver that can create fields with DEFAULT values.
 * @since GDAL 2.0
 */
#define GDAL_DCAP_DEFAULT_FIELDS "DCAP_DEFAULT_FIELDS"

/** Capability set by a driver that can create geometry fields with NOT NULL
 * constraint.
 * @since GDAL 2.0
 */
#define GDAL_DCAP_NOTNULL_GEOMFIELDS "DCAP_NOTNULL_GEOMFIELDS"

/** Capability set by a non-spatial driver having no support for geometries.
 * E.g. non-spatial vector drivers (e.g. spreadsheet format drivers) do not
 * support geometries, and accordingly will have this capability present.
 * @since GDAL 2.3
 */
#define GDAL_DCAP_NONSPATIAL "DCAP_NONSPATIAL"

/** Capability set by a driver that can support curved geometries.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_CURVE_GEOMETRIES "DCAP_CURVE_GEOMETRIES"

/** Capability set by a driver that can support measured geometries.
 *
 * @since GDAL 3.6
 */
#define GDAL_DCAP_MEASURED_GEOMETRIES "DCAP_MEASURED_GEOMETRIES"

/** Capability set by a driver that can support the Z dimension for geometries.
 *
 * @since GDAL 3.6
 */
#define GDAL_DCAP_Z_GEOMETRIES "DCAP_Z_GEOMETRIES"

/** List of (space separated) flags which reflect the geometry handling behavior
 * of a driver.
 *
 * Supported values are currently:
 *
 * - "EquatesMultiAndSingleLineStringDuringWrite" and
 * "EquatesMultiAndSinglePolygonDuringWrite". These flags indicate that the
 * driver does not differentiate between single-part and multi-part linestring
 * and polygon geometries when writing features respectively.
 *
 * @since GDAL 3.6
 */
#define GDAL_DMD_GEOMETRY_FLAGS "GDAL_DMD_GEOMETRY_FLAGS"

/** Capability set by drivers which support either reading or writing feature
 * styles.
 *
 * Consider using the more granular GDAL_DCAP_FEATURE_STYLES_READ or
 * GDAL_DCAP_FEATURE_STYLES_WRITE capabilities instead.
 *
 * @since GDAL 2.3
 */
#define GDAL_DCAP_FEATURE_STYLES "DCAP_FEATURE_STYLES"

/** Capability set by drivers which support reading feature styles.
 * @since GDAL 3.7
 */
#define GDAL_DCAP_FEATURE_STYLES_READ "DCAP_FEATURE_STYLES_READ"

/** Capability set by drivers which support writing feature styles.
 * @since GDAL 3.7
 */
#define GDAL_DCAP_FEATURE_STYLES_WRITE "DCAP_FEATURE_STYLES_WRITE"

/** Capability set by drivers which support storing/retrieving coordinate epoch
 * for dynamic CRS
 * @since GDAL 3.4
 */
#define GDAL_DCAP_COORDINATE_EPOCH "DCAP_COORDINATE_EPOCH"

/** Capability set by drivers for formats which support multiple vector layers.
 *
 * Note: some GDAL drivers expose "virtual" layer support while the underlying
 * formats themselves do not. This capability is only set for drivers of formats
 * which have a native concept of multiple vector layers (such as GeoPackage).
 *
 * @since GDAL 3.4
 */
#define GDAL_DCAP_MULTIPLE_VECTOR_LAYERS "DCAP_MULTIPLE_VECTOR_LAYERS"

/** Capability set by drivers for formats which support reading field domains.
 *
 * @since GDAL 3.5
 */
#define GDAL_DCAP_FIELD_DOMAINS "DCAP_FIELD_DOMAINS"

/** Capability set by drivers for formats which support reading table
 * relationships.
 *
 * @since GDAL 3.6
 */
#define GDAL_DCAP_RELATIONSHIPS "DCAP_RELATIONSHIPS"

/** Capability set by drivers for formats which support creating table
 * relationships.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_CREATE_RELATIONSHIP "DCAP_CREATE_RELATIONSHIP"

/** Capability set by drivers for formats which support deleting table
 * relationships.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_DELETE_RELATIONSHIP "DCAP_DELETE_RELATIONSHIP"

/** Capability set by drivers for formats which support updating existing table
 * relationships.
 * @since GDAL 3.6
 */
#define GDAL_DCAP_UPDATE_RELATIONSHIP "DCAP_UPDATE_RELATIONSHIP"

/** Capability set by drivers whose FlushCache() implementation returns a
 * dataset that can be opened afterwards and seen in a consistent state, without
 * requiring the dataset on which FlushCache() has been called to be closed.
 * @since GDAL 3.8
 */
#define GDAL_DCAP_FLUSHCACHE_CONSISTENT_STATE "DCAP_FLUSHCACHE_CONSISTENT_STATE"

/** Capability set by drivers which honor the OGRCoordinatePrecision settings
 * of geometry fields at layer creation and/or for OGRLayer::CreateGeomField().
 * Note that while those drivers honor the settings at feature writing time,
 * they might not be able to store the precision settings in layer metadata,
 * hence on reading it might not be possible to recover the precision with
 * which coordinates have been written.
 * @since GDAL 3.9
 */
#define GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION                              \
    "DCAP_HONOR_GEOM_COORDINATE_PRECISION"

/** List of (space separated) flags indicating the features of relationships are
 * supported by the driver.
 *
 * Supported values are:
 *
 * - "OneToOne": supports one-to-one relationships, see
 * GDALRelationshipCardinality::GRC_ONE_TO_ONE
 * - "OneToMany": supports one-to-many relationships, see
 * GDALRelationshipCardinality::GRC_ONE_TO_MANY
 * - "ManyToOne": supports many-to-one relationships, see
 * GDALRelationshipCardinality::GRC_MANY_TO_ONE
 * - "ManyToMany": supports many-to-many relationships, see
 * GDALRelationshipCardinality::GRC_MANY_TO_MANY
 * - "Composite": supports composite relationship types, see
 * GDALRelationshipType::GRT_COMPOSITE
 * - "Association": supports association relationship types, see
 * GDALRelationshipType::GRT_ASSOCIATION
 * - "Aggregation": supports aggregation relationship types, see
 * GDALRelationshipType::GRT_AGGREGATION
 * - "MultipleFieldKeys": multiple fields can be used for relationship keys. If
 * not present then only a single field name can be used.
 * - "ForwardPathLabel": supports forward path labels
 * - "BackwardPathLabel": supports backward path labels
 *
 * @since GDAL 3.6
 */
#define GDAL_DMD_RELATIONSHIP_FLAGS "GDAL_DMD_RELATIONSHIP_FLAGS"

/** List of (space separated) standard related table types which are recognised
 * by the driver.
 *
 * See GDALRelationshipGetRelatedTableType/GDALRelationshipSetRelatedTableType
 *
 * @since GDAL 3.7
 */
#define GDAL_DMD_RELATIONSHIP_RELATED_TABLE_TYPES                              \
    "GDAL_DMD_RELATIONSHIP_RELATED_TABLE_TYPES"

/** Capability set by drivers for formats which support renaming vector layers.
 *
 * @since GDAL 3.5
 */
#define GDAL_DCAP_RENAME_LAYERS "DCAP_RENAME_LAYERS"

/** List of (space separated) field domain types supported by the AddFieldDomain()
 * API.
 *
 * Supported values are Coded, Range and Glob, corresponding to the
 * OGRFieldDomainType::OFDT_CODED, OGRFieldDomainType::OFDT_RANGE, and
 * OGRFieldDomainType::OFDT_GLOB field domain types respectively.
 *
 * @since GDAL 3.5
 */
#define GDAL_DMD_CREATION_FIELD_DOMAIN_TYPES "DMD_CREATION_FIELD_DOMAIN_TYPES"

/** List of (space separated) flags supported by the
 * OGRLayer::AlterGeomFieldDefn() API.
 *
 * Supported values are "Name", "Type", "Nullable", "SRS", "CoordinateEpoch",
 * corresponding respectively to the ALTER_GEOM_FIELD_DEFN_NAME_FLAG,
 * ALTER_GEOM_FIELD_DEFN_TYPE_FLAG, ALTER_GEOM_FIELD_DEFN_NULLABLE_FLAG,
 * ALTER_GEOM_FIELD_DEFN_SRS_FLAG, ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG
 * flags. Note that advertizing one of these flags doesn't necessarily mean that
 * all modifications of the corresponding property can be made. For example,
 * altering the geometry type may be restricted by the type of the geometries in
 * the field, or changing the nullable state to non-nullable is not possible if
 * null geometries are present, etc.
 *
 * @since GDAL 3.6
 */
#define GDAL_DMD_ALTER_GEOM_FIELD_DEFN_FLAGS "DMD_ALTER_GEOM_FIELD_DEFN_FLAGS"

/** List of (space separated) SQL dialects supported by the driver.
 *
 * The default SQL dialect for the driver will always be the first listed value.
 *
 * Standard values are:
 *
 * - "OGRSQL": the OGR SQL dialect, see
 * https://gdal.org/user/ogr_sql_dialect.html
 * - "SQLITE": the SQLite dialect, see
 * https://gdal.org/user/sql_sqlite_dialect.html
 * - "NATIVE": for drivers with an RDBMS backend this value indicates that the
 * SQL will be passed directly to that database backend, and therefore the
 * RDBMS' native dialect will be used
 *
 * Other dialect values may also be present for some drivers (for some of them,
 * the query string to use might not even by SQL but a dedicated query
 * language). For further details on their interpretation, see the documentation
 * for the respective driver.
 *
 * @since GDAL 3.6
 */
#define GDAL_DMD_SUPPORTED_SQL_DIALECTS "DMD_SUPPORTED_SQL_DIALECTS"

/*! @cond Doxygen_Suppress */
#define GDAL_DMD_PLUGIN_INSTALLATION_MESSAGE "DMD_PLUGIN_INSTALLATION_MESSAGE"
/*! @endcond */

/** List of (space separated) items that a dataset opened in update mode supports
 * updating. Possible values are:
 * - for raster: "GeoTransform" (through GDALDataset::SetGeoTransform),
 *   "SRS" (GDALDataset::SetSpatialRef), "GCPs" (GDALDataset::SetGCPs()),
 *    "NoData" (GDALRasterBand::SetNoDataValue),
 *   "ColorInterpretation" (GDALRasterBand::SetColorInterpretation()),
 *   "RasterValues" (GF_Write flag of GDALDataset::RasterIO() and GDALRasterBand::RasterIO()),
 *   "DatasetMetadata" (GDALDataset::SetMetadata/SetMetadataItem), "BandMetadata"
 *   (GDALRasterBand::SetMetadata/SetMetadataItem)
 * - for vector: "Features" (OGRLayer::SetFeature()), "DatasetMetadata",
 *   "LayerMetadata"
 *
 * No distinction is made if the update is done in the native format,
 * or in a Persistent Auxiliary Metadata .aux.xml side car file.
 *
 * @since GDAL 3.11
 */
#define GDAL_DMD_UPDATE_ITEMS "DMD_UPDATE_ITEMS"

/** Value for GDALDimension::GetType() specifying the X axis of a horizontal
 * CRS.
 * @since GDAL 3.1
 */
#define GDAL_DIM_TYPE_HORIZONTAL_X "HORIZONTAL_X"

/** Value for GDALDimension::GetType() specifying the Y axis of a horizontal
 * CRS.
 * @since GDAL 3.1
 */
#define GDAL_DIM_TYPE_HORIZONTAL_Y "HORIZONTAL_Y"

/** Value for GDALDimension::GetType() specifying a vertical axis.
 * @since GDAL 3.1
 */
#define GDAL_DIM_TYPE_VERTICAL "VERTICAL"

/** Value for GDALDimension::GetType() specifying a temporal axis.
 * @since GDAL 3.1
 */
#define GDAL_DIM_TYPE_TEMPORAL "TEMPORAL"

/** Value for GDALDimension::GetType() specifying a parametric axis.
 * @since GDAL 3.1
 */
#define GDAL_DIM_TYPE_PARAMETRIC "PARAMETRIC"

#define GDsCAddRelationship                                                    \
    "AddRelationship" /**< Dataset capability for supporting AddRelationship() \
                         (at least partially) */
#define GDsCDeleteRelationship                                                 \
    "DeleteRelationship" /**< Dataset capability for supporting                \
                            DeleteRelationship()*/
#define GDsCUpdateRelationship                                                 \
    "UpdateRelationship" /**< Dataset capability for supporting                \
                            UpdateRelationship()*/

/** Dataset capability if GDALDataset::GetExtent() is fast.
 *
 * @since 3.12
 */
#define GDsCFastGetExtent "FastGetExtent"

/** Dataset capability if GDALDataset::GetExtentWGS84LongLat() is fast.
 *
 * @since 3.12
 */
#define GDsCFastGetExtentWGS84LongLat "FastGetExtentWGS84LongLat"

void CPL_DLL CPL_STDCALL GDALAllRegister(void);
void CPL_DLL GDALRegisterPlugins(void);
CPLErr CPL_DLL GDALRegisterPlugin(const char *name);

GDALDatasetH CPL_DLL CPL_STDCALL
GDALCreate(GDALDriverH hDriver, const char *, int, int, int, GDALDataType,
           CSLConstList) CPL_WARN_UNUSED_RESULT;
GDALDatasetH CPL_DLL CPL_STDCALL GDALCreateCopy(GDALDriverH, const char *,
                                                GDALDatasetH, int, CSLConstList,
                                                GDALProgressFunc,
                                                void *) CPL_WARN_UNUSED_RESULT;

GDALDriverH CPL_DLL CPL_STDCALL GDALIdentifyDriver(const char *pszFilename,
                                                   CSLConstList papszFileList);

GDALDriverH CPL_DLL CPL_STDCALL GDALIdentifyDriverEx(
    const char *pszFilename, unsigned int nIdentifyFlags,
    const char *const *papszAllowedDrivers, const char *const *papszFileList);

GDALDatasetH CPL_DLL CPL_STDCALL
GDALOpen(const char *pszFilename, GDALAccess eAccess) CPL_WARN_UNUSED_RESULT;
GDALDatasetH CPL_DLL CPL_STDCALL GDALOpenShared(const char *, GDALAccess)
    CPL_WARN_UNUSED_RESULT;

/* Note: we define GDAL_OF_READONLY and GDAL_OF_UPDATE to be on purpose */
/* equals to GA_ReadOnly and GA_Update */

/** Open in read-only mode.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_READONLY 0x00

/** Open in update mode.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_UPDATE 0x01

/** Allow raster and vector drivers to be used.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_ALL 0x00

/** Allow raster drivers to be used.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_RASTER 0x02

/** Allow vector drivers to be used.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_VECTOR 0x04

/** Allow gnm drivers to be used.
 * Used by GDALOpenEx().
 * @since GDAL 2.1
 */
#define GDAL_OF_GNM 0x08

/** Allow multidimensional raster drivers to be used.
 * Used by GDALOpenEx().
 * @since GDAL 3.1
 */
#define GDAL_OF_MULTIDIM_RASTER 0x10

#ifndef DOXYGEN_SKIP
#define GDAL_OF_KIND_MASK 0x1E
#endif

/** Open in shared mode.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_SHARED 0x20

/** Emit error message in case of failed open.
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_VERBOSE_ERROR 0x40

/** Open as internal dataset. Such dataset isn't registered in the global list
 * of opened dataset. Cannot be used with GDAL_OF_SHARED.
 *
 * Used by GDALOpenEx().
 * @since GDAL 2.0
 */
#define GDAL_OF_INTERNAL 0x80

/** Let GDAL decide if a array-based or hashset-based storage strategy for
 * cached blocks must be used.
 *
 * GDAL_OF_DEFAULT_BLOCK_ACCESS, GDAL_OF_ARRAY_BLOCK_ACCESS and
 * GDAL_OF_HASHSET_BLOCK_ACCESS are mutually exclusive.
 *
 * Used by GDALOpenEx().
 * @since GDAL 2.1
 */
#define GDAL_OF_DEFAULT_BLOCK_ACCESS 0

/** Use a array-based storage strategy for cached blocks.
 *
 * GDAL_OF_DEFAULT_BLOCK_ACCESS, GDAL_OF_ARRAY_BLOCK_ACCESS and
 * GDAL_OF_HASHSET_BLOCK_ACCESS are mutually exclusive.
 *
 * Used by GDALOpenEx().
 * @since GDAL 2.1
 */
#define GDAL_OF_ARRAY_BLOCK_ACCESS 0x100

/** Use a hashset-based storage strategy for cached blocks.
 *
 * GDAL_OF_DEFAULT_BLOCK_ACCESS, GDAL_OF_ARRAY_BLOCK_ACCESS and
 * GDAL_OF_HASHSET_BLOCK_ACCESS are mutually exclusive.
 *
 * Used by GDALOpenEx().
 * @since GDAL 2.1
 */
#define GDAL_OF_HASHSET_BLOCK_ACCESS 0x200

#ifndef DOXYGEN_SKIP
/* Reserved for a potential future alternative to GDAL_OF_ARRAY_BLOCK_ACCESS
 * and GDAL_OF_HASHSET_BLOCK_ACCESS */
#define GDAL_OF_RESERVED_1 0x300

/** Mask to detect the block access method */
#define GDAL_OF_BLOCK_ACCESS_MASK 0x300
#endif

#ifndef DOXYGEN_SKIP
/** Set by GDALOpenEx() to indicate to Identify() method that they are called
 * from it */
#define GDAL_OF_FROM_GDALOPEN 0x400
#endif

/** Open in thread-safe mode. Not compatible with
 * GDAL_OF_VECTOR, GDAL_OF_MULTIDIM_RASTER or GDAL_OF_UPDATE
 *
 * Used by GDALOpenEx().
 * @since GDAL 3.10
 */
#define GDAL_OF_THREAD_SAFE 0x800

GDALDatasetH CPL_DLL CPL_STDCALL GDALOpenEx(
    const char *pszFilename, unsigned int nOpenFlags,
    const char *const *papszAllowedDrivers, const char *const *papszOpenOptions,
    const char *const *papszSiblingFiles) CPL_WARN_UNUSED_RESULT;

int CPL_DLL CPL_STDCALL GDALDumpOpenDatasets(FILE *);

GDALDriverH CPL_DLL CPL_STDCALL GDALGetDriverByName(const char *);
int CPL_DLL CPL_STDCALL GDALGetDriverCount(void);
GDALDriverH CPL_DLL CPL_STDCALL GDALGetDriver(int);
GDALDriverH CPL_DLL CPL_STDCALL GDALCreateDriver(void);
void CPL_DLL CPL_STDCALL GDALDestroyDriver(GDALDriverH);
int CPL_DLL CPL_STDCALL GDALRegisterDriver(GDALDriverH);
void CPL_DLL CPL_STDCALL GDALDeregisterDriver(GDALDriverH);
void CPL_DLL CPL_STDCALL GDALDestroyDriverManager(void);
void CPL_DLL GDALDestroy(void);
CPLErr CPL_DLL CPL_STDCALL GDALDeleteDataset(GDALDriverH, const char *);
CPLErr CPL_DLL CPL_STDCALL GDALRenameDataset(GDALDriverH,
                                             const char *pszNewName,
                                             const char *pszOldName);
CPLErr CPL_DLL CPL_STDCALL GDALCopyDatasetFiles(GDALDriverH,
                                                const char *pszNewName,
                                                const char *pszOldName);
int CPL_DLL CPL_STDCALL
GDALValidateCreationOptions(GDALDriverH, CSLConstList papszCreationOptions);
char CPL_DLL **GDALGetOutputDriversForDatasetName(const char *pszDestFilename,
                                                  int nFlagRasterVector,
                                                  bool bSingleMatch,
                                                  bool bEmitWarning);

bool CPL_DLL GDALDriverHasOpenOption(GDALDriverH,
                                     const char *pszOpenOptionName);

/* The following are deprecated */
const char CPL_DLL *CPL_STDCALL GDALGetDriverShortName(GDALDriverH);
const char CPL_DLL *CPL_STDCALL GDALGetDriverLongName(GDALDriverH);
const char CPL_DLL *CPL_STDCALL GDALGetDriverHelpTopic(GDALDriverH);
const char CPL_DLL *CPL_STDCALL GDALGetDriverCreationOptionList(GDALDriverH);

/* ==================================================================== */
/*      GDAL_GCP                                                        */
/* ==================================================================== */

/** Ground Control Point */
typedef struct
{
    /** Unique identifier, often numeric */
    char *pszId;

    /** Informational message or "" */
    char *pszInfo;

    /** Pixel (x) location of GCP on raster */
    double dfGCPPixel;
    /** Line (y) location of GCP on raster */
    double dfGCPLine;

    /** X position of GCP in georeferenced space */
    double dfGCPX;

    /** Y position of GCP in georeferenced space */
    double dfGCPY;

    /** Elevation of GCP, or zero if not known */
    double dfGCPZ;
} GDAL_GCP;

void CPL_DLL CPL_STDCALL GDALInitGCPs(int, GDAL_GCP *);
void CPL_DLL CPL_STDCALL GDALDeinitGCPs(int, GDAL_GCP *);
GDAL_GCP CPL_DLL *CPL_STDCALL GDALDuplicateGCPs(int, const GDAL_GCP *);

int CPL_DLL CPL_STDCALL GDALGCPsToGeoTransform(
    int nGCPCount, const GDAL_GCP *pasGCPs, double *padfGeoTransform,
    int bApproxOK) CPL_WARN_UNUSED_RESULT;
int CPL_DLL CPL_STDCALL GDALInvGeoTransform(const double *padfGeoTransformIn,
                                            double *padfInvGeoTransformOut)
    CPL_WARN_UNUSED_RESULT;
void CPL_DLL CPL_STDCALL GDALApplyGeoTransform(const double *, double, double,
                                               double *, double *);
void CPL_DLL GDALComposeGeoTransforms(const double *padfGeoTransform1,
                                      const double *padfGeoTransform2,
                                      double *padfGeoTransformOut);
int CPL_DLL GDALGCPsToHomography(int nGCPCount, const GDAL_GCP *pasGCPs,
                                 double *padfHomography) CPL_WARN_UNUSED_RESULT;
int CPL_DLL GDALInvHomography(const double *padfHomographyIn,
                              double *padfInvHomographyOut)
    CPL_WARN_UNUSED_RESULT;
int CPL_DLL GDALApplyHomography(const double *, double, double, double *,
                                double *) CPL_WARN_UNUSED_RESULT;
void CPL_DLL GDALComposeHomographies(const double *padfHomography1,
                                     const double *padfHomography2,
                                     double *padfHomographyOut);

/* ==================================================================== */
/*      major objects (dataset, and, driver, drivermanager).            */
/* ==================================================================== */

char CPL_DLL **CPL_STDCALL GDALGetMetadataDomainList(GDALMajorObjectH hObject);
char CPL_DLL **CPL_STDCALL GDALGetMetadata(GDALMajorObjectH, const char *);
CPLErr CPL_DLL CPL_STDCALL GDALSetMetadata(GDALMajorObjectH, CSLConstList,
                                           const char *);
const char CPL_DLL *CPL_STDCALL GDALGetMetadataItem(GDALMajorObjectH,
                                                    const char *, const char *);
CPLErr CPL_DLL CPL_STDCALL GDALSetMetadataItem(GDALMajorObjectH, const char *,
                                               const char *, const char *);
const char CPL_DLL *CPL_STDCALL GDALGetDescription(GDALMajorObjectH);
void CPL_DLL CPL_STDCALL GDALSetDescription(GDALMajorObjectH, const char *);

/* ==================================================================== */
/*      GDALDataset class ... normally this represents one file.        */
/* ==================================================================== */

/** Name of driver metadata item for layer creation option list */
#define GDAL_DS_LAYER_CREATIONOPTIONLIST "DS_LAYER_CREATIONOPTIONLIST"

GDALDriverH CPL_DLL CPL_STDCALL GDALGetDatasetDriver(GDALDatasetH);
char CPL_DLL **CPL_STDCALL GDALGetFileList(GDALDatasetH);
void CPL_DLL GDALDatasetMarkSuppressOnClose(GDALDatasetH);
CPLErr CPL_DLL CPL_STDCALL GDALClose(GDALDatasetH);
int CPL_DLL CPL_STDCALL GDALGetRasterXSize(GDALDatasetH);
int CPL_DLL CPL_STDCALL GDALGetRasterYSize(GDALDatasetH);
int CPL_DLL CPL_STDCALL GDALGetRasterCount(GDALDatasetH);
GDALRasterBandH CPL_DLL CPL_STDCALL GDALGetRasterBand(GDALDatasetH, int);

bool CPL_DLL GDALDatasetIsThreadSafe(GDALDatasetH, int nScopeFlags,
                                     CSLConstList papszOptions);
GDALDatasetH CPL_DLL GDALGetThreadSafeDataset(GDALDatasetH, int nScopeFlags,
                                              CSLConstList papszOptions);

CPLErr CPL_DLL CPL_STDCALL GDALAddBand(GDALDatasetH hDS, GDALDataType eType,
                                       CSLConstList papszOptions);

GDALAsyncReaderH CPL_DLL CPL_STDCALL GDALBeginAsyncReader(
    GDALDatasetH hDS, int nXOff, int nYOff, int nXSize, int nYSize, void *pBuf,
    int nBufXSize, int nBufYSize, GDALDataType eBufType, int nBandCount,
    int *panBandMap, int nPixelSpace, int nLineSpace, int nBandSpace,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

void CPL_DLL CPL_STDCALL GDALEndAsyncReader(GDALDatasetH hDS,
                                            GDALAsyncReaderH hAsynchReaderH);

CPLErr CPL_DLL CPL_STDCALL GDALDatasetRasterIO(
    GDALDatasetH hDS, GDALRWFlag eRWFlag, int nDSXOff, int nDSYOff,
    int nDSXSize, int nDSYSize, void *pBuffer, int nBXSize, int nBYSize,
    GDALDataType eBDataType, int nBandCount, const int *panBandCount,
    int nPixelSpace, int nLineSpace, int nBandSpace) CPL_WARN_UNUSED_RESULT;

CPLErr CPL_DLL CPL_STDCALL GDALDatasetRasterIOEx(
    GDALDatasetH hDS, GDALRWFlag eRWFlag, int nDSXOff, int nDSYOff,
    int nDSXSize, int nDSYSize, void *pBuffer, int nBXSize, int nBYSize,
    GDALDataType eBDataType, int nBandCount, const int *panBandCount,
    GSpacing nPixelSpace, GSpacing nLineSpace, GSpacing nBandSpace,
    GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;

CPLErr CPL_DLL CPL_STDCALL GDALDatasetAdviseRead(
    GDALDatasetH hDS, int nDSXOff, int nDSYOff, int nDSXSize, int nDSYSize,
    int nBXSize, int nBYSize, GDALDataType eBDataType, int nBandCount,
    int *panBandCount, CSLConstList papszOptions);

char CPL_DLL **
GDALDatasetGetCompressionFormats(GDALDatasetH hDS, int nXOff, int nYOff,
                                 int nXSize, int nYSize, int nBandCount,
                                 const int *panBandList) CPL_WARN_UNUSED_RESULT;
CPLErr CPL_DLL GDALDatasetReadCompressedData(
    GDALDatasetH hDS, const char *pszFormat, int nXOff, int nYOff, int nXSize,
    int nYSize, int nBandCount, const int *panBandList, void **ppBuffer,
    size_t *pnBufferSize, char **ppszDetailedFormat);

const char CPL_DLL *CPL_STDCALL GDALGetProjectionRef(GDALDatasetH);
OGRSpatialReferenceH CPL_DLL GDALGetSpatialRef(GDALDatasetH);
CPLErr CPL_DLL CPL_STDCALL GDALSetProjection(GDALDatasetH, const char *);
CPLErr CPL_DLL GDALSetSpatialRef(GDALDatasetH, OGRSpatialReferenceH);
CPLErr CPL_DLL CPL_STDCALL GDALGetGeoTransform(GDALDatasetH, double *);
CPLErr CPL_DLL CPL_STDCALL GDALSetGeoTransform(GDALDatasetH, const double *);

CPLErr CPL_DLL GDALGetExtent(GDALDatasetH, OGREnvelope *,
                             OGRSpatialReferenceH hCRS);
CPLErr CPL_DLL GDALGetExtentWGS84LongLat(GDALDatasetH, OGREnvelope *);

CPLErr CPL_DLL GDALDatasetGeolocationToPixelLine(
    GDALDatasetH, double dfGeolocX, double dfGeolocY, OGRSpatialReferenceH hSRS,
    double *pdfPixel, double *pdfLine, CSLConstList papszTransformerOptions);

int CPL_DLL CPL_STDCALL GDALGetGCPCount(GDALDatasetH);
const char CPL_DLL *CPL_STDCALL GDALGetGCPProjection(GDALDatasetH);
OGRSpatialReferenceH CPL_DLL GDALGetGCPSpatialRef(GDALDatasetH);
const GDAL_GCP CPL_DLL *CPL_STDCALL GDALGetGCPs(GDALDatasetH);
CPLErr CPL_DLL CPL_STDCALL GDALSetGCPs(GDALDatasetH, int, const GDAL_GCP *,
                                       const char *);
CPLErr CPL_DLL GDALSetGCPs2(GDALDatasetH, int, const GDAL_GCP *,
                            OGRSpatialReferenceH);

void CPL_DLL *CPL_STDCALL GDALGetInternalHandle(GDALDatasetH, const char *);
int CPL_DLL CPL_STDCALL GDALReferenceDataset(GDALDatasetH);
int CPL_DLL CPL_STDCALL GDALDereferenceDataset(GDALDatasetH);
int CPL_DLL CPL_STDCALL GDALReleaseDataset(GDALDatasetH);

CPLErr CPL_DLL CPL_STDCALL GDALBuildOverviews(GDALDatasetH, const char *, int,
                                              const int *, int, const int *,
                                              GDALProgressFunc,
                                              void *) CPL_WARN_UNUSED_RESULT;
CPLErr CPL_DLL CPL_STDCALL GDALBuildOverviewsEx(
    GDALDatasetH, const char *, int, const int *, int, const int *,
    GDALProgressFunc, void *, CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
void CPL_DLL CPL_STDCALL GDALGetOpenDatasets(GDALDatasetH **hDS, int *pnCount);
int CPL_DLL CPL_STDCALL GDALGetAccess(GDALDatasetH hDS);
CPLErr CPL_DLL CPL_STDCALL GDALFlushCache(GDALDatasetH hDS);
CPLErr CPL_DLL CPL_STDCALL GDALDropCache(GDALDatasetH hDS);

CPLErr CPL_DLL CPL_STDCALL GDALCreateDatasetMaskBand(GDALDatasetH hDS,
                                                     int nFlags);

CPLErr CPL_DLL CPL_STDCALL GDALDatasetCopyWholeRaster(
    GDALDatasetH hSrcDS, GDALDatasetH hDstDS, CSLConstList papszOptions,
    GDALProgressFunc pfnProgress, void *pProgressData) CPL_WARN_UNUSED_RESULT;

CPLErr CPL_DLL CPL_STDCALL GDALRasterBandCopyWholeRaster(
    GDALRasterBandH hSrcBand, GDALRasterBandH hDstBand,
    const char *const *constpapszOptions, GDALProgressFunc pfnProgress,
    void *pProgressData) CPL_WARN_UNUSED_RESULT;

CPLErr CPL_DLL GDALRegenerateOverviews(GDALRasterBandH hSrcBand,
                                       int nOverviewCount,
                                       GDALRasterBandH *pahOverviewBands,
                                       const char *pszResampling,
                                       GDALProgressFunc pfnProgress,
                                       void *pProgressData);

CPLErr CPL_DLL GDALRegenerateOverviewsEx(GDALRasterBandH hSrcBand,
                                         int nOverviewCount,
                                         GDALRasterBandH *pahOverviewBands,
                                         const char *pszResampling,
                                         GDALProgressFunc pfnProgress,
                                         void *pProgressData,
                                         CSLConstList papszOptions);

int CPL_DLL GDALDatasetGetLayerCount(GDALDatasetH);
OGRLayerH CPL_DLL GDALDatasetGetLayer(GDALDatasetH, int);

/* Defined here to avoid circular dependency with ogr_api.h */
GDALDatasetH CPL_DLL OGR_L_GetDataset(OGRLayerH hLayer);

OGRLayerH CPL_DLL GDALDatasetGetLayerByName(GDALDatasetH, const char *);
int CPL_DLL GDALDatasetIsLayerPrivate(GDALDatasetH, int);
OGRErr CPL_DLL GDALDatasetDeleteLayer(GDALDatasetH, int);
OGRLayerH CPL_DLL GDALDatasetCreateLayer(GDALDatasetH, const char *,
                                         OGRSpatialReferenceH,
                                         OGRwkbGeometryType, CSLConstList);
OGRLayerH CPL_DLL GDALDatasetCreateLayerFromGeomFieldDefn(GDALDatasetH,
                                                          const char *,
                                                          OGRGeomFieldDefnH,
                                                          CSLConstList);
OGRLayerH CPL_DLL GDALDatasetCopyLayer(GDALDatasetH, OGRLayerH, const char *,
                                       CSLConstList);
void CPL_DLL GDALDatasetResetReading(GDALDatasetH);
OGRFeatureH CPL_DLL GDALDatasetGetNextFeature(GDALDatasetH hDS,
                                              OGRLayerH *phBelongingLayer,
                                              double *pdfProgressPct,
                                              GDALProgressFunc pfnProgress,
                                              void *pProgressData);
int CPL_DLL GDALDatasetTestCapability(GDALDatasetH, const char *);
OGRLayerH CPL_DLL GDALDatasetExecuteSQL(GDALDatasetH, const char *,
                                        OGRGeometryH, const char *);
OGRErr CPL_DLL GDALDatasetAbortSQL(GDALDatasetH);
void CPL_DLL GDALDatasetReleaseResultSet(GDALDatasetH, OGRLayerH);
OGRStyleTableH CPL_DLL GDALDatasetGetStyleTable(GDALDatasetH);
void CPL_DLL GDALDatasetSetStyleTableDirectly(GDALDatasetH, OGRStyleTableH);
void CPL_DLL GDALDatasetSetStyleTable(GDALDatasetH, OGRStyleTableH);
OGRErr CPL_DLL GDALDatasetStartTransaction(GDALDatasetH hDS, int bForce);
OGRErr CPL_DLL GDALDatasetCommitTransaction(GDALDatasetH hDS);
OGRErr CPL_DLL GDALDatasetRollbackTransaction(GDALDatasetH hDS);
void CPL_DLL GDALDatasetClearStatistics(GDALDatasetH hDS);

char CPL_DLL **GDALDatasetGetFieldDomainNames(GDALDatasetH, CSLConstList)
    CPL_WARN_UNUSED_RESULT;
OGRFieldDomainH CPL_DLL GDALDatasetGetFieldDomain(GDALDatasetH hDS,
                                                  const char *pszName);
bool CPL_DLL GDALDatasetAddFieldDomain(GDALDatasetH hDS,
                                       OGRFieldDomainH hFieldDomain,
                                       char **ppszFailureReason);
bool CPL_DLL GDALDatasetDeleteFieldDomain(GDALDatasetH hDS, const char *pszName,
                                          char **ppszFailureReason);
bool CPL_DLL GDALDatasetUpdateFieldDomain(GDALDatasetH hDS,
                                          OGRFieldDomainH hFieldDomain,
                                          char **ppszFailureReason);

char CPL_DLL **GDALDatasetGetRelationshipNames(GDALDatasetH, CSLConstList)
    CPL_WARN_UNUSED_RESULT;
GDALRelationshipH CPL_DLL GDALDatasetGetRelationship(GDALDatasetH hDS,
                                                     const char *pszName);

bool CPL_DLL GDALDatasetAddRelationship(GDALDatasetH hDS,
                                        GDALRelationshipH hRelationship,
                                        char **ppszFailureReason);
bool CPL_DLL GDALDatasetDeleteRelationship(GDALDatasetH hDS,
                                           const char *pszName,
                                           char **ppszFailureReason);
bool CPL_DLL GDALDatasetUpdateRelationship(GDALDatasetH hDS,
                                           GDALRelationshipH hRelationship,
                                           char **ppszFailureReason);

/** Type of functions to pass to GDALDatasetSetQueryLoggerFunc
 * @since GDAL 3.7 */
typedef void (*GDALQueryLoggerFunc)(const char *pszSQL, const char *pszError,
                                    int64_t lNumRecords,
                                    int64_t lExecutionTimeMilliseconds,
                                    void *pQueryLoggerArg);

/**
 * Sets the SQL query logger callback.
 *
 * When supported by the driver, the callback will be called with
 * the executed SQL text, the error message, the execution time in milliseconds,
 * the number of records fetched/affected and the client status data.
 *
 * A value of -1 in the execution time or in the number of records indicates
 * that the values are unknown.
 *
 * @param hDS                   Dataset handle.
 * @param pfnQueryLoggerFunc    Callback function
 * @param poQueryLoggerArg      Opaque client status data
 * @return                      true in case of success.
 * @since                       GDAL 3.7
 */
bool CPL_DLL GDALDatasetSetQueryLoggerFunc(
    GDALDatasetH hDS, GDALQueryLoggerFunc pfnQueryLoggerFunc,
    void *poQueryLoggerArg);

/* ==================================================================== */
/*      Informational utilities about subdatasets in file names         */
/* ==================================================================== */

/**
 * @brief Returns a new GDALSubdatasetInfo object with methods to extract
 *        and manipulate subdataset information.
 *        If the pszFileName argument is not recognized by any driver as
 *        a subdataset descriptor, NULL is returned.
 *        The returned object must be freed with GDALDestroySubdatasetInfo().
 * @param pszFileName           File name with subdataset information
 * @note                        This method does not check if the subdataset actually exists.
 * @return                      Opaque pointer to a GDALSubdatasetInfo object or NULL if no drivers accepted the file name.
 * @since                       GDAL 3.8
 */
GDALSubdatasetInfoH CPL_DLL GDALGetSubdatasetInfo(const char *pszFileName);

/**
 * @brief Returns the file path component of a
 *        subdataset descriptor effectively stripping the information about the subdataset
 *        and returning the "parent" dataset descriptor.
 *        The returned string must be freed with CPLFree().
 * @param hInfo                 Pointer to GDALSubdatasetInfo object
 * @note                        This method does not check if the subdataset actually exists.
 * @return                      The original string with the subdataset information removed.
 * @since                       GDAL 3.8
 */
char CPL_DLL *GDALSubdatasetInfoGetPathComponent(GDALSubdatasetInfoH hInfo);

/**
 * @brief Returns the subdataset component of a subdataset descriptor descriptor.
 *        The returned string must be freed with CPLFree().
 * @param hInfo                 Pointer to GDALSubdatasetInfo object
 * @note                        This method does not check if the subdataset actually exists.
 * @return                      The subdataset name.
 * @since                       GDAL 3.8
 */
char CPL_DLL *
GDALSubdatasetInfoGetSubdatasetComponent(GDALSubdatasetInfoH hInfo);

/**
 * @brief Replaces the path component of a subdataset descriptor.
 *        The returned string must be freed with CPLFree().
 * @param hInfo                 Pointer to GDALSubdatasetInfo object
 * @param pszNewPath            New path.
 * @note                        This method does not check if the subdataset actually exists.
 * @return                      The original subdataset descriptor with the old path component replaced by newPath.
 * @since                       GDAL 3.8
 */
char CPL_DLL *GDALSubdatasetInfoModifyPathComponent(GDALSubdatasetInfoH hInfo,
                                                    const char *pszNewPath);

/**
 * @brief Destroys a GDALSubdatasetInfo object.
 * @param hInfo                 Pointer to GDALSubdatasetInfo object
 * @since                       GDAL 3.8
 */
void CPL_DLL GDALDestroySubdatasetInfo(GDALSubdatasetInfoH hInfo);

/* ==================================================================== */
/*      GDALRasterBand ... one band/channel in a dataset.               */
/* ==================================================================== */

/* Note: the only user of SRCVAL() was frmts/vrt/pixelfunctions.cpp and we no */
/* longer use it. */

/**
 * SRCVAL - Macro which may be used by pixel functions to obtain
 *          a pixel from a source buffer.
 */
#define SRCVAL(papoSource, eSrcType, ii)                                                                      \
    (eSrcType == GDT_Byte                                                                                     \
         ? CPL_REINTERPRET_CAST(const GByte *, papoSource)[ii]                                                \
         : (eSrcType == GDT_Int8                                                                              \
                ? CPL_REINTERPRET_CAST(const GInt8 *, papoSource)[ii]                                         \
                : (eSrcType == GDT_Float32                                                                    \
                       ? CPL_REINTERPRET_CAST(const float *, papoSource)[ii]                                  \
                       : (eSrcType == GDT_Float64                                                             \
                              ? CPL_REINTERPRET_CAST(const double *,                                          \
                                                     papoSource)[ii]                                          \
                              : (eSrcType == GDT_Int32                                                        \
                                     ? CPL_REINTERPRET_CAST(const GInt32 *,                                   \
                                                            papoSource)[ii]                                   \
                                     : (eSrcType == GDT_UInt16                                                \
                                            ? CPL_REINTERPRET_CAST(                                           \
                                                  const GUInt16 *,                                            \
                                                  papoSource)[ii]                                             \
                                            : (eSrcType == GDT_Int16                                          \
                                                   ? CPL_REINTERPRET_CAST(                                    \
                                                         const GInt16 *,                                      \
                                                         papoSource)[ii]                                      \
                                                   : (eSrcType == GDT_UInt32                                  \
                                                          ? CPL_REINTERPRET_CAST(                             \
                                                                const GUInt32                                 \
                                                                    *,                                        \
                                                                papoSource)                                   \
                                                                [ii]                                          \
                                                          : (eSrcType ==                                      \
                                                                     GDT_CInt16                               \
                                                                 ? CPL_REINTERPRET_CAST(                      \
                                                                       const GInt16                           \
                                                                           *,                                 \
                                                                       papoSource)                            \
                                                                       [(ii)*2]                               \
                                                                 : (eSrcType ==                               \
                                                                            GDT_CInt32                        \
                                                                        ? CPL_REINTERPRET_CAST(               \
                                                                              const GInt32                    \
                                                                                  *,                          \
                                                                              papoSource)                     \
                                                                              [(ii)*2]                        \
                                                                        : (eSrcType ==                        \
                                                                                   GDT_CFloat32               \
                                                                               ? CPL_REINTERPRET_CAST(        \
                                                                                     const float              \
                                                                                         *,                   \
                                                                                     papoSource)              \
                                                                                     [(ii)*2]                 \
                                                                               : (eSrcType ==                 \
                                                                                          GDT_CFloat64        \
                                                                                      ? CPL_REINTERPRET_CAST( \
                                                                                            const double      \
                                                                                                *,            \
                                                                                            papoSource)       \
                                                                                            [(ii)*2]          \
                                                                                      : 0))))))))))))

/** Type of functions to pass to GDALAddDerivedBandPixelFunc.
 * @since GDAL 2.2 */
typedef CPLErr (*GDALDerivedPixelFunc)(void **papoSources, int nSources,
                                       void *pData, int nBufXSize,
                                       int nBufYSize, GDALDataType eSrcType,
                                       GDALDataType eBufType, int nPixelSpace,
                                       int nLineSpace);

/** Type of functions to pass to GDALAddDerivedBandPixelFuncWithArgs.
 * @since GDAL 3.4 */
typedef CPLErr (*GDALDerivedPixelFuncWithArgs)(
    void **papoSources, int nSources, void *pData, int nBufXSize, int nBufYSize,
    GDALDataType eSrcType, GDALDataType eBufType, int nPixelSpace,
    int nLineSpace, CSLConstList papszFunctionArgs);

GDALDataType CPL_DLL CPL_STDCALL GDALGetRasterDataType(GDALRasterBandH);
void CPL_DLL CPL_STDCALL GDALGetBlockSize(GDALRasterBandH, int *pnXSize,
                                          int *pnYSize);

CPLErr CPL_DLL CPL_STDCALL GDALGetActualBlockSize(GDALRasterBandH,
                                                  int nXBlockOff,
                                                  int nYBlockOff, int *pnXValid,
                                                  int *pnYValid);

CPLErr CPL_DLL CPL_STDCALL GDALRasterAdviseRead(GDALRasterBandH hRB,
                                                int nDSXOff, int nDSYOff,
                                                int nDSXSize, int nDSYSize,
                                                int nBXSize, int nBYSize,
                                                GDALDataType eBDataType,
                                                CSLConstList papszOptions);

CPLErr CPL_DLL CPL_STDCALL GDALRasterIO(GDALRasterBandH hRBand,
                                        GDALRWFlag eRWFlag, int nDSXOff,
                                        int nDSYOff, int nDSXSize, int nDSYSize,
                                        void *pBuffer, int nBXSize, int nBYSize,
                                        GDALDataType eBDataType,
                                        int nPixelSpace,
                                        int nLineSpace) CPL_WARN_UNUSED_RESULT;
CPLErr CPL_DLL CPL_STDCALL GDALRasterIOEx(
    GDALRasterBandH hRBand, GDALRWFlag eRWFlag, int nDSXOff, int nDSYOff,
    int nDSXSize, int nDSYSize, void *pBuffer, int nBXSize, int nBYSize,
    GDALDataType eBDataType, GSpacing nPixelSpace, GSpacing nLineSpace,
    GDALRasterIOExtraArg *psExtraArg) CPL_WARN_UNUSED_RESULT;
CPLErr CPL_DLL CPL_STDCALL GDALReadBlock(GDALRasterBandH, int, int,
                                         void *) CPL_WARN_UNUSED_RESULT;
CPLErr CPL_DLL CPL_STDCALL GDALWriteBlock(GDALRasterBandH, int, int,
                                          void *) CPL_WARN_UNUSED_RESULT;
int CPL_DLL CPL_STDCALL GDALGetRasterBandXSize(GDALRasterBandH);
int CPL_DLL CPL_STDCALL GDALGetRasterBandYSize(GDALRasterBandH);
GDALAccess CPL_DLL CPL_STDCALL GDALGetRasterAccess(GDALRasterBandH);
int CPL_DLL CPL_STDCALL GDALGetBandNumber(GDALRasterBandH);
GDALDatasetH CPL_DLL CPL_STDCALL GDALGetBandDataset(GDALRasterBandH);

GDALColorInterp
    CPL_DLL CPL_STDCALL GDALGetRasterColorInterpretation(GDALRasterBandH);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterColorInterpretation(GDALRasterBandH,
                                                            GDALColorInterp);
GDALColorTableH CPL_DLL CPL_STDCALL GDALGetRasterColorTable(GDALRasterBandH);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterColorTable(GDALRasterBandH,
                                                   GDALColorTableH);
int CPL_DLL CPL_STDCALL GDALHasArbitraryOverviews(GDALRasterBandH);
int CPL_DLL CPL_STDCALL GDALGetOverviewCount(GDALRasterBandH);
GDALRasterBandH CPL_DLL CPL_STDCALL GDALGetOverview(GDALRasterBandH, int);
double CPL_DLL CPL_STDCALL GDALGetRasterNoDataValue(GDALRasterBandH, int *);
int64_t CPL_DLL CPL_STDCALL GDALGetRasterNoDataValueAsInt64(GDALRasterBandH,
                                                            int *);
uint64_t CPL_DLL CPL_STDCALL GDALGetRasterNoDataValueAsUInt64(GDALRasterBandH,
                                                              int *);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterNoDataValue(GDALRasterBandH, double);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterNoDataValueAsInt64(GDALRasterBandH,
                                                           int64_t);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterNoDataValueAsUInt64(GDALRasterBandH,
                                                            uint64_t);
CPLErr CPL_DLL CPL_STDCALL GDALDeleteRasterNoDataValue(GDALRasterBandH);
char CPL_DLL **CPL_STDCALL GDALGetRasterCategoryNames(GDALRasterBandH);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterCategoryNames(GDALRasterBandH,
                                                      CSLConstList);
double CPL_DLL CPL_STDCALL GDALGetRasterMinimum(GDALRasterBandH,
                                                int *pbSuccess);
double CPL_DLL CPL_STDCALL GDALGetRasterMaximum(GDALRasterBandH,
                                                int *pbSuccess);
CPLErr CPL_DLL CPL_STDCALL GDALGetRasterStatistics(
    GDALRasterBandH, int bApproxOK, int bForce, double *pdfMin, double *pdfMax,
    double *pdfMean, double *pdfStdDev);
CPLErr CPL_DLL CPL_STDCALL
GDALComputeRasterStatistics(GDALRasterBandH, int bApproxOK, double *pdfMin,
                            double *pdfMax, double *pdfMean, double *pdfStdDev,
                            GDALProgressFunc pfnProgress, void *pProgressData);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterStatistics(GDALRasterBandH hBand,
                                                   double dfMin, double dfMax,
                                                   double dfMean,
                                                   double dfStdDev);

GDALMDArrayH
    CPL_DLL GDALRasterBandAsMDArray(GDALRasterBandH) CPL_WARN_UNUSED_RESULT;

const char CPL_DLL *CPL_STDCALL GDALGetRasterUnitType(GDALRasterBandH);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterUnitType(GDALRasterBandH hBand,
                                                 const char *pszNewValue);
double CPL_DLL CPL_STDCALL GDALGetRasterOffset(GDALRasterBandH, int *pbSuccess);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterOffset(GDALRasterBandH hBand,
                                               double dfNewOffset);
double CPL_DLL CPL_STDCALL GDALGetRasterScale(GDALRasterBandH, int *pbSuccess);
CPLErr CPL_DLL CPL_STDCALL GDALSetRasterScale(GDALRasterBandH hBand,
                                              double dfNewOffset);
CPLErr CPL_DLL CPL_STDCALL GDALComputeRasterMinMax(GDALRasterBandH hBand,
                                                   int bApproxOK,
                                                   double adfMinMax[2]);
CPLErr CPL_DLL GDALComputeRasterMinMaxLocation(GDALRasterBandH hBand,
                                               double *pdfMin, double *pdfMax,
                                               int *pnMinX, int *pnMinY,
                                               int *pnMaxX, int *pnMaxY);
CPLErr CPL_DLL CPL_STDCALL GDALFlushRasterCache(GDALRasterBandH hBand);
CPLErr CPL_DLL CPL_STDCALL GDALDropRasterCache(GDALRasterBandH hBand);
CPLErr CPL_DLL CPL_STDCALL GDALGetRasterHistogram(
    GDALRasterBandH hBand, double dfMin, double dfMax, int nBuckets,
    int *panHistogram, int bIncludeOutOfRange, int bApproxOK,
    GDALProgressFunc pfnProgress, void *pProgressData)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use GDALGetRasterHistogramEx() instead")
    /*! @endcond */
    ;
CPLErr CPL_DLL CPL_STDCALL GDALGetRasterHistogramEx(
    GDALRasterBandH hBand, double dfMin, double dfMax, int nBuckets,
    GUIntBig *panHistogram, int bIncludeOutOfRange, int bApproxOK,
    GDALProgressFunc pfnProgress, void *pProgressData);
CPLErr CPL_DLL CPL_STDCALL
GDALGetDefaultHistogram(GDALRasterBandH hBand, double *pdfMin, double *pdfMax,
                        int *pnBuckets, int **ppanHistogram, int bForce,
                        GDALProgressFunc pfnProgress, void *pProgressData)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use GDALGetDefaultHistogramEx() instead")
    /*! @endcond */
    ;
CPLErr CPL_DLL CPL_STDCALL
GDALGetDefaultHistogramEx(GDALRasterBandH hBand, double *pdfMin, double *pdfMax,
                          int *pnBuckets, GUIntBig **ppanHistogram, int bForce,
                          GDALProgressFunc pfnProgress, void *pProgressData);
CPLErr CPL_DLL CPL_STDCALL GDALSetDefaultHistogram(GDALRasterBandH hBand,
                                                   double dfMin, double dfMax,
                                                   int nBuckets,
                                                   int *panHistogram)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use GDALSetDefaultHistogramEx() instead")
    /*! @endcond */
    ;
CPLErr CPL_DLL CPL_STDCALL GDALSetDefaultHistogramEx(GDALRasterBandH hBand,
                                                     double dfMin, double dfMax,
                                                     int nBuckets,
                                                     GUIntBig *panHistogram);
int CPL_DLL CPL_STDCALL GDALGetRandomRasterSample(GDALRasterBandH, int,
                                                  float *);
GDALRasterBandH CPL_DLL CPL_STDCALL GDALGetRasterSampleOverview(GDALRasterBandH,
                                                                int);
GDALRasterBandH CPL_DLL CPL_STDCALL
    GDALGetRasterSampleOverviewEx(GDALRasterBandH, GUIntBig);
CPLErr CPL_DLL CPL_STDCALL GDALFillRaster(GDALRasterBandH hBand,
                                          double dfRealValue,
                                          double dfImaginaryValue);
CPLErr CPL_DLL CPL_STDCALL GDALComputeBandStats(
    GDALRasterBandH hBand, int nSampleStep, double *pdfMean, double *pdfStdDev,
    GDALProgressFunc pfnProgress, void *pProgressData);
CPLErr CPL_DLL GDALOverviewMagnitudeCorrection(GDALRasterBandH hBaseBand,
                                               int nOverviewCount,
                                               GDALRasterBandH *pahOverviews,
                                               GDALProgressFunc pfnProgress,
                                               void *pProgressData);

GDALRasterAttributeTableH CPL_DLL CPL_STDCALL
GDALGetDefaultRAT(GDALRasterBandH hBand);
CPLErr CPL_DLL CPL_STDCALL GDALSetDefaultRAT(GDALRasterBandH,
                                             GDALRasterAttributeTableH);
CPLErr CPL_DLL CPL_STDCALL GDALAddDerivedBandPixelFunc(
    const char *pszName, GDALDerivedPixelFunc pfnPixelFunc);
CPLErr CPL_DLL CPL_STDCALL GDALAddDerivedBandPixelFuncWithArgs(
    const char *pszName, GDALDerivedPixelFuncWithArgs pfnPixelFunc,
    const char *pszMetadata);

CPLErr CPL_DLL GDALRasterInterpolateAtPoint(GDALRasterBandH hBand,
                                            double dfPixel, double dfLine,
                                            GDALRIOResampleAlg eInterpolation,
                                            double *pdfRealValue,
                                            double *pdfImagValue);

CPLErr CPL_DLL GDALRasterInterpolateAtGeolocation(
    GDALRasterBandH hBand, double dfGeolocX, double dfGeolocY,
    OGRSpatialReferenceH hSRS, GDALRIOResampleAlg eInterpolation,
    double *pdfRealValue, double *pdfImagValue,
    CSLConstList papszTransformerOptions);

/** Generic pointer for the working structure of VRTProcessedDataset
 * function. */
typedef void *VRTPDWorkingDataPtr;

/** Initialization function to pass to GDALVRTRegisterProcessedDatasetFunc.
 *
 * This initialization function is called for each step of a VRTProcessedDataset
 * that uses the related algorithm.
 * The initialization function returns the output data type, output band count
 * and potentially initializes a working structure, typically parsing arguments.
 *
 * @param pszFuncName Function name. Must be unique and not null.
 * @param pUserData User data. May be nullptr. Must remain valid during the
 *                  lifetime of GDAL.
 * @param papszFunctionArgs Function arguments as a list of key=value pairs.
 * @param nInBands Number of input bands.
 * @param eInDT Input data type.
 * @param[in,out] padfInNoData Array of nInBands values for the input nodata
 *                             value. The init function may also override them.
 * @param[in,out] pnOutBands Pointer whose value must be set to the number of
 *                           output bands. This will be set to 0 by the caller
 *                           when calling the function, unless this is the
 *                           final step, in which case it will be initialized
 *                           with the number of expected output bands.
 * @param[out] peOutDT Pointer whose value must be set to the output
 *                     data type.
 * @param[in,out] ppadfOutNoData Pointer to an array of *pnOutBands values
 *                               for the output nodata value that the
 *                               function must set.
 *                               For non-final steps, *ppadfOutNoData
 *                               will be nullptr and it is the responsibility
 *                               of the function to CPLMalloc()'ate it.
 *                               If this is the final step, it will be
 *                               already allocated and initialized with the
 *                               expected nodata values from the output
 *                               dataset (if the init function need to
 *                               reallocate it, it must use CPLRealloc())
 * @param pszVRTPath Directory of the VRT
 * @param[out] ppWorkingData Pointer whose value must be set to a working
 *                           structure, or nullptr.
 * @return CE_None in case of success, error otherwise.
 * @since GDAL 3.9 */
typedef CPLErr (*GDALVRTProcessedDatasetFuncInit)(
    const char *pszFuncName, void *pUserData, CSLConstList papszFunctionArgs,
    int nInBands, GDALDataType eInDT, double *padfInNoData, int *pnOutBands,
    GDALDataType *peOutDT, double **ppadfOutNoData, const char *pszVRTPath,
    VRTPDWorkingDataPtr *ppWorkingData);

/** Free function to pass to GDALVRTRegisterProcessedDatasetFunc.
 *
 * @param pszFuncName Function name. Must be unique and not null.
 * @param pUserData User data. May be nullptr. Must remain valid during the
 *                  lifetime of GDAL.
 * @param pWorkingData Value of the *ppWorkingData output parameter of
 *                     GDALVRTProcessedDatasetFuncInit.
 * @since GDAL 3.9
 */
typedef void (*GDALVRTProcessedDatasetFuncFree)(
    const char *pszFuncName, void *pUserData, VRTPDWorkingDataPtr pWorkingData);

/** Processing function to pass to GDALVRTRegisterProcessedDatasetFunc.
 * @param pszFuncName Function name. Must be unique and not null.
 * @param pUserData User data. May be nullptr. Must remain valid during the
 *                  lifetime of GDAL.
 * @param pWorkingData Value of the *ppWorkingData output parameter of
 *                     GDALVRTProcessedDatasetFuncInit.
 * @param papszFunctionArgs Function arguments as a list of key=value pairs.
 * @param nBufXSize Width in pixels of pInBuffer and pOutBuffer
 * @param nBufYSize Height in pixels of pInBuffer and pOutBuffer
 * @param pInBuffer Input buffer. It is pixel-interleaved
 *                  (i.e. R00,G00,B00,R01,G01,B01, etc.)
 * @param nInBufferSize Size in bytes of pInBuffer
 * @param eInDT Data type of pInBuffer
 * @param nInBands Number of bands in pInBuffer.
 * @param padfInNoData Input nodata values.
 * @param pOutBuffer Output buffer. It is pixel-interleaved
 *                   (i.e. R00,G00,B00,R01,G01,B01, etc.)
 * @param nOutBufferSize Size in bytes of pOutBuffer
 * @param eOutDT Data type of pOutBuffer
 * @param nOutBands Number of bands in pOutBuffer.
 * @param padfOutNoData Input nodata values.
 * @param dfSrcXOff Source X coordinate in pixel of the top-left of the region
 * @param dfSrcYOff Source Y coordinate in pixel of the top-left of the region
 * @param dfSrcXSize Width in pixels of the region
 * @param dfSrcYSize Height in pixels of the region
 * @param adfSrcGT Source geotransform
 * @param pszVRTPath Directory of the VRT
 * @param papszExtra Extra arguments (unused for now)
 * @since GDAL 3.9
 */
typedef CPLErr (*GDALVRTProcessedDatasetFuncProcess)(
    const char *pszFuncName, void *pUserData, VRTPDWorkingDataPtr pWorkingData,
    CSLConstList papszFunctionArgs, int nBufXSize, int nBufYSize,
    const void *pInBuffer, size_t nInBufferSize, GDALDataType eInDT,
    int nInBands, const double *padfInNoData, void *pOutBuffer,
    size_t nOutBufferSize, GDALDataType eOutDT, int nOutBands,
    const double *padfOutNoData, double dfSrcXOff, double dfSrcYOff,
    double dfSrcXSize, double dfSrcYSize, const double adfSrcGT[/*6*/],
    const char *pszVRTPath, CSLConstList papszExtra);

CPLErr CPL_DLL GDALVRTRegisterProcessedDatasetFunc(
    const char *pszFuncName, void *pUserData, const char *pszXMLMetadata,
    GDALDataType eRequestedInputDT, const GDALDataType *paeSupportedInputDT,
    size_t nSupportedInputDTSize, const int *panSupportedInputBandCount,
    size_t nSupportedInputBandCountSize,
    GDALVRTProcessedDatasetFuncInit pfnInit,
    GDALVRTProcessedDatasetFuncFree pfnFree,
    GDALVRTProcessedDatasetFuncProcess pfnProcess, CSLConstList papszOptions);

GDALRasterBandH CPL_DLL CPL_STDCALL GDALGetMaskBand(GDALRasterBandH hBand);
int CPL_DLL CPL_STDCALL GDALGetMaskFlags(GDALRasterBandH hBand);
CPLErr CPL_DLL CPL_STDCALL GDALCreateMaskBand(GDALRasterBandH hBand,
                                              int nFlags);
bool CPL_DLL GDALIsMaskBand(GDALRasterBandH hBand);

/** Flag returned by GDALGetMaskFlags() to indicate that all pixels are valid */
#define GMF_ALL_VALID 0x01
/** Flag returned by GDALGetMaskFlags() to indicate that the mask band is
 * valid for all bands */
#define GMF_PER_DATASET 0x02
/** Flag returned by GDALGetMaskFlags() to indicate that the mask band is
 * an alpha band */
#define GMF_ALPHA 0x04
/** Flag returned by GDALGetMaskFlags() to indicate that the mask band is
 * computed from nodata values */
#define GMF_NODATA 0x08

/** Flag returned by GDALGetDataCoverageStatus() when the driver does not
 * implement GetDataCoverageStatus(). This flag should be returned together
 * with GDAL_DATA_COVERAGE_STATUS_DATA */
#define GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED 0x01

/** Flag returned by GDALGetDataCoverageStatus() when there is (potentially)
 * data in the queried window. Can be combined with the binary or operator
 * with GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED or
 * GDAL_DATA_COVERAGE_STATUS_EMPTY */
#define GDAL_DATA_COVERAGE_STATUS_DATA 0x02

/** Flag returned by GDALGetDataCoverageStatus() when there is nodata in the
 * queried window. This is typically identified by the concept of missing block
 * in formats that supports it.
 * Can be combined with the binary or operator with
 * GDAL_DATA_COVERAGE_STATUS_DATA */
#define GDAL_DATA_COVERAGE_STATUS_EMPTY 0x04

int CPL_DLL CPL_STDCALL GDALGetDataCoverageStatus(GDALRasterBandH hBand,
                                                  int nXOff, int nYOff,
                                                  int nXSize, int nYSize,
                                                  int nMaskFlagStop,
                                                  double *pdfDataPct);

void CPL_DLL GDALComputedRasterBandRelease(GDALComputedRasterBandH hBand);

/** Raster algebra unary operation */
typedef enum
{
    /** Logical not */
    GRAUO_LOGICAL_NOT,
    /** Absolute value (module for complex data type) */
    GRAUO_ABS,
    /** Square root */
    GRAUO_SQRT,
    /** Natural logarithm (``ln``) */
    GRAUO_LOG,
    /** Logarithm base 10 */
    GRAUO_LOG10,
} GDALRasterAlgebraUnaryOperation;

GDALComputedRasterBandH CPL_DLL GDALRasterBandUnaryOp(
    GDALRasterBandH hBand,
    GDALRasterAlgebraUnaryOperation eOp) CPL_WARN_UNUSED_RESULT;

/** Raster algebra binary operation */
typedef enum
{
    /** Addition */
    GRABO_ADD,
    /** Subtraction */
    GRABO_SUB,
    /** Multiplication */
    GRABO_MUL,
    /** Division */
    GRABO_DIV,
    /** Power */
    GRABO_POW,
    /** Strictly greater than test*/
    GRABO_GT,
    /** Greater or equal to test */
    GRABO_GE,
    /** Strictly lesser than test */
    GRABO_LT,
    /** Lesser or equal to test */
    GRABO_LE,
    /** Equality test */
    GRABO_EQ,
    /** Non-equality test */
    GRABO_NE,
    /** Logical and */
    GRABO_LOGICAL_AND,
    /** Logical or */
    GRABO_LOGICAL_OR
} GDALRasterAlgebraBinaryOperation;

GDALComputedRasterBandH CPL_DLL GDALRasterBandBinaryOpBand(
    GDALRasterBandH hBand, GDALRasterAlgebraBinaryOperation eOp,
    GDALRasterBandH hOtherBand) CPL_WARN_UNUSED_RESULT;
GDALComputedRasterBandH CPL_DLL GDALRasterBandBinaryOpDouble(
    GDALRasterBandH hBand, GDALRasterAlgebraBinaryOperation eOp,
    double constant) CPL_WARN_UNUSED_RESULT;
GDALComputedRasterBandH CPL_DLL GDALRasterBandBinaryOpDoubleToBand(
    double constant, GDALRasterAlgebraBinaryOperation eOp,
    GDALRasterBandH hBand) CPL_WARN_UNUSED_RESULT;

GDALComputedRasterBandH CPL_DLL
GDALRasterBandIfThenElse(GDALRasterBandH hCondBand, GDALRasterBandH hThenBand,
                         GDALRasterBandH hElseBand) CPL_WARN_UNUSED_RESULT;

GDALComputedRasterBandH CPL_DLL GDALRasterBandAsDataType(
    GDALRasterBandH hBand, GDALDataType eDT) CPL_WARN_UNUSED_RESULT;

GDALComputedRasterBandH CPL_DLL GDALMaximumOfNBands(
    size_t nBandCount, GDALRasterBandH *pahBands) CPL_WARN_UNUSED_RESULT;
GDALComputedRasterBandH CPL_DLL GDALRasterBandMaxConstant(
    GDALRasterBandH hBand, double dfConstant) CPL_WARN_UNUSED_RESULT;
GDALComputedRasterBandH CPL_DLL GDALMinimumOfNBands(
    size_t nBandCount, GDALRasterBandH *pahBands) CPL_WARN_UNUSED_RESULT;
GDALComputedRasterBandH CPL_DLL GDALRasterBandMinConstant(
    GDALRasterBandH hBand, double dfConstant) CPL_WARN_UNUSED_RESULT;
GDALComputedRasterBandH CPL_DLL GDALMeanOfNBands(
    size_t nBandCount, GDALRasterBandH *pahBands) CPL_WARN_UNUSED_RESULT;

/* ==================================================================== */
/*     GDALAsyncReader                                                  */
/* ==================================================================== */

GDALAsyncStatusType CPL_DLL CPL_STDCALL GDALARGetNextUpdatedRegion(
    GDALAsyncReaderH hARIO, double dfTimeout, int *pnXBufOff, int *pnYBufOff,
    int *pnXBufSize, int *pnYBufSize);
int CPL_DLL CPL_STDCALL GDALARLockBuffer(GDALAsyncReaderH hARIO,
                                         double dfTimeout);
void CPL_DLL CPL_STDCALL GDALARUnlockBuffer(GDALAsyncReaderH hARIO);

/* -------------------------------------------------------------------- */
/*      Helper functions.                                               */
/* -------------------------------------------------------------------- */
int CPL_DLL CPL_STDCALL GDALGeneralCmdLineProcessor(int nArgc,
                                                    char ***ppapszArgv,
                                                    int nOptions);
void CPL_DLL CPL_STDCALL GDALSwapWords(void *pData, int nWordSize,
                                       int nWordCount, int nWordSkip);
void CPL_DLL CPL_STDCALL GDALSwapWordsEx(void *pData, int nWordSize,
                                         size_t nWordCount, int nWordSkip);

void CPL_DLL CPL_STDCALL GDALCopyWords(const void *CPL_RESTRICT pSrcData,
                                       GDALDataType eSrcType,
                                       int nSrcPixelOffset,
                                       void *CPL_RESTRICT pDstData,
                                       GDALDataType eDstType,
                                       int nDstPixelOffset, int nWordCount);

void CPL_DLL CPL_STDCALL GDALCopyWords64(
    const void *CPL_RESTRICT pSrcData, GDALDataType eSrcType,
    int nSrcPixelOffset, void *CPL_RESTRICT pDstData, GDALDataType eDstType,
    int nDstPixelOffset, GPtrDiff_t nWordCount);

void CPL_DLL GDALCopyBits(const GByte *pabySrcData, int nSrcOffset,
                          int nSrcStep, GByte *pabyDstData, int nDstOffset,
                          int nDstStep, int nBitCount, int nStepCount);

void CPL_DLL GDALDeinterleave(const void *pSourceBuffer, GDALDataType eSourceDT,
                              int nComponents, void **ppDestBuffer,
                              GDALDataType eDestDT, size_t nIters);

void CPL_DLL GDALTranspose2D(const void *pSrc, GDALDataType eSrcType,
                             void *pDst, GDALDataType eDstType,
                             size_t nSrcWidth, size_t nSrcHeight);

double CPL_DLL GDALGetNoDataReplacementValue(GDALDataType, double);

int CPL_DLL CPL_STDCALL GDALLoadWorldFile(const char *, double *);
int CPL_DLL CPL_STDCALL GDALReadWorldFile(const char *, const char *, double *);
int CPL_DLL CPL_STDCALL GDALWriteWorldFile(const char *, const char *,
                                           double *);
int CPL_DLL CPL_STDCALL GDALLoadTabFile(const char *, double *, char **, int *,
                                        GDAL_GCP **);
int CPL_DLL CPL_STDCALL GDALReadTabFile(const char *, double *, char **, int *,
                                        GDAL_GCP **);
int CPL_DLL CPL_STDCALL GDALLoadOziMapFile(const char *, double *, char **,
                                           int *, GDAL_GCP **);
int CPL_DLL CPL_STDCALL GDALReadOziMapFile(const char *, double *, char **,
                                           int *, GDAL_GCP **);

const char CPL_DLL *CPL_STDCALL GDALDecToDMS(double, const char *, int);
double CPL_DLL CPL_STDCALL GDALPackedDMSToDec(double);
double CPL_DLL CPL_STDCALL GDALDecToPackedDMS(double);

/* Note to developers : please keep this section in sync with ogr_core.h */

#ifndef GDAL_VERSION_INFO_DEFINED
#ifndef DOXYGEN_SKIP
#define GDAL_VERSION_INFO_DEFINED
#endif
const char CPL_DLL *CPL_STDCALL GDALVersionInfo(const char *);
#endif

#ifndef GDAL_CHECK_VERSION

int CPL_DLL CPL_STDCALL GDALCheckVersion(int nVersionMajor, int nVersionMinor,
                                         const char *pszCallingComponentName);

/** Helper macro for GDALCheckVersion()
  @see GDALCheckVersion()
  */
#define GDAL_CHECK_VERSION(pszCallingComponentName)                            \
    GDALCheckVersion(GDAL_VERSION_MAJOR, GDAL_VERSION_MINOR,                   \
                     pszCallingComponentName)

#endif

/*! @cond Doxygen_Suppress */
#ifdef GDAL_COMPILATION
#define GDALExtractRPCInfoV1 GDALExtractRPCInfo
#else
#define GDALRPCInfo GDALRPCInfoV2
#define GDALExtractRPCInfo GDALExtractRPCInfoV2
#endif

/* Deprecated: use GDALRPCInfoV2 */
typedef struct
{
    double dfLINE_OFF;   /*!< Line offset */
    double dfSAMP_OFF;   /*!< Sample/Pixel offset */
    double dfLAT_OFF;    /*!< Latitude offset */
    double dfLONG_OFF;   /*!< Longitude offset */
    double dfHEIGHT_OFF; /*!< Height offset */

    double dfLINE_SCALE;   /*!< Line scale */
    double dfSAMP_SCALE;   /*!< Sample/Pixel scale */
    double dfLAT_SCALE;    /*!< Latitude scale */
    double dfLONG_SCALE;   /*!< Longitude scale */
    double dfHEIGHT_SCALE; /*!< Height scale */

    double adfLINE_NUM_COEFF[20]; /*!< Line Numerator Coefficients */
    double adfLINE_DEN_COEFF[20]; /*!< Line Denominator Coefficients */
    double adfSAMP_NUM_COEFF[20]; /*!< Sample/Pixel Numerator Coefficients */
    double adfSAMP_DEN_COEFF[20]; /*!< Sample/Pixel Denominator Coefficients */

    double dfMIN_LONG; /*!< Minimum longitude */
    double dfMIN_LAT;  /*!< Minimum latitude */
    double dfMAX_LONG; /*!< Maximum longitude */
    double dfMAX_LAT;  /*!< Maximum latitude */
} GDALRPCInfoV1;

/*! @endcond */

/** Structure to store Rational Polynomial Coefficients / Rigorous Projection
 * Model. See http://geotiff.maptools.org/rpc_prop.html */
typedef struct
{
    double dfLINE_OFF;   /*!< Line offset */
    double dfSAMP_OFF;   /*!< Sample/Pixel offset */
    double dfLAT_OFF;    /*!< Latitude offset */
    double dfLONG_OFF;   /*!< Longitude offset */
    double dfHEIGHT_OFF; /*!< Height offset */

    double dfLINE_SCALE;   /*!< Line scale */
    double dfSAMP_SCALE;   /*!< Sample/Pixel scale */
    double dfLAT_SCALE;    /*!< Latitude scale */
    double dfLONG_SCALE;   /*!< Longitude scale */
    double dfHEIGHT_SCALE; /*!< Height scale */

    double adfLINE_NUM_COEFF[20]; /*!< Line Numerator Coefficients */
    double adfLINE_DEN_COEFF[20]; /*!< Line Denominator Coefficients */
    double adfSAMP_NUM_COEFF[20]; /*!< Sample/Pixel Numerator Coefficients */
    double adfSAMP_DEN_COEFF[20]; /*!< Sample/Pixel Denominator Coefficients */

    double dfMIN_LONG; /*!< Minimum longitude */
    double dfMIN_LAT;  /*!< Minimum latitude */
    double dfMAX_LONG; /*!< Maximum longitude */
    double dfMAX_LAT;  /*!< Maximum latitude */

    /* Those fields should be at the end. And all above fields should be the
     * same as in GDALRPCInfoV1 */
    double dfERR_BIAS; /*!< Bias error */
    double dfERR_RAND; /*!< Random error */
} GDALRPCInfoV2;

/*! @cond Doxygen_Suppress */
int CPL_DLL CPL_STDCALL GDALExtractRPCInfoV1(CSLConstList, GDALRPCInfoV1 *);
/*! @endcond */
int CPL_DLL CPL_STDCALL GDALExtractRPCInfoV2(CSLConstList, GDALRPCInfoV2 *);

/* ==================================================================== */
/*      Color tables.                                                   */
/* ==================================================================== */

/** Color tuple */
typedef struct
{
    /*! gray, red, cyan or hue */
    short c1;

    /*! green, magenta, or lightness */
    short c2;

    /*! blue, yellow, or saturation */
    short c3;

    /*! alpha or blackband */
    short c4;
} GDALColorEntry;

GDALColorTableH CPL_DLL CPL_STDCALL GDALCreateColorTable(GDALPaletteInterp)
    CPL_WARN_UNUSED_RESULT;
void CPL_DLL CPL_STDCALL GDALDestroyColorTable(GDALColorTableH);
GDALColorTableH CPL_DLL CPL_STDCALL GDALCloneColorTable(GDALColorTableH);
GDALPaletteInterp
    CPL_DLL CPL_STDCALL GDALGetPaletteInterpretation(GDALColorTableH);
int CPL_DLL CPL_STDCALL GDALGetColorEntryCount(GDALColorTableH);
const GDALColorEntry CPL_DLL *CPL_STDCALL GDALGetColorEntry(GDALColorTableH,
                                                            int);
int CPL_DLL CPL_STDCALL GDALGetColorEntryAsRGB(GDALColorTableH, int,
                                               GDALColorEntry *);
void CPL_DLL CPL_STDCALL GDALSetColorEntry(GDALColorTableH, int,
                                           const GDALColorEntry *);
void CPL_DLL CPL_STDCALL GDALCreateColorRamp(GDALColorTableH hTable,
                                             int nStartIndex,
                                             const GDALColorEntry *psStartColor,
                                             int nEndIndex,
                                             const GDALColorEntry *psEndColor);

/* ==================================================================== */
/*      Raster Attribute Table                                          */
/* ==================================================================== */

/** Field type of raster attribute table */
typedef enum
{
    /*! Integer field */ GFT_Integer,
    /*! Floating point (double) field */ GFT_Real,
    /*! String field */ GFT_String
} GDALRATFieldType;

/** Field usage of raster attribute table */
typedef enum
{
    /*! General purpose field. */ GFU_Generic = 0,
    /*! Histogram pixel count */ GFU_PixelCount = 1,
    /*! Class name */ GFU_Name = 2,
    /*! Class range minimum */ GFU_Min = 3,
    /*! Class range maximum */ GFU_Max = 4,
    /*! Class value (min=max) */ GFU_MinMax = 5,
    /*! Red class color (0-255) */ GFU_Red = 6,
    /*! Green class color (0-255) */ GFU_Green = 7,
    /*! Blue class color (0-255) */ GFU_Blue = 8,
    /*! Alpha (0=transparent,255=opaque)*/ GFU_Alpha = 9,
    /*! Color Range Red Minimum */ GFU_RedMin = 10,
    /*! Color Range Green Minimum */ GFU_GreenMin = 11,
    /*! Color Range Blue Minimum */ GFU_BlueMin = 12,
    /*! Color Range Alpha Minimum */ GFU_AlphaMin = 13,
    /*! Color Range Red Maximum */ GFU_RedMax = 14,
    /*! Color Range Green Maximum */ GFU_GreenMax = 15,
    /*! Color Range Blue Maximum */ GFU_BlueMax = 16,
    /*! Color Range Alpha Maximum */ GFU_AlphaMax = 17,
    /*! Maximum GFU value (equals to GFU_AlphaMax+1 currently) */ GFU_MaxCount
} GDALRATFieldUsage;

/** RAT table type (thematic or athematic)
 * @since GDAL 2.4
 */
typedef enum
{
    /*! Thematic table type */ GRTT_THEMATIC,
    /*! Athematic table type */ GRTT_ATHEMATIC
} GDALRATTableType;

GDALRasterAttributeTableH CPL_DLL CPL_STDCALL
GDALCreateRasterAttributeTable(void) CPL_WARN_UNUSED_RESULT;

void CPL_DLL CPL_STDCALL
    GDALDestroyRasterAttributeTable(GDALRasterAttributeTableH);

int CPL_DLL CPL_STDCALL GDALRATGetColumnCount(GDALRasterAttributeTableH);

const char CPL_DLL *CPL_STDCALL GDALRATGetNameOfCol(GDALRasterAttributeTableH,
                                                    int);
GDALRATFieldUsage CPL_DLL CPL_STDCALL
GDALRATGetUsageOfCol(GDALRasterAttributeTableH, int);
GDALRATFieldType CPL_DLL CPL_STDCALL
GDALRATGetTypeOfCol(GDALRasterAttributeTableH, int);

int CPL_DLL CPL_STDCALL GDALRATGetColOfUsage(GDALRasterAttributeTableH,
                                             GDALRATFieldUsage);
int CPL_DLL CPL_STDCALL GDALRATGetRowCount(GDALRasterAttributeTableH);

const char CPL_DLL *CPL_STDCALL
GDALRATGetValueAsString(GDALRasterAttributeTableH, int, int);
int CPL_DLL CPL_STDCALL GDALRATGetValueAsInt(GDALRasterAttributeTableH, int,
                                             int);
double CPL_DLL CPL_STDCALL GDALRATGetValueAsDouble(GDALRasterAttributeTableH,
                                                   int, int);

void CPL_DLL CPL_STDCALL GDALRATSetValueAsString(GDALRasterAttributeTableH, int,
                                                 int, const char *);
void CPL_DLL CPL_STDCALL GDALRATSetValueAsInt(GDALRasterAttributeTableH, int,
                                              int, int);
void CPL_DLL CPL_STDCALL GDALRATSetValueAsDouble(GDALRasterAttributeTableH, int,
                                                 int, double);

int CPL_DLL CPL_STDCALL
GDALRATChangesAreWrittenToFile(GDALRasterAttributeTableH hRAT);

CPLErr CPL_DLL CPL_STDCALL GDALRATValuesIOAsDouble(
    GDALRasterAttributeTableH hRAT, GDALRWFlag eRWFlag, int iField,
    int iStartRow, int iLength, double *pdfData);
CPLErr CPL_DLL CPL_STDCALL
GDALRATValuesIOAsInteger(GDALRasterAttributeTableH hRAT, GDALRWFlag eRWFlag,
                         int iField, int iStartRow, int iLength, int *pnData);
CPLErr CPL_DLL CPL_STDCALL GDALRATValuesIOAsString(
    GDALRasterAttributeTableH hRAT, GDALRWFlag eRWFlag, int iField,
    int iStartRow, int iLength, char **papszStrList);

void CPL_DLL CPL_STDCALL GDALRATSetRowCount(GDALRasterAttributeTableH, int);
CPLErr CPL_DLL CPL_STDCALL GDALRATCreateColumn(GDALRasterAttributeTableH,
                                               const char *, GDALRATFieldType,
                                               GDALRATFieldUsage);
CPLErr CPL_DLL CPL_STDCALL GDALRATSetLinearBinning(GDALRasterAttributeTableH,
                                                   double, double);
int CPL_DLL CPL_STDCALL GDALRATGetLinearBinning(GDALRasterAttributeTableH,
                                                double *, double *);
CPLErr CPL_DLL CPL_STDCALL GDALRATSetTableType(
    GDALRasterAttributeTableH hRAT, const GDALRATTableType eInTableType);
GDALRATTableType CPL_DLL CPL_STDCALL
GDALRATGetTableType(GDALRasterAttributeTableH hRAT);
CPLErr CPL_DLL CPL_STDCALL
    GDALRATInitializeFromColorTable(GDALRasterAttributeTableH, GDALColorTableH);
GDALColorTableH CPL_DLL CPL_STDCALL
GDALRATTranslateToColorTable(GDALRasterAttributeTableH, int nEntryCount);
void CPL_DLL CPL_STDCALL GDALRATDumpReadable(GDALRasterAttributeTableH, FILE *);
GDALRasterAttributeTableH CPL_DLL CPL_STDCALL
GDALRATClone(const GDALRasterAttributeTableH);

void CPL_DLL *CPL_STDCALL GDALRATSerializeJSON(GDALRasterAttributeTableH)
    CPL_WARN_UNUSED_RESULT;

int CPL_DLL CPL_STDCALL GDALRATGetRowOfValue(GDALRasterAttributeTableH, double);
void CPL_DLL CPL_STDCALL GDALRATRemoveStatistics(GDALRasterAttributeTableH);

/* -------------------------------------------------------------------- */
/*                          Relationships                               */
/* -------------------------------------------------------------------- */

/** Cardinality of relationship.
 *
 * @since GDAL 3.6
 */
typedef enum
{
    /** One-to-one */
    GRC_ONE_TO_ONE,
    /** One-to-many */
    GRC_ONE_TO_MANY,
    /** Many-to-one */
    GRC_MANY_TO_ONE,
    /** Many-to-many */
    GRC_MANY_TO_MANY,
} GDALRelationshipCardinality;

/** Type of relationship.
 *
 * @since GDAL 3.6
 */
typedef enum
{
    /** Composite relationship */
    GRT_COMPOSITE,
    /** Association relationship */
    GRT_ASSOCIATION,
    /** Aggregation relationship */
    GRT_AGGREGATION,
} GDALRelationshipType;

GDALRelationshipH CPL_DLL GDALRelationshipCreate(const char *, const char *,
                                                 const char *,
                                                 GDALRelationshipCardinality);
void CPL_DLL CPL_STDCALL GDALDestroyRelationship(GDALRelationshipH);
const char CPL_DLL *GDALRelationshipGetName(GDALRelationshipH);
GDALRelationshipCardinality
    CPL_DLL GDALRelationshipGetCardinality(GDALRelationshipH);
const char CPL_DLL *GDALRelationshipGetLeftTableName(GDALRelationshipH);
const char CPL_DLL *GDALRelationshipGetRightTableName(GDALRelationshipH);
const char CPL_DLL *GDALRelationshipGetMappingTableName(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetMappingTableName(GDALRelationshipH,
                                                 const char *);
char CPL_DLL **GDALRelationshipGetLeftTableFields(GDALRelationshipH);
char CPL_DLL **GDALRelationshipGetRightTableFields(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetLeftTableFields(GDALRelationshipH,
                                                CSLConstList);
void CPL_DLL GDALRelationshipSetRightTableFields(GDALRelationshipH,
                                                 CSLConstList);
char CPL_DLL **GDALRelationshipGetLeftMappingTableFields(GDALRelationshipH);
char CPL_DLL **GDALRelationshipGetRightMappingTableFields(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetLeftMappingTableFields(GDALRelationshipH,
                                                       CSLConstList);
void CPL_DLL GDALRelationshipSetRightMappingTableFields(GDALRelationshipH,
                                                        CSLConstList);
GDALRelationshipType CPL_DLL GDALRelationshipGetType(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetType(GDALRelationshipH, GDALRelationshipType);
const char CPL_DLL *GDALRelationshipGetForwardPathLabel(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetForwardPathLabel(GDALRelationshipH,
                                                 const char *);
const char CPL_DLL *GDALRelationshipGetBackwardPathLabel(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetBackwardPathLabel(GDALRelationshipH,
                                                  const char *);
const char CPL_DLL *GDALRelationshipGetRelatedTableType(GDALRelationshipH);
void CPL_DLL GDALRelationshipSetRelatedTableType(GDALRelationshipH,
                                                 const char *);

/* ==================================================================== */
/*      GDAL Cache Management                                           */
/* ==================================================================== */

void CPL_DLL CPL_STDCALL GDALSetCacheMax(int nBytes);
int CPL_DLL CPL_STDCALL GDALGetCacheMax(void);
int CPL_DLL CPL_STDCALL GDALGetCacheUsed(void);
void CPL_DLL CPL_STDCALL GDALSetCacheMax64(GIntBig nBytes);
GIntBig CPL_DLL CPL_STDCALL GDALGetCacheMax64(void);
GIntBig CPL_DLL CPL_STDCALL GDALGetCacheUsed64(void);

int CPL_DLL CPL_STDCALL GDALFlushCacheBlock(void);

/* ==================================================================== */
/*      GDAL virtual memory                                             */
/* ==================================================================== */

CPLVirtualMem CPL_DLL *GDALDatasetGetVirtualMem(
    GDALDatasetH hDS, GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
    int nYSize, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    int nBandCount, int *panBandMap, int nPixelSpace, GIntBig nLineSpace,
    GIntBig nBandSpace, size_t nCacheSize, size_t nPageSizeHint,
    int bSingleThreadUsage, CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

CPLVirtualMem CPL_DLL *GDALRasterBandGetVirtualMem(
    GDALRasterBandH hBand, GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
    int nYSize, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    int nPixelSpace, GIntBig nLineSpace, size_t nCacheSize,
    size_t nPageSizeHint, int bSingleThreadUsage,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

CPLVirtualMem CPL_DLL *
GDALGetVirtualMemAuto(GDALRasterBandH hBand, GDALRWFlag eRWFlag,
                      int *pnPixelSpace, GIntBig *pnLineSpace,
                      CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

/**! Enumeration to describe the tile organization */
typedef enum
{
    /*! Tile Interleaved by Pixel: tile (0,0) with internal band interleaved by
       pixel organization, tile (1, 0), ...  */
    GTO_TIP,
    /*! Band Interleaved by Tile : tile (0,0) of first band, tile (0,0) of
       second band, ... tile (1,0) of first band, tile (1,0) of second band, ...
     */
    GTO_BIT,
    /*! Band SeQuential : all the tiles of first band, all the tiles of
       following band... */
    GTO_BSQ
} GDALTileOrganization;

CPLVirtualMem CPL_DLL *GDALDatasetGetTiledVirtualMem(
    GDALDatasetH hDS, GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
    int nYSize, int nTileXSize, int nTileYSize, GDALDataType eBufType,
    int nBandCount, int *panBandMap, GDALTileOrganization eTileOrganization,
    size_t nCacheSize, int bSingleThreadUsage,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

CPLVirtualMem CPL_DLL *GDALRasterBandGetTiledVirtualMem(
    GDALRasterBandH hBand, GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
    int nYSize, int nTileXSize, int nTileYSize, GDALDataType eBufType,
    size_t nCacheSize, int bSingleThreadUsage,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

/* ==================================================================== */
/*      VRTPansharpenedDataset class.                                   */
/* ==================================================================== */

GDALDatasetH CPL_DLL GDALCreatePansharpenedVRT(
    const char *pszXML, GDALRasterBandH hPanchroBand, int nInputSpectralBands,
    GDALRasterBandH *pahInputSpectralBands) CPL_WARN_UNUSED_RESULT;

/* =================================================================== */
/*      Misc API                                                        */
/* ==================================================================== */

CPLXMLNode CPL_DLL *
GDALGetJPEG2000Structure(const char *pszFilename,
                         CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

/* ==================================================================== */
/*      Multidimensional API_api                                       */
/* ==================================================================== */

GDALDatasetH CPL_DLL
GDALCreateMultiDimensional(GDALDriverH hDriver, const char *pszName,
                           CSLConstList papszRootGroupOptions,
                           CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

GDALExtendedDataTypeH CPL_DLL GDALExtendedDataTypeCreate(GDALDataType eType)
    CPL_WARN_UNUSED_RESULT;
GDALExtendedDataTypeH CPL_DLL GDALExtendedDataTypeCreateString(
    size_t nMaxStringLength) CPL_WARN_UNUSED_RESULT;
GDALExtendedDataTypeH CPL_DLL GDALExtendedDataTypeCreateStringEx(
    size_t nMaxStringLength,
    GDALExtendedDataTypeSubType eSubType) CPL_WARN_UNUSED_RESULT;
GDALExtendedDataTypeH CPL_DLL GDALExtendedDataTypeCreateCompound(
    const char *pszName, size_t nTotalSize, size_t nComponents,
    const GDALEDTComponentH *comps) CPL_WARN_UNUSED_RESULT;
void CPL_DLL GDALExtendedDataTypeRelease(GDALExtendedDataTypeH hEDT);
const char CPL_DLL *GDALExtendedDataTypeGetName(GDALExtendedDataTypeH hEDT);
GDALExtendedDataTypeClass CPL_DLL
GDALExtendedDataTypeGetClass(GDALExtendedDataTypeH hEDT);
GDALDataType CPL_DLL
GDALExtendedDataTypeGetNumericDataType(GDALExtendedDataTypeH hEDT);
size_t CPL_DLL GDALExtendedDataTypeGetSize(GDALExtendedDataTypeH hEDT);
size_t CPL_DLL
GDALExtendedDataTypeGetMaxStringLength(GDALExtendedDataTypeH hEDT);
GDALEDTComponentH CPL_DLL *
GDALExtendedDataTypeGetComponents(GDALExtendedDataTypeH hEDT,
                                  size_t *pnCount) CPL_WARN_UNUSED_RESULT;
void CPL_DLL GDALExtendedDataTypeFreeComponents(GDALEDTComponentH *components,
                                                size_t nCount);
int CPL_DLL GDALExtendedDataTypeCanConvertTo(GDALExtendedDataTypeH hSourceEDT,
                                             GDALExtendedDataTypeH hTargetEDT);
int CPL_DLL GDALExtendedDataTypeEquals(GDALExtendedDataTypeH hFirstEDT,
                                       GDALExtendedDataTypeH hSecondEDT);
GDALExtendedDataTypeSubType CPL_DLL
GDALExtendedDataTypeGetSubType(GDALExtendedDataTypeH hEDT);
GDALRasterAttributeTableH CPL_DLL
GDALExtendedDataTypeGetRAT(GDALExtendedDataTypeH hEDT) CPL_WARN_UNUSED_RESULT;

GDALEDTComponentH CPL_DLL
GDALEDTComponentCreate(const char *pszName, size_t nOffset,
                       GDALExtendedDataTypeH hType) CPL_WARN_UNUSED_RESULT;
void CPL_DLL GDALEDTComponentRelease(GDALEDTComponentH hComp);
const char CPL_DLL *GDALEDTComponentGetName(GDALEDTComponentH hComp);
size_t CPL_DLL GDALEDTComponentGetOffset(GDALEDTComponentH hComp);
GDALExtendedDataTypeH CPL_DLL GDALEDTComponentGetType(GDALEDTComponentH hComp)
    CPL_WARN_UNUSED_RESULT;

GDALGroupH CPL_DLL GDALDatasetGetRootGroup(GDALDatasetH hDS)
    CPL_WARN_UNUSED_RESULT;
void CPL_DLL GDALGroupRelease(GDALGroupH hGroup);
const char CPL_DLL *GDALGroupGetName(GDALGroupH hGroup);
const char CPL_DLL *GDALGroupGetFullName(GDALGroupH hGroup);
char CPL_DLL **
GDALGroupGetMDArrayNames(GDALGroupH hGroup,
                         CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
char CPL_DLL **GDALGroupGetMDArrayFullNamesRecursive(
    GDALGroupH hGroup, CSLConstList papszGroupOptions,
    CSLConstList papszArrayOptions) CPL_WARN_UNUSED_RESULT;
GDALMDArrayH CPL_DLL
GDALGroupOpenMDArray(GDALGroupH hGroup, const char *pszMDArrayName,
                     CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALMDArrayH CPL_DLL GDALGroupOpenMDArrayFromFullname(
    GDALGroupH hGroup, const char *pszMDArrayName,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALMDArrayH CPL_DLL GDALGroupResolveMDArray(
    GDALGroupH hGroup, const char *pszName, const char *pszStartingPoint,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
char CPL_DLL **
GDALGroupGetGroupNames(GDALGroupH hGroup,
                       CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALGroupH CPL_DLL
GDALGroupOpenGroup(GDALGroupH hGroup, const char *pszSubGroupName,
                   CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALGroupH CPL_DLL GDALGroupOpenGroupFromFullname(
    GDALGroupH hGroup, const char *pszMDArrayName,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
char CPL_DLL **
GDALGroupGetVectorLayerNames(GDALGroupH hGroup,
                             CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
OGRLayerH CPL_DLL
GDALGroupOpenVectorLayer(GDALGroupH hGroup, const char *pszVectorLayerName,
                         CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALDimensionH CPL_DLL *
GDALGroupGetDimensions(GDALGroupH hGroup, size_t *pnCount,
                       CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALAttributeH CPL_DLL GDALGroupGetAttribute(
    GDALGroupH hGroup, const char *pszName) CPL_WARN_UNUSED_RESULT;
GDALAttributeH CPL_DLL *
GDALGroupGetAttributes(GDALGroupH hGroup, size_t *pnCount,
                       CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
CSLConstList CPL_DLL GDALGroupGetStructuralInfo(GDALGroupH hGroup);
GDALGroupH CPL_DLL
GDALGroupCreateGroup(GDALGroupH hGroup, const char *pszSubGroupName,
                     CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
bool CPL_DLL GDALGroupDeleteGroup(GDALGroupH hGroup, const char *pszName,
                                  CSLConstList papszOptions);
GDALDimensionH CPL_DLL GDALGroupCreateDimension(
    GDALGroupH hGroup, const char *pszName, const char *pszType,
    const char *pszDirection, GUInt64 nSize,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALMDArrayH CPL_DLL GDALGroupCreateMDArray(
    GDALGroupH hGroup, const char *pszName, size_t nDimensions,
    GDALDimensionH *pahDimensions, GDALExtendedDataTypeH hEDT,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
bool CPL_DLL GDALGroupDeleteMDArray(GDALGroupH hGroup, const char *pszName,
                                    CSLConstList papszOptions);
GDALAttributeH CPL_DLL GDALGroupCreateAttribute(
    GDALGroupH hGroup, const char *pszName, size_t nDimensions,
    const GUInt64 *panDimensions, GDALExtendedDataTypeH hEDT,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
bool CPL_DLL GDALGroupDeleteAttribute(GDALGroupH hGroup, const char *pszName,
                                      CSLConstList papszOptions);
bool CPL_DLL GDALGroupRename(GDALGroupH hGroup, const char *pszNewName);
GDALGroupH CPL_DLL GDALGroupSubsetDimensionFromSelection(
    GDALGroupH hGroup, const char *pszSelection, CSLConstList papszOptions);
size_t CPL_DLL GDALGroupGetDataTypeCount(GDALGroupH hGroup);
GDALExtendedDataTypeH CPL_DLL GDALGroupGetDataType(GDALGroupH hGroup,
                                                   size_t nIdx);

void CPL_DLL GDALMDArrayRelease(GDALMDArrayH hMDArray);
const char CPL_DLL *GDALMDArrayGetName(GDALMDArrayH hArray);
const char CPL_DLL *GDALMDArrayGetFullName(GDALMDArrayH hArray);
GUInt64 CPL_DLL GDALMDArrayGetTotalElementsCount(GDALMDArrayH hArray);
size_t CPL_DLL GDALMDArrayGetDimensionCount(GDALMDArrayH hArray);
GDALDimensionH CPL_DLL *
GDALMDArrayGetDimensions(GDALMDArrayH hArray,
                         size_t *pnCount) CPL_WARN_UNUSED_RESULT;
GDALExtendedDataTypeH CPL_DLL GDALMDArrayGetDataType(GDALMDArrayH hArray)
    CPL_WARN_UNUSED_RESULT;
int CPL_DLL GDALMDArrayRead(GDALMDArrayH hArray, const GUInt64 *arrayStartIdx,
                            const size_t *count, const GInt64 *arrayStep,
                            const GPtrDiff_t *bufferStride,
                            GDALExtendedDataTypeH bufferDatatype,
                            void *pDstBuffer, const void *pDstBufferAllocStart,
                            size_t nDstBufferllocSize);
int CPL_DLL GDALMDArrayWrite(GDALMDArrayH hArray, const GUInt64 *arrayStartIdx,
                             const size_t *count, const GInt64 *arrayStep,
                             const GPtrDiff_t *bufferStride,
                             GDALExtendedDataTypeH bufferDatatype,
                             const void *pSrcBuffer,
                             const void *psrcBufferAllocStart,
                             size_t nSrcBufferllocSize);
int CPL_DLL GDALMDArrayAdviseRead(GDALMDArrayH hArray,
                                  const GUInt64 *arrayStartIdx,
                                  const size_t *count);
int CPL_DLL GDALMDArrayAdviseReadEx(GDALMDArrayH hArray,
                                    const GUInt64 *arrayStartIdx,
                                    const size_t *count,
                                    CSLConstList papszOptions);
GDALAttributeH CPL_DLL GDALMDArrayGetAttribute(
    GDALMDArrayH hArray, const char *pszName) CPL_WARN_UNUSED_RESULT;
GDALAttributeH CPL_DLL *
GDALMDArrayGetAttributes(GDALMDArrayH hArray, size_t *pnCount,
                         CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
GDALAttributeH CPL_DLL GDALMDArrayCreateAttribute(
    GDALMDArrayH hArray, const char *pszName, size_t nDimensions,
    const GUInt64 *panDimensions, GDALExtendedDataTypeH hEDT,
    CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;
bool CPL_DLL GDALMDArrayDeleteAttribute(GDALMDArrayH hArray,
                                        const char *pszName,
                                        CSLConstList papszOptions);
bool CPL_DLL GDALMDArrayResize(GDALMDArrayH hArray,
                               const GUInt64 *panNewDimSizes,
                               CSLConstList papszOptions);
const void CPL_DLL *GDALMDArrayGetRawNoDataValue(GDALMDArrayH hArray);
double CPL_DLL GDALMDArrayGetNoDataValueAsDouble(GDALMDArrayH hArray,
                                                 int *pbHasNoDataValue);
int64_t CPL_DLL GDALMDArrayGetNoDataValueAsInt64(GDALMDArrayH hArray,
                                                 int *pbHasNoDataValue);
uint64_t CPL_DLL GDALMDArrayGetNoDataValueAsUInt64(GDALMDArrayH hArray,
                                                   int *pbHasNoDataValue);
int CPL_DLL GDALMDArraySetRawNoDataValue(GDALMDArrayH hArray, const void *);
int CPL_DLL GDALMDArraySetNoDataValueAsDouble(GDALMDArrayH hArray,
                                              double dfNoDataValue);
int CPL_DLL GDALMDArraySetNoDataValueAsInt64(GDALMDArrayH hArray,
                                             int64_t nNoDataValue);
int CPL_DLL GDALMDArraySetNoDataValueAsUInt64(GDALMDArrayH hArray,
                                              uint64_t nNoDataValue);
int CPL_DLL GDALMDArraySetScale(GDALMDArrayH hArray, double dfScale);
int CPL_DLL GDALMDArraySetScaleEx(GDALMDArrayH hArray, double dfScale,
                                  GDALDataType eStorageType);
double CPL_DLL GDALMDArrayGetScale(GDALMDArrayH hArray, int *pbHasValue);
double CPL_DLL GDALMDArrayGetScaleEx(GDALMDArrayH hArray, int *pbHasValue,
                                     GDALDataType *peStorageType);
int CPL_DLL GDALMDArraySetOffset(GDALMDArrayH hArray, double dfOffset);
int CPL_DLL GDALMDArraySetOffsetEx(GDALMDArrayH hArray, double dfOffset,
                                   GDALDataType eStorageType);
double CPL_DLL GDALMDArrayGetOffset(GDALMDArrayH hArray, int *pbHasValue);
double CPL_DLL GDALMDArrayGetOffsetEx(GDALMDArrayH hArray, int *pbHasValue,
                                      GDALDataType *peStorageType);
GUInt64 CPL_DLL *GDALMDArrayGetBlockSize(GDALMDArrayH hArray, size_t *pnCount);
int CPL_DLL GDALMDArraySetUnit(GDALMDArrayH hArray, const char *);
const char CPL_DLL *GDALMDArrayGetUnit(GDALMDArrayH hArray);
int CPL_DLL GDALMDArraySetSpatialRef(GDALMDArrayH, OGRSpatialReferenceH);
OGRSpatialReferenceH CPL_DLL GDALMDArrayGetSpatialRef(GDALMDArrayH hArray);
size_t CPL_DLL *GDALMDArrayGetProcessingChunkSize(GDALMDArrayH hArray,
                                                  size_t *pnCount,
                                                  size_t nMaxChunkMemory);
CSLConstList CPL_DLL GDALMDArrayGetStructuralInfo(GDALMDArrayH hArray);
GDALMDArrayH CPL_DLL GDALMDArrayGetView(GDALMDArrayH hArray,
                                        const char *pszViewExpr);
GDALMDArrayH CPL_DLL GDALMDArrayTranspose(GDALMDArrayH hArray,
                                          size_t nNewAxisCount,
                                          const int *panMapNewAxisToOldAxis);
GDALMDArrayH CPL_DLL GDALMDArrayGetUnscaled(GDALMDArrayH hArray);
GDALMDArrayH CPL_DLL GDALMDArrayGetMask(GDALMDArrayH hArray,
                                        CSLConstList papszOptions);
GDALDatasetH CPL_DLL GDALMDArrayAsClassicDataset(GDALMDArrayH hArray,
                                                 size_t iXDim, size_t iYDim);
GDALDatasetH CPL_DLL GDALMDArrayAsClassicDatasetEx(GDALMDArrayH hArray,
                                                   size_t iXDim, size_t iYDim,
                                                   GDALGroupH hRootGroup,
                                                   CSLConstList papszOptions);
CPLErr CPL_DLL GDALMDArrayGetStatistics(
    GDALMDArrayH hArray, GDALDatasetH, int bApproxOK, int bForce,
    double *pdfMin, double *pdfMax, double *pdfMean, double *pdfStdDev,
    GUInt64 *pnValidCount, GDALProgressFunc pfnProgress, void *pProgressData);
int CPL_DLL GDALMDArrayComputeStatistics(GDALMDArrayH hArray, GDALDatasetH,
                                         int bApproxOK, double *pdfMin,
                                         double *pdfMax, double *pdfMean,
                                         double *pdfStdDev,
                                         GUInt64 *pnValidCount,
                                         GDALProgressFunc, void *pProgressData);
int CPL_DLL GDALMDArrayComputeStatisticsEx(
    GDALMDArrayH hArray, GDALDatasetH, int bApproxOK, double *pdfMin,
    double *pdfMax, double *pdfMean, double *pdfStdDev, GUInt64 *pnValidCount,
    GDALProgressFunc, void *pProgressData, CSLConstList papszOptions);
GDALMDArrayH CPL_DLL GDALMDArrayGetResampled(GDALMDArrayH hArray,
                                             size_t nNewDimCount,
                                             const GDALDimensionH *pahNewDims,
                                             GDALRIOResampleAlg resampleAlg,
                                             OGRSpatialReferenceH hTargetSRS,
                                             CSLConstList papszOptions);
GDALMDArrayH CPL_DLL GDALMDArrayGetGridded(
    GDALMDArrayH hArray, const char *pszGridOptions, GDALMDArrayH hXArray,
    GDALMDArrayH hYArray, CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

GDALMDArrayH CPL_DLL *
GDALMDArrayGetCoordinateVariables(GDALMDArrayH hArray,
                                  size_t *pnCount) CPL_WARN_UNUSED_RESULT;

GDALMDArrayH CPL_DLL *
GDALMDArrayGetMeshGrid(const GDALMDArrayH *pahInputArrays,
                       size_t nCountInputArrays, size_t *pnCountOutputArrays,
                       CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

void CPL_DLL GDALReleaseArrays(GDALMDArrayH *arrays, size_t nCount);
int CPL_DLL GDALMDArrayCache(GDALMDArrayH hArray, CSLConstList papszOptions);
bool CPL_DLL GDALMDArrayRename(GDALMDArrayH hArray, const char *pszNewName);

GDALRasterAttributeTableH CPL_DLL GDALCreateRasterAttributeTableFromMDArrays(
    GDALRATTableType eTableType, int nArrays, const GDALMDArrayH *ahArrays,
    const GDALRATFieldUsage *paeUsages);

void CPL_DLL GDALAttributeRelease(GDALAttributeH hAttr);
void CPL_DLL GDALReleaseAttributes(GDALAttributeH *attributes, size_t nCount);
const char CPL_DLL *GDALAttributeGetName(GDALAttributeH hAttr);
const char CPL_DLL *GDALAttributeGetFullName(GDALAttributeH hAttr);
GUInt64 CPL_DLL GDALAttributeGetTotalElementsCount(GDALAttributeH hAttr);
size_t CPL_DLL GDALAttributeGetDimensionCount(GDALAttributeH hAttr);
GUInt64 CPL_DLL *
GDALAttributeGetDimensionsSize(GDALAttributeH hAttr,
                               size_t *pnCount) CPL_WARN_UNUSED_RESULT;
GDALExtendedDataTypeH CPL_DLL GDALAttributeGetDataType(GDALAttributeH hAttr)
    CPL_WARN_UNUSED_RESULT;
GByte CPL_DLL *GDALAttributeReadAsRaw(GDALAttributeH hAttr,
                                      size_t *pnSize) CPL_WARN_UNUSED_RESULT;
void CPL_DLL GDALAttributeFreeRawResult(GDALAttributeH hAttr, GByte *raw,
                                        size_t nSize);
const char CPL_DLL *GDALAttributeReadAsString(GDALAttributeH hAttr);
int CPL_DLL GDALAttributeReadAsInt(GDALAttributeH hAttr);
int64_t CPL_DLL GDALAttributeReadAsInt64(GDALAttributeH hAttr);
double CPL_DLL GDALAttributeReadAsDouble(GDALAttributeH hAttr);
char CPL_DLL **
GDALAttributeReadAsStringArray(GDALAttributeH hAttr) CPL_WARN_UNUSED_RESULT;
int CPL_DLL *GDALAttributeReadAsIntArray(GDALAttributeH hAttr, size_t *pnCount)
    CPL_WARN_UNUSED_RESULT;
int64_t CPL_DLL *
GDALAttributeReadAsInt64Array(GDALAttributeH hAttr,
                              size_t *pnCount) CPL_WARN_UNUSED_RESULT;
double CPL_DLL *
GDALAttributeReadAsDoubleArray(GDALAttributeH hAttr,
                               size_t *pnCount) CPL_WARN_UNUSED_RESULT;
int CPL_DLL GDALAttributeWriteRaw(GDALAttributeH hAttr, const void *, size_t);
int CPL_DLL GDALAttributeWriteString(GDALAttributeH hAttr, const char *);
int CPL_DLL GDALAttributeWriteStringArray(GDALAttributeH hAttr, CSLConstList);
int CPL_DLL GDALAttributeWriteInt(GDALAttributeH hAttr, int);
int CPL_DLL GDALAttributeWriteIntArray(GDALAttributeH hAttr, const int *,
                                       size_t);
int CPL_DLL GDALAttributeWriteInt64(GDALAttributeH hAttr, int64_t);
int CPL_DLL GDALAttributeWriteInt64Array(GDALAttributeH hAttr, const int64_t *,
                                         size_t);
int CPL_DLL GDALAttributeWriteDouble(GDALAttributeH hAttr, double);
int CPL_DLL GDALAttributeWriteDoubleArray(GDALAttributeH hAttr, const double *,
                                          size_t);
bool CPL_DLL GDALAttributeRename(GDALAttributeH hAttr, const char *pszNewName);

void CPL_DLL GDALDimensionRelease(GDALDimensionH hDim);
void CPL_DLL GDALReleaseDimensions(GDALDimensionH *dims, size_t nCount);
const char CPL_DLL *GDALDimensionGetName(GDALDimensionH hDim);
const char CPL_DLL *GDALDimensionGetFullName(GDALDimensionH hDim);
const char CPL_DLL *GDALDimensionGetType(GDALDimensionH hDim);
const char CPL_DLL *GDALDimensionGetDirection(GDALDimensionH hDim);
GUInt64 CPL_DLL GDALDimensionGetSize(GDALDimensionH hDim);
GDALMDArrayH CPL_DLL GDALDimensionGetIndexingVariable(GDALDimensionH hDim)
    CPL_WARN_UNUSED_RESULT;
int CPL_DLL GDALDimensionSetIndexingVariable(GDALDimensionH hDim,
                                             GDALMDArrayH hArray);
bool CPL_DLL GDALDimensionRename(GDALDimensionH hDim, const char *pszNewName);

CPL_C_END

#endif /* ndef GDAL_H_INCLUDED */
