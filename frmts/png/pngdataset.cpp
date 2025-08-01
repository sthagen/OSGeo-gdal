/******************************************************************************
 *
 * Project:  PNG Driver
 * Purpose:  Implement GDAL PNG Support
 * Author:   Frank Warmerdam, warmerda@home.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************
 *
 * ISSUES:
 *  o CollectMetadata() will only capture TEXT chunks before the image
 *    data as the code is currently structured.
 *  o Interlaced images are read entirely into memory for use.  This is
 *    bad for large images.
 *  o Image reading is always strictly sequential.  Reading backwards will
 *    cause the file to be rewound, and access started again from the
 *    beginning.
 *  o 16 bit alpha values are not scaled by to eight bit.
 *
 */

#include "pngdataset.h"
#include "pngdrivercore.h"

#include "cpl_string.h"
#include "cpl_vsi_virtual.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
#endif

#include "png.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <csetjmp>

#include <algorithm>
#include <limits>

// Note: Callers must provide blocks in increasing Y order.
// Disclaimer (E. Rouault): this code is not production ready at all. A lot of
// issues remain: uninitialized variables, unclosed files, lack of proper
// multiband handling, and an inability to read and write at the same time. Do
// not use it unless you're ready to fix it.

// Define SUPPORT_CREATE to enable use of the Create() call.
// #define SUPPORT_CREATE

#ifdef _MSC_VER
#pragma warning(disable : 4611)
#endif

static void png_vsi_read_data(png_structp png_ptr, png_bytep data,
                              png_size_t length);

static void png_vsi_write_data(png_structp png_ptr, png_bytep data,
                               png_size_t length);

static void png_vsi_flush(png_structp png_ptr);

static void png_gdal_error(png_structp png_ptr, const char *error_message);
static void png_gdal_warning(png_structp png_ptr, const char *error_message);

#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION

/************************************************************************/
/*                      IsCompatibleOfSingleBlock()                     */
/************************************************************************/

bool PNGDataset::IsCompatibleOfSingleBlock() const
{
    return nBitDepth == 8 && !bInterlaced && nRasterXSize <= 512 &&
           nRasterYSize <= 512 &&
           CPLTestBool(
               CPLGetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "YES")) &&
           CPLTestBool(CPLGetConfigOption("GDAL_PNG_SINGLE_BLOCK", "YES"));
}
#endif

/************************************************************************/
/*                           PNGRasterBand()                            */
/************************************************************************/

PNGRasterBand::PNGRasterBand(PNGDataset *poDSIn, int nBandIn)
    : bHaveNoData(FALSE), dfNoDataValue(-1)
{
    poDS = poDSIn;
    nBand = nBandIn;

    if (poDSIn->nBitDepth == 16)
        eDataType = GDT_UInt16;
    else
        eDataType = GDT_Byte;

    nBlockXSize = poDSIn->nRasterXSize;
#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION
    if (poDSIn->IsCompatibleOfSingleBlock())
    {
        nBlockYSize = poDSIn->nRasterYSize;
    }
    else
#endif
    {
        nBlockYSize = 1;
    }

#ifdef SUPPORT_CREATE
    reset_band_provision_flags();
#endif
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr PNGRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)

{
#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION
    if (nBlockYSize > 1)
    {
        GDALRasterIOExtraArg sExtraArg;
        INIT_RASTERIO_EXTRA_ARG(sExtraArg);
        const int nDTSize = GDALGetDataTypeSizeBytes(eDataType);
        return IRasterIO(GF_Read, 0, 0, nRasterXSize, nRasterYSize, pImage,
                         nRasterXSize, nRasterYSize, eDataType, nDTSize,
                         static_cast<GSpacing>(nDTSize) * nRasterXSize,
                         &sExtraArg);
    }
#endif

    PNGDataset *poGDS = cpl::down_cast<PNGDataset *>(poDS);
    CPLAssert(nBlockXOff == 0);

    const int nPixelSize = (poGDS->nBitDepth == 16) ? 2 : 1;

    if (poGDS->fpImage == nullptr)
    {
        memset(pImage, 0, cpl::fits_on<int>(nPixelSize * nRasterXSize));
        return CE_None;
    }

    // Load the desired scanline into the working buffer.
    CPLErr eErr = poGDS->LoadScanline(nBlockYOff);
    if (eErr != CE_None)
        return eErr;

    const int nPixelOffset = poGDS->nBands * nPixelSize;

    const auto CopyToDstBuffer =
        [this, nPixelOffset, nPixelSize](const GByte *pabyScanline, void *pDest)
    {
        // Transfer between the working buffer and the caller's buffer.
        if (nPixelSize == nPixelOffset)
            memcpy(pDest, pabyScanline,
                   cpl::fits_on<int>(nPixelSize * nRasterXSize));
        else if (nPixelSize == 1)
        {
            for (int i = 0; i < nRasterXSize; i++)
                reinterpret_cast<GByte *>(pDest)[i] =
                    pabyScanline[i * nPixelOffset];
        }
        else
        {
            CPLAssert(nPixelSize == 2);
            for (int i = 0; i < nRasterXSize; i++)
            {
                reinterpret_cast<GUInt16 *>(pDest)[i] =
                    *reinterpret_cast<const GUInt16 *>(pabyScanline +
                                                       i * nPixelOffset);
            }
        }
    };

    const GByte *const pabySrcBufferFirstBand =
        poGDS->pabyBuffer +
        (nBlockYOff - poGDS->nBufferStartLine) * nPixelOffset * nRasterXSize;
    CopyToDstBuffer(pabySrcBufferFirstBand + nPixelSize * (nBand - 1), pImage);

    // Forcibly load the other bands associated with this scanline.
    for (int iBand = 1; iBand <= poGDS->GetRasterCount(); iBand++)
    {
        if (iBand != nBand)
        {
            auto poIterBand = poGDS->GetRasterBand(iBand);
            GDALRasterBlock *poBlock =
                poIterBand->TryGetLockedBlockRef(nBlockXOff, nBlockYOff);
            if (poBlock != nullptr)
            {
                // Block already cached
                poBlock->DropLock();
                continue;
            }

            // Instantiate the block
            poBlock =
                poIterBand->GetLockedBlockRef(nBlockXOff, nBlockYOff, TRUE);
            if (poBlock == nullptr)
            {
                continue;
            }

            CopyToDstBuffer(pabySrcBufferFirstBand + nPixelSize * (iBand - 1),
                            poBlock->GetDataRef());

            poBlock->DropLock();
        }
    }

    return CE_None;
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp PNGRasterBand::GetColorInterpretation()

{
    PNGDataset *poGDS = cpl::down_cast<PNGDataset *>(poDS);

    if (poGDS->nColorType == PNG_COLOR_TYPE_GRAY)
        return GCI_GrayIndex;

    else if (poGDS->nColorType == PNG_COLOR_TYPE_GRAY_ALPHA)
    {
        if (nBand == 1)
            return GCI_GrayIndex;
        else
            return GCI_AlphaBand;
    }

    else if (poGDS->nColorType == PNG_COLOR_TYPE_PALETTE)
        return GCI_PaletteIndex;

    else if (poGDS->nColorType == PNG_COLOR_TYPE_RGB ||
             poGDS->nColorType == PNG_COLOR_TYPE_RGB_ALPHA)
    {
        if (nBand == 1)
            return GCI_RedBand;
        else if (nBand == 2)
            return GCI_GreenBand;
        else if (nBand == 3)
            return GCI_BlueBand;
        else
            return GCI_AlphaBand;
    }
    else
        return GCI_GrayIndex;
}

/************************************************************************/
/*                           GetColorTable()                            */
/************************************************************************/

GDALColorTable *PNGRasterBand::GetColorTable()

{
    PNGDataset *poGDS = cpl::down_cast<PNGDataset *>(poDS);

    if (nBand == 1)
        return poGDS->poColorTable;

    return nullptr;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr PNGRasterBand::SetNoDataValue(double dfNewValue)

{
    bHaveNoData = TRUE;
    dfNoDataValue = dfNewValue;

    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double PNGRasterBand::GetNoDataValue(int *pbSuccess)

{
    if (bHaveNoData)
    {
        if (pbSuccess != nullptr)
            *pbSuccess = bHaveNoData;
        return dfNoDataValue;
    }

    return GDALPamRasterBand::GetNoDataValue(pbSuccess);
}

/************************************************************************/
/* ==================================================================== */
/*                             PNGDataset                               */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                             PNGDataset()                             */
/************************************************************************/

PNGDataset::PNGDataset()
{
    memset(&sSetJmpContext, 0, sizeof(sSetJmpContext));
}

/************************************************************************/
/*                            ~PNGDataset()                             */
/************************************************************************/

PNGDataset::~PNGDataset()

{
    PNGDataset::FlushCache(true);

    if (hPNG != nullptr)
        png_destroy_read_struct(&hPNG, &psPNGInfo, nullptr);

    if (fpImage)
        VSIFCloseL(fpImage);

    if (poColorTable != nullptr)
        delete poColorTable;
}

/************************************************************************/
/*                         LoadWholeImage()                             */
/************************************************************************/

#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION

#ifdef HAVE_SSE2
#include "filter_sse2_intrinsics.c"
#endif

#if defined(__GNUC__) && !defined(__SSE2__) && !defined(USE_NEON_OPTIMIZATIONS)
__attribute__((optimize("tree-vectorize"))) static inline void
AddVectors(const GByte *CPL_RESTRICT pabyInputLine,
           GByte *CPL_RESTRICT pabyOutputLine, int nSize)
{
    for (int iX = 0; iX < nSize; ++iX)
        pabyOutputLine[iX] =
            static_cast<GByte>(pabyInputLine[iX] + pabyOutputLine[iX]);
}

__attribute__((optimize("tree-vectorize"))) static inline void
AddVectors(const GByte *CPL_RESTRICT pabyInputLine1,
           const GByte *CPL_RESTRICT pabyInputLine2,
           GByte *CPL_RESTRICT pabyOutputLine, int nSize)
{
    for (int iX = 0; iX < nSize; ++iX)
        pabyOutputLine[iX] =
            static_cast<GByte>(pabyInputLine1[iX] + pabyInputLine2[iX]);
}
#endif  //  defined(__GNUC__) && !defined(__SSE2__)

CPLErr PNGDataset::LoadWholeImage(void *pSingleBuffer, GSpacing nPixelSpace,
                                  GSpacing nLineSpace, GSpacing nBandSpace,
                                  void *apabyBuffers[4])
{
    if (fpImage == nullptr)
    {
        for (int iY = 0; iY < nRasterYSize; ++iY)
        {
            if (pSingleBuffer)
            {
                GByte *pabyDest =
                    static_cast<GByte *>(pSingleBuffer) + iY * nLineSpace;
                for (int x = 0; x < nRasterXSize; ++x)
                {
                    for (int iBand = 0; iBand < nBands; iBand++)
                    {
                        pabyDest[(x * nPixelSpace) + iBand * nBandSpace] = 0;
                    }
                }
            }
            else
            {
                for (int iBand = 0; iBand < nBands; iBand++)
                {
                    GByte *l_pabyBuffer =
                        static_cast<GByte *>(apabyBuffers[iBand]) +
                        iY * nRasterXSize;
                    memset(l_pabyBuffer, 0, nRasterXSize);
                }
            }
        }
        return CE_None;
    }

    const bool bCanUseDeinterleave =
        (nBands == 3 || nBands == 4) &&
        (apabyBuffers != nullptr ||
         (nPixelSpace == 1 &&
          nBandSpace == static_cast<GSpacing>(nRasterXSize) * nRasterYSize));

    // Below should work without SSE2, but the lack of optimized
    // filters can sometimes make it slower than regular optimized libpng,
    // so restrict to when SSE2 is available.

    // CPLDebug("PNG", "Using libdeflate optimization");

    char szChunkName[5] = {0};
    bool bError = false;

    // We try to read the zlib compressed data into pData, if there is
    // enough room for that
    size_t nDataSize = 0;
    std::vector<GByte> abyCompressedData;  // keep in this scope
    GByte *pabyCompressedData = static_cast<GByte *>(pSingleBuffer);
    size_t nCompressedDataSize = 0;
    if (pSingleBuffer)
    {
        if (nPixelSpace == nBands && nLineSpace == nPixelSpace * nRasterXSize &&
            (nBands == 1 || nBandSpace == 1))
        {
            nDataSize =
                static_cast<size_t>(nRasterXSize) * nRasterYSize * nBands;
        }
        else if (nPixelSpace == 1 && nLineSpace == nRasterXSize &&
                 nBandSpace ==
                     static_cast<GSpacing>(nRasterXSize) * nRasterYSize)
        {
            nDataSize =
                static_cast<size_t>(nRasterXSize) * nRasterYSize * nBands;
        }
    }

    const auto nPosBefore = VSIFTellL(fpImage);
    VSIFSeekL(fpImage, 8, SEEK_SET);
    // Iterate over PNG chunks
    while (true)
    {
        uint32_t nChunkSize;
        if (VSIFReadL(&nChunkSize, sizeof(nChunkSize), 1, fpImage) == 0)
        {
            bError = true;
            break;
        }
        CPL_MSBPTR32(&nChunkSize);
        if (VSIFReadL(szChunkName, 4, 1, fpImage) == 0)
        {
            bError = true;
            break;
        }
        if (strcmp(szChunkName, "IDAT") == 0)
        {
            // CPLDebug("PNG", "IDAT %u %u", unsigned(nCompressedDataSize),
            // unsigned(nChunkSize));

            // There can be several IDAT chunks: concatenate ZLib stream
            if (nChunkSize >
                std::numeric_limits<size_t>::max() - nCompressedDataSize)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Out of memory when reading compressed stream");
                bError = true;
                break;
            }

            // Sanity check to avoid allocating too much memory
            if (nCompressedDataSize + nChunkSize > 100 * 1024 * 1024)
            {
                const auto nCurPos = VSIFTellL(fpImage);
                VSIFSeekL(fpImage, 0, SEEK_END);
                const auto nSize = VSIFTellL(fpImage);
                VSIFSeekL(fpImage, nCurPos, SEEK_SET);
                if (nSize < 100 * 1024 * 1024)
                {
                    CPLError(CE_Failure, CPLE_OutOfMemory,
                             "Attempt at reading more data than available in "
                             "compressed stream");
                    bError = true;
                    break;
                }
            }

            if (nCompressedDataSize + nChunkSize > nDataSize)
            {
                const bool bVectorEmptyBefore = abyCompressedData.empty();
                // unlikely situation: would mean that the zlib compressed
                // data is longer than the decompressed image
                try
                {
                    abyCompressedData.resize(nCompressedDataSize + nChunkSize);
                    pabyCompressedData = abyCompressedData.data();
                }
                catch (const std::exception &)
                {
                    CPLError(CE_Failure, CPLE_OutOfMemory,
                             "Out of memory when allocating compressed stream");
                    bError = true;
                    break;
                }
                if (bVectorEmptyBefore && pSingleBuffer &&
                    nCompressedDataSize > 0)
                {
                    memcpy(pabyCompressedData, pSingleBuffer,
                           nCompressedDataSize);
                }
            }
            VSIFReadL(pabyCompressedData + nCompressedDataSize, nChunkSize, 1,
                      fpImage);
            nCompressedDataSize += nChunkSize;
        }
        else if (strcmp(szChunkName, "IEND") == 0)
            break;
        else
        {
            // CPLDebug("PNG", "Skipping chunk %s of size %u", szChunkName,
            // nChunkSize);
            VSIFSeekL(fpImage, nChunkSize, SEEK_CUR);
        }
        VSIFSeekL(fpImage, 4, SEEK_CUR);  // CRC
    }
    VSIFSeekL(fpImage, nPosBefore, SEEK_SET);
    if (bError)
        return CE_Failure;

    const int nSamplesPerLine = nRasterXSize * nBands;
    size_t nOutBytes;
    constexpr int FILTER_TYPE_BYTE = 1;
    const size_t nZlibDecompressedSize = static_cast<size_t>(nRasterYSize) *
                                         (FILTER_TYPE_BYTE + nSamplesPerLine);
    GByte *pabyZlibDecompressed =
        static_cast<GByte *>(VSI_MALLOC_VERBOSE(nZlibDecompressedSize));
    if (pabyZlibDecompressed == nullptr)
    {
        return CE_Failure;
    }

    if (CPLZLibInflate(pabyCompressedData, nCompressedDataSize,
                       pabyZlibDecompressed, nZlibDecompressedSize,
                       &nOutBytes) == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "CPLZLibInflate() failed");
        CPLFree(pabyZlibDecompressed);
        return CE_Failure;
    }

    GByte *pabyOutputBuffer;
    std::vector<GByte> abyTemp;
    std::vector<GByte> abyLineUp;

    if (pSingleBuffer != nullptr && nPixelSpace == nBands &&
        nLineSpace == nPixelSpace * nRasterXSize &&
        (nBands == 1 || nBandSpace == 1))
    {
        pabyOutputBuffer = static_cast<GByte *>(pSingleBuffer);
    }
    else
    {
        abyTemp.resize(nSamplesPerLine);
        pabyOutputBuffer = abyTemp.data();
    }

    for (int iY = 0; iY < nRasterYSize; ++iY)
    {
        // Cf http://www.libpng.org/pub/png/spec/1.2/PNG-Filters.html
        // CPLDebug("PNG", "Line %d, filter type = %d", iY, nFilterType);
        const GByte *CPL_RESTRICT pabyInputLine =
            pabyZlibDecompressed +
            static_cast<size_t>(iY) * (FILTER_TYPE_BYTE + nSamplesPerLine);
        const GByte nFilterType = pabyInputLine[0];
        pabyInputLine++;
        GByte *const CPL_RESTRICT pabyOutputLine =
            abyTemp.empty()
                ? pabyOutputBuffer + static_cast<size_t>(iY) * nSamplesPerLine
                : abyTemp.data();
        if (nFilterType == 0)
        {
            // Filter type 0: None
            memcpy(pabyOutputLine, pabyInputLine, nSamplesPerLine);
        }
        else if (nFilterType == 1)
        {
            // Filter type 1: Sub (horizontal differencing)
#ifdef HAVE_SSE2
            if (nBands == 3)
            {
                png_row_info row_info;
                memset(&row_info, 0, sizeof(row_info));
                row_info.rowbytes = nSamplesPerLine;

                gdal_png_read_filter_row_sub3_sse2(&row_info, pabyInputLine,
                                                   pabyOutputLine);
            }
            else if (nBands == 4)
            {
                png_row_info row_info;
                memset(&row_info, 0, sizeof(row_info));
                row_info.rowbytes = nSamplesPerLine;

                gdal_png_read_filter_row_sub4_sse2(&row_info, pabyInputLine,
                                                   pabyOutputLine);
            }
            else
#endif
            {
                int iX;
                for (iX = 0; iX < nBands; ++iX)
                    pabyOutputLine[iX] = pabyInputLine[iX];
#if !defined(HAVE_SSE2)
                if (nBands == 3)
                {
                    GByte nLast0 = pabyOutputLine[0];
                    GByte nLast1 = pabyOutputLine[1];
                    GByte nLast2 = pabyOutputLine[2];
                    for (; iX + 5 < nSamplesPerLine; iX += 6)
                    {
                        nLast0 =
                            static_cast<GByte>(nLast0 + pabyInputLine[iX + 0]);
                        nLast1 =
                            static_cast<GByte>(nLast1 + pabyInputLine[iX + 1]);
                        nLast2 =
                            static_cast<GByte>(nLast2 + pabyInputLine[iX + 2]);
                        pabyOutputLine[iX + 0] = nLast0;
                        pabyOutputLine[iX + 1] = nLast1;
                        pabyOutputLine[iX + 2] = nLast2;
                        nLast0 =
                            static_cast<GByte>(nLast0 + pabyInputLine[iX + 3]);
                        nLast1 =
                            static_cast<GByte>(nLast1 + pabyInputLine[iX + 4]);
                        nLast2 =
                            static_cast<GByte>(nLast2 + pabyInputLine[iX + 5]);
                        pabyOutputLine[iX + 3] = nLast0;
                        pabyOutputLine[iX + 4] = nLast1;
                        pabyOutputLine[iX + 5] = nLast2;
                    }
                }
                else if (nBands == 4)
                {
                    GByte nLast0 = pabyOutputLine[0];
                    GByte nLast1 = pabyOutputLine[1];
                    GByte nLast2 = pabyOutputLine[2];
                    GByte nLast3 = pabyOutputLine[3];
                    for (; iX + 7 < nSamplesPerLine; iX += 8)
                    {
                        nLast0 =
                            static_cast<GByte>(nLast0 + pabyInputLine[iX + 0]);
                        nLast1 =
                            static_cast<GByte>(nLast1 + pabyInputLine[iX + 1]);
                        nLast2 =
                            static_cast<GByte>(nLast2 + pabyInputLine[iX + 2]);
                        nLast3 =
                            static_cast<GByte>(nLast3 + pabyInputLine[iX + 3]);
                        pabyOutputLine[iX + 0] = nLast0;
                        pabyOutputLine[iX + 1] = nLast1;
                        pabyOutputLine[iX + 2] = nLast2;
                        pabyOutputLine[iX + 3] = nLast3;
                        nLast0 =
                            static_cast<GByte>(nLast0 + pabyInputLine[iX + 4]);
                        nLast1 =
                            static_cast<GByte>(nLast1 + pabyInputLine[iX + 5]);
                        nLast2 =
                            static_cast<GByte>(nLast2 + pabyInputLine[iX + 6]);
                        nLast3 =
                            static_cast<GByte>(nLast3 + pabyInputLine[iX + 7]);
                        pabyOutputLine[iX + 4] = nLast0;
                        pabyOutputLine[iX + 5] = nLast1;
                        pabyOutputLine[iX + 6] = nLast2;
                        pabyOutputLine[iX + 7] = nLast3;
                    }
                }
#endif
                for (; iX < nSamplesPerLine; ++iX)
                    pabyOutputLine[iX] = static_cast<GByte>(
                        pabyInputLine[iX] + pabyOutputLine[iX - nBands]);
            }
        }
        else if (nFilterType == 2)
        {
            // Filter type 2: Up (vertical differencing)
            if (iY == 0)
            {
                memcpy(pabyOutputLine, pabyInputLine, nSamplesPerLine);
            }
            else
            {
                if (abyTemp.empty())
                {
                    const GByte *CPL_RESTRICT pabyOutputLineUp =
                        pabyOutputBuffer +
                        (static_cast<size_t>(iY) - 1) * nSamplesPerLine;
#if defined(__GNUC__) && !defined(__SSE2__) && !defined(USE_NEON_OPTIMIZATIONS)
                    AddVectors(pabyInputLine, pabyOutputLineUp, pabyOutputLine,
                               nSamplesPerLine);
#else
                    int iX;
#ifdef HAVE_SSE2
                    for (iX = 0; iX + 31 < nSamplesPerLine; iX += 32)
                    {
                        auto in =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyInputLine + iX));
                        auto in2 =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyInputLine + iX + 16));
                        auto up =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyOutputLineUp + iX));
                        auto up2 =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyOutputLineUp + iX + 16));
                        in = _mm_add_epi8(in, up);
                        in2 = _mm_add_epi8(in2, up2);
                        _mm_storeu_si128(
                            reinterpret_cast<__m128i *>(pabyOutputLine + iX),
                            in);
                        _mm_storeu_si128(reinterpret_cast<__m128i *>(
                                             pabyOutputLine + iX + 16),
                                         in2);
                    }
#endif
                    for (; iX < nSamplesPerLine; ++iX)
                        pabyOutputLine[iX] = static_cast<GByte>(
                            pabyInputLine[iX] + pabyOutputLineUp[iX]);
#endif
                }
                else
                {
#if defined(__GNUC__) && !defined(__SSE2__) && !defined(USE_NEON_OPTIMIZATIONS)
                    AddVectors(pabyInputLine, pabyOutputLine, nSamplesPerLine);
#else
                    int iX;
#ifdef HAVE_SSE2
                    for (iX = 0; iX + 31 < nSamplesPerLine; iX += 32)
                    {
                        auto in =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyInputLine + iX));
                        auto in2 =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyInputLine + iX + 16));
                        auto out =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyOutputLine + iX));
                        auto out2 =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                                pabyOutputLine + iX + 16));
                        out = _mm_add_epi8(out, in);
                        out2 = _mm_add_epi8(out2, in2);
                        _mm_storeu_si128(
                            reinterpret_cast<__m128i *>(pabyOutputLine + iX),
                            out);
                        _mm_storeu_si128(reinterpret_cast<__m128i *>(
                                             pabyOutputLine + iX + 16),
                                         out2);
                    }
#endif
                    for (; iX < nSamplesPerLine; ++iX)
                        pabyOutputLine[iX] = static_cast<GByte>(
                            pabyOutputLine[iX] + pabyInputLine[iX]);
#endif
                }
            }
        }
        else if (nFilterType == 3)
        {
            // Filter type 3: Average
            if (iY == 0)
            {
                for (int iX = 0; iX < nBands; ++iX)
                {
                    pabyOutputLine[iX] = pabyInputLine[iX];
                }
                for (int iX = nBands; iX < nSamplesPerLine; ++iX)
                {
                    pabyOutputLine[iX] = static_cast<GByte>(
                        pabyInputLine[iX] + pabyOutputLine[iX - nBands] / 2);
                }
            }
            else
            {
#ifdef HAVE_SSE2
                if (nBands == 3)
                {
                    png_row_info row_info;
                    memset(&row_info, 0, sizeof(row_info));
                    row_info.rowbytes = nSamplesPerLine;
                    if (!abyTemp.empty())
                        abyLineUp = abyTemp;
                    const GByte *const pabyOutputLineUp =
                        abyTemp.empty()
                            ? pabyOutputBuffer + (static_cast<size_t>(iY) - 1) *
                                                     nSamplesPerLine
                            : abyLineUp.data();

                    gdal_png_read_filter_row_avg3_sse2(&row_info, pabyInputLine,
                                                       pabyOutputLine,
                                                       pabyOutputLineUp);
                }
                else if (nBands == 4)
                {
                    png_row_info row_info;
                    memset(&row_info, 0, sizeof(row_info));
                    row_info.rowbytes = nSamplesPerLine;
                    if (!abyTemp.empty())
                        abyLineUp = abyTemp;
                    const GByte *const pabyOutputLineUp =
                        abyTemp.empty()
                            ? pabyOutputBuffer + (static_cast<size_t>(iY) - 1) *
                                                     nSamplesPerLine
                            : abyLineUp.data();

                    gdal_png_read_filter_row_avg4_sse2(&row_info, pabyInputLine,
                                                       pabyOutputLine,
                                                       pabyOutputLineUp);
                }
                else
#endif
                    if (abyTemp.empty())
                {
                    const GByte *CPL_RESTRICT pabyOutputLineUp =
                        pabyOutputBuffer +
                        (static_cast<size_t>(iY) - 1) * nSamplesPerLine;
                    for (int iX = 0; iX < nBands; ++iX)
                    {
                        pabyOutputLine[iX] = static_cast<GByte>(
                            pabyInputLine[iX] + pabyOutputLineUp[iX] / 2);
                    }
                    for (int iX = nBands; iX < nSamplesPerLine; ++iX)
                    {
                        pabyOutputLine[iX] = static_cast<GByte>(
                            pabyInputLine[iX] + (pabyOutputLine[iX - nBands] +
                                                 pabyOutputLineUp[iX]) /
                                                    2);
                    }
                }
                else
                {
                    for (int iX = 0; iX < nBands; ++iX)
                    {
                        pabyOutputLine[iX] = static_cast<GByte>(
                            pabyInputLine[iX] + pabyOutputLine[iX] / 2);
                    }
                    for (int iX = nBands; iX < nSamplesPerLine; ++iX)
                    {
                        pabyOutputLine[iX] = static_cast<GByte>(
                            pabyInputLine[iX] +
                            (pabyOutputLine[iX - nBands] + pabyOutputLine[iX]) /
                                2);
                    }
                }
            }
        }
        else if (nFilterType == 4)
        {
            // Filter type 4: Paeth
            if (iY == 0)
            {
                for (int iX = 0; iX < nSamplesPerLine; ++iX)
                {
                    GByte a = iX < nBands ? 0 : pabyOutputLine[iX - nBands];
                    pabyOutputLine[iX] =
                        static_cast<GByte>(pabyInputLine[iX] + a);
                }
            }
            else
            {
                if (!abyTemp.empty())
                    abyLineUp = abyTemp;
                const GByte *const pabyOutputLineUp =
                    abyTemp.empty()
                        ? pabyOutputBuffer +
                              (static_cast<size_t>(iY) - 1) * nSamplesPerLine
                        : abyLineUp.data();
#ifdef HAVE_SSE2
                if (nBands == 3)
                {
                    png_row_info row_info;
                    memset(&row_info, 0, sizeof(row_info));
                    row_info.rowbytes = nSamplesPerLine;
                    gdal_png_read_filter_row_paeth3_sse2(
                        &row_info, pabyInputLine, pabyOutputLine,
                        pabyOutputLineUp);
                }
                else if (nBands == 4)
                {
                    png_row_info row_info;
                    memset(&row_info, 0, sizeof(row_info));
                    row_info.rowbytes = nSamplesPerLine;
                    gdal_png_read_filter_row_paeth4_sse2(
                        &row_info, pabyInputLine, pabyOutputLine,
                        pabyOutputLineUp);
                }
                else
#endif
                {
                    int iX = 0;
                    for (; iX < nBands; ++iX)
                    {
                        GByte b = pabyOutputLineUp[iX];
                        pabyOutputLine[iX] =
                            static_cast<GByte>(pabyInputLine[iX] + b);
                    }
                    for (; iX < nSamplesPerLine; ++iX)
                    {
                        GByte a = pabyOutputLine[iX - nBands];
                        GByte b = pabyOutputLineUp[iX];
                        GByte c = pabyOutputLineUp[iX - nBands];
                        int p_minus_a = b - c;
                        int p_minus_b = a - c;
                        int p_minus_c = p_minus_a + p_minus_b;
                        int pa = std::abs(p_minus_a);
                        int pb = std::abs(p_minus_b);
                        int pc = std::abs(p_minus_c);
                        if (pa <= pb && pa <= pc)
                            pabyOutputLine[iX] =
                                static_cast<GByte>(pabyInputLine[iX] + a);
                        else if (pb <= pc)
                            pabyOutputLine[iX] =
                                static_cast<GByte>(pabyInputLine[iX] + b);
                        else
                            pabyOutputLine[iX] =
                                static_cast<GByte>(pabyInputLine[iX] + c);
                    }
                }
            }
        }
        else
        {
            CPLError(CE_Failure, CPLE_NotSupported, "Invalid filter type %d",
                     nFilterType);
            CPLFree(pabyZlibDecompressed);
            return CE_Failure;
        }

        if (!abyTemp.empty())
        {
            if (pSingleBuffer)
            {
                GByte *pabyDest =
                    static_cast<GByte *>(pSingleBuffer) + iY * nLineSpace;
                if (bCanUseDeinterleave)
                {
                    // Cache friendly way for typical band interleaved case.
                    void *apDestBuffers[4];
                    apDestBuffers[0] = pabyDest;
                    apDestBuffers[1] = pabyDest + nBandSpace;
                    apDestBuffers[2] = pabyDest + 2 * nBandSpace;
                    apDestBuffers[3] = pabyDest + 3 * nBandSpace;
                    GDALDeinterleave(pabyOutputLine, GDT_Byte, nBands,
                                     apDestBuffers, GDT_Byte, nRasterXSize);
                }
                else if (nPixelSpace <= nBands && nBandSpace > nBands)
                {
                    // Cache friendly way for typical band interleaved case.
                    for (int iBand = 0; iBand < nBands; iBand++)
                    {
                        GByte *pabyDest2 = pabyDest + iBand * nBandSpace;
                        const GByte *pabyScanline2 = pabyOutputLine + iBand;
                        GDALCopyWords(pabyScanline2, GDT_Byte, nBands,
                                      pabyDest2, GDT_Byte,
                                      static_cast<int>(nPixelSpace),
                                      nRasterXSize);
                    }
                }
                else
                {
                    // Generic method
                    for (int x = 0; x < nRasterXSize; ++x)
                    {
                        for (int iBand = 0; iBand < nBands; iBand++)
                        {
                            pabyDest[(x * nPixelSpace) + iBand * nBandSpace] =
                                pabyOutputLine[x * nBands + iBand];
                        }
                    }
                }
            }
            else
            {
                GByte *apabyDestBuffers[4];
                for (int iBand = 0; iBand < nBands; iBand++)
                {
                    apabyDestBuffers[iBand] =
                        static_cast<GByte *>(apabyBuffers[iBand]) +
                        iY * nRasterXSize;
                }
                if (bCanUseDeinterleave)
                {
                    // Cache friendly way for typical band interleaved case.
                    GDALDeinterleave(
                        pabyOutputLine, GDT_Byte, nBands,
                        reinterpret_cast<void **>(apabyDestBuffers), GDT_Byte,
                        nRasterXSize);
                }
                else
                {
                    // Generic method
                    for (int x = 0; x < nRasterXSize; ++x)
                    {
                        for (int iBand = 0; iBand < nBands; iBand++)
                        {
                            apabyDestBuffers[iBand][x] =
                                pabyOutputLine[x * nBands + iBand];
                        }
                    }
                }
            }
        }
    }

    CPLFree(pabyZlibDecompressed);

    return CE_None;
}

#endif  // ENABLE_WHOLE_IMAGE_OPTIMIZATION

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr PNGDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, BANDMAP_TYPE panBandMap,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg)

{
    // Coverity says that we cannot pass a nullptr to IRasterIO.
    if (panBandMap == nullptr)
    {
        return CE_Failure;
    }

    if ((eRWFlag == GF_Read) && (nBandCount == nBands) && (nXOff == 0) &&
        (nYOff == 0) && (nXSize == nBufXSize) && (nXSize == nRasterXSize) &&
        (nYSize == nBufYSize) && (nYSize == nRasterYSize) &&
        (eBufType == GDT_Byte) &&
        (eBufType == GetRasterBand(1)->GetRasterDataType()) &&
        (pData != nullptr) && IsAllBands(nBands, panBandMap))
    {
#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION
        // Below should work without SSE2, but the lack of optimized
        // filters can sometimes make it slower than regular optimized libpng,
        // so restrict to when SSE2 is available.

        if (!bInterlaced && nBitDepth == 8 &&
            CPLTestBool(
                CPLGetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "YES")))
        {
            return LoadWholeImage(pData, nPixelSpace, nLineSpace, nBandSpace,
                                  nullptr);
        }
        else if (cpl::down_cast<PNGRasterBand *>(papoBands[0])->nBlockYSize > 1)
        {
            // Below code requires scanline access in
            // PNGRasterBand::IReadBlock()
        }
        else
#endif  // ENABLE_WHOLE_IMAGE_OPTIMIZATION

            // Pixel interleaved case.
            if (nBandSpace == 1)
            {
                for (int y = 0; y < nYSize; ++y)
                {
                    CPLErr tmpError = LoadScanline(y);
                    if (tmpError != CE_None)
                        return tmpError;
                    const GByte *pabyScanline =
                        pabyBuffer + (y - nBufferStartLine) * nBands * nXSize;
                    if (nPixelSpace == nBandSpace * nBandCount)
                    {
                        memcpy(&(static_cast<GByte *>(pData)[(y * nLineSpace)]),
                               pabyScanline,
                               cpl::fits_on<int>(nBandCount * nXSize));
                    }
                    else
                    {
                        for (int x = 0; x < nXSize; ++x)
                        {
                            memcpy(&(static_cast<GByte *>(
                                       pData)[(y * nLineSpace) +
                                              (x * nPixelSpace)]),
                                   &(pabyScanline[x * nBandCount]), nBandCount);
                        }
                    }
                }
                return CE_None;
            }
            else
            {
                const bool bCanUseDeinterleave =
                    (nBands == 3 || nBands == 4) && nPixelSpace == 1 &&
                    nBandSpace ==
                        static_cast<GSpacing>(nRasterXSize) * nRasterYSize;

                for (int y = 0; y < nYSize; ++y)
                {
                    CPLErr tmpError = LoadScanline(y);
                    if (tmpError != CE_None)
                        return tmpError;
                    const GByte *pabyScanline =
                        pabyBuffer + (y - nBufferStartLine) * nBands * nXSize;
                    GByte *pabyDest =
                        static_cast<GByte *>(pData) + y * nLineSpace;
                    if (bCanUseDeinterleave)
                    {
                        // Cache friendly way for typical band interleaved case.
                        void *apDestBuffers[4];
                        apDestBuffers[0] = pabyDest;
                        apDestBuffers[1] = pabyDest + nBandSpace;
                        apDestBuffers[2] = pabyDest + 2 * nBandSpace;
                        apDestBuffers[3] = pabyDest + 3 * nBandSpace;
                        GDALDeinterleave(pabyScanline, GDT_Byte, nBands,
                                         apDestBuffers, GDT_Byte, nRasterXSize);
                    }
                    else if (nPixelSpace <= nBands && nBandSpace > nBands)
                    {
                        // Cache friendly way for typical band interleaved case.
                        for (int iBand = 0; iBand < nBands; iBand++)
                        {
                            GByte *pabyDest2 = pabyDest + iBand * nBandSpace;
                            const GByte *pabyScanline2 = pabyScanline + iBand;
                            GDALCopyWords(pabyScanline2, GDT_Byte, nBands,
                                          pabyDest2, GDT_Byte,
                                          static_cast<int>(nPixelSpace),
                                          nXSize);
                        }
                    }
                    else
                    {
                        // Generic method
                        for (int x = 0; x < nXSize; ++x)
                        {
                            for (int iBand = 0; iBand < nBands; iBand++)
                            {
                                pabyDest[(x * nPixelSpace) +
                                         iBand * nBandSpace] =
                                    pabyScanline[x * nBands + iBand];
                            }
                        }
                    }
                }
                return CE_None;
            }
    }

    return GDALPamDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nBandCount, panBandMap, nPixelSpace,
                                     nLineSpace, nBandSpace, psExtraArg);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr PNGRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                int nXSize, int nYSize, void *pData,
                                int nBufXSize, int nBufYSize,
                                GDALDataType eBufType, GSpacing nPixelSpace,
                                GSpacing nLineSpace,
                                GDALRasterIOExtraArg *psExtraArg)

{
#ifdef ENABLE_WHOLE_IMAGE_OPTIMIZATION
    auto poGDS = cpl::down_cast<PNGDataset *>(poDS);
    if ((eRWFlag == GF_Read) && (nXOff == 0) && (nYOff == 0) &&
        (nXSize == nBufXSize) && (nXSize == nRasterXSize) &&
        (nYSize == nBufYSize) && (nYSize == nRasterYSize) &&
        (eBufType == GDT_Byte) && (eBufType == eDataType))
    {
        bool bBlockAlreadyLoaded = false;
        if (nBlockYSize > 1)
        {
            auto poBlock = TryGetLockedBlockRef(0, 0);
            if (poBlock != nullptr)
            {
                bBlockAlreadyLoaded = poBlock->GetDataRef() != pData;
                poBlock->DropLock();
            }
        }

        if (bBlockAlreadyLoaded)
        {
            // will got to general case
        }
        else if (poGDS->nBands == 1 && !poGDS->bInterlaced &&
                 poGDS->nBitDepth == 8 &&
                 CPLTestBool(
                     CPLGetConfigOption("GDAL_PNG_WHOLE_IMAGE_OPTIM", "YES")))
        {
            return poGDS->LoadWholeImage(pData, nPixelSpace, nLineSpace, 0,
                                         nullptr);
        }
        else if (nBlockYSize > 1)
        {
            void *apabyBuffers[4];
            GDALRasterBlock *apoBlocks[4] = {nullptr, nullptr, nullptr,
                                             nullptr};
            CPLErr eErr = CE_None;
            bool bNeedToUseDefaultCase = true;
            for (int i = 0; i < poGDS->nBands; ++i)
            {
                if (i + 1 == nBand && nPixelSpace == 1 &&
                    nLineSpace == nRasterXSize)
                {
                    bNeedToUseDefaultCase = false;
                    apabyBuffers[i] = pData;
                }
                else
                {
                    apoBlocks[i] =
                        poGDS->GetRasterBand(i + 1)->GetLockedBlockRef(0, 0,
                                                                       TRUE);
                    apabyBuffers[i] =
                        apoBlocks[i] ? apoBlocks[i]->GetDataRef() : nullptr;
                    if (apabyBuffers[i] == nullptr)
                        eErr = CE_Failure;
                }
            }
            if (eErr == CE_None)
            {
                eErr = poGDS->LoadWholeImage(nullptr, 0, 0, 0, apabyBuffers);
            }
            for (int i = 0; i < poGDS->nBands; ++i)
            {
                if (apoBlocks[i])
                    apoBlocks[i]->DropLock();
            }
            if (eErr != CE_None || !bNeedToUseDefaultCase)
                return eErr;
        }
    }
#endif
    return GDALPamRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                        pData, nBufXSize, nBufYSize, eBufType,
                                        nPixelSpace, nLineSpace, psExtraArg);
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr PNGDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    const_cast<PNGDataset *>(this)->LoadWorldFile();

    if (bGeoTransformValid)
    {
        gt = m_gt;
        return CE_None;
    }

    return GDALPamDataset::GetGeoTransform(gt);
}

/************************************************************************/
/*                             FlushCache()                             */
/*                                                                      */
/*      We override this so we can also flush out local TIFF strip      */
/*      cache if need be.                                               */
/************************************************************************/

CPLErr PNGDataset::FlushCache(bool bAtClosing)

{
    const CPLErr eErr = GDALPamDataset::FlushCache(bAtClosing);

    if (pabyBuffer != nullptr)
    {
        CPLFree(pabyBuffer);
        pabyBuffer = nullptr;
        nBufferStartLine = 0;
        nBufferLines = 0;
    }
    return eErr;
}

#ifdef DISABLE_CRC_CHECK
/************************************************************************/
/*                     PNGDatasetDisableCRCCheck()                      */
/************************************************************************/

static void PNGDatasetDisableCRCCheck(png_structp hPNG)
{
    hPNG->flags &= ~PNG_FLAG_CRC_CRITICAL_MASK;
    hPNG->flags |= PNG_FLAG_CRC_CRITICAL_IGNORE;

    hPNG->flags &= ~PNG_FLAG_CRC_ANCILLARY_MASK;
    hPNG->flags |= PNG_FLAG_CRC_ANCILLARY_NOWARN;
}
#endif

/************************************************************************/
/*                              Restart()                               */
/*                                                                      */
/*      Restart reading from the beginning of the file.                 */
/************************************************************************/

void PNGDataset::Restart()

{
    if (!m_bHasRewind)
    {
        m_bHasRewind = true;
        CPLDebug("PNG", "Restart decompression from top (emitted once)");
    }

    png_destroy_read_struct(&hPNG, &psPNGInfo, nullptr);

    hPNG =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, this, nullptr, nullptr);

#ifdef DISABLE_CRC_CHECK
    PNGDatasetDisableCRCCheck(hPNG);
#endif

    png_set_error_fn(hPNG, &sSetJmpContext, png_gdal_error, png_gdal_warning);
    if (setjmp(sSetJmpContext) != 0)
        return;

    psPNGInfo = png_create_info_struct(hPNG);

    VSIFSeekL(fpImage, 0, SEEK_SET);
    png_set_read_fn(hPNG, fpImage, png_vsi_read_data);
    png_read_info(hPNG, psPNGInfo);

    if (nBitDepth < 8)
        png_set_packing(hPNG);

    nLastLineRead = -1;
}

/************************************************************************/
/*                        safe_png_read_image()                         */
/************************************************************************/

static bool safe_png_read_image(png_structp hPNG, png_bytep *png_rows,
                                jmp_buf sSetJmpContext)
{
    if (setjmp(sSetJmpContext) != 0)
        return false;
    png_read_image(hPNG, png_rows);
    return true;
}

/************************************************************************/
/*                        LoadInterlacedChunk()                         */
/************************************************************************/

CPLErr PNGDataset::LoadInterlacedChunk(int iLine)

{
    const int nPixelOffset =
        (nBitDepth == 16) ? 2 * GetRasterCount() : GetRasterCount();

    // What is the biggest chunk we can safely operate on?
    constexpr int MAX_PNG_CHUNK_BYTES = 100000000;

    int nMaxChunkLines =
        std::max(1, MAX_PNG_CHUNK_BYTES / (nPixelOffset * GetRasterXSize()));

    if (nMaxChunkLines > GetRasterYSize())
        nMaxChunkLines = GetRasterYSize();

    // Allocate chunk buffer if we don't already have it from a previous
    // request.
    nBufferLines = nMaxChunkLines;
    if (nMaxChunkLines + iLine > GetRasterYSize())
        nBufferStartLine = GetRasterYSize() - nMaxChunkLines;
    else
        nBufferStartLine = iLine;

    if (pabyBuffer == nullptr)
    {
        pabyBuffer = static_cast<GByte *>(VSI_MALLOC3_VERBOSE(
            nPixelOffset, GetRasterXSize(), nMaxChunkLines));

        if (pabyBuffer == nullptr)
        {
            return CE_Failure;
        }
#ifdef notdef
        if (nMaxChunkLines < GetRasterYSize())
            CPLDebug("PNG",
                     "Interlaced file being handled in %d line chunks.\n"
                     "Performance is likely to be quite poor.",
                     nMaxChunkLines);
#endif
    }

    // Do we need to restart reading? We do this if we aren't on the first
    // attempt to read the image.
    if (nLastLineRead != -1)
    {
        Restart();
    }

    // Allocate and populate rows array. We create a row for each row in the
    // image but use our dummy line for rows not in the target window.
    png_bytep dummy_row = reinterpret_cast<png_bytep>(
        CPLMalloc(cpl::fits_on<int>(nPixelOffset * GetRasterXSize())));
    png_bytep *png_rows = reinterpret_cast<png_bytep *>(
        CPLMalloc(sizeof(png_bytep) * GetRasterYSize()));

    for (int i = 0; i < GetRasterYSize(); i++)
    {
        if (i >= nBufferStartLine && i < nBufferStartLine + nBufferLines)
            png_rows[i] = pabyBuffer + (i - nBufferStartLine) * nPixelOffset *
                                           GetRasterXSize();
        else
            png_rows[i] = dummy_row;
    }

    bool bRet = safe_png_read_image(hPNG, png_rows, sSetJmpContext);

    // Do swap on LSB machines. 16-bit PNG data is stored in MSB format.
    if (bRet && nBitDepth == 16
#ifdef CPL_LSB
        && !m_bByteOrderIsLittleEndian
#else
        && m_bByteOrderIsLittleEndian
#endif
    )
    {
        for (int i = 0; i < GetRasterYSize(); i++)
        {
            if (i >= nBufferStartLine && i < nBufferStartLine + nBufferLines)
            {
                GDALSwapWords(png_rows[i], 2,
                              GetRasterXSize() * GetRasterCount(), 2);
            }
        }
    }

    CPLFree(png_rows);
    CPLFree(dummy_row);
    if (!bRet)
        return CE_Failure;

    nLastLineRead = nBufferStartLine + nBufferLines - 1;

    return CE_None;
}

/************************************************************************/
/*                        safe_png_read_rows()                          */
/************************************************************************/

static bool safe_png_read_rows(png_structp hPNG, png_bytep row,
                               jmp_buf sSetJmpContext)
{
    if (setjmp(sSetJmpContext) != 0)
        return false;
    png_read_rows(hPNG, &row, nullptr, 1);
    return true;
}

/************************************************************************/
/*                            LoadScanline()                            */
/************************************************************************/

CPLErr PNGDataset::LoadScanline(int nLine)

{
    CPLAssert(nLine >= 0 && nLine < GetRasterYSize());

    if (nLine >= nBufferStartLine && nLine < nBufferStartLine + nBufferLines)
        return CE_None;

    const int nPixelOffset =
        (nBitDepth == 16) ? 2 * GetRasterCount() : GetRasterCount();

    // If the file is interlaced, we load the entire image into memory using the
    // high-level API.
    if (bInterlaced)
        return LoadInterlacedChunk(nLine);

    // Ensure we have space allocated for one scanline.
    if (pabyBuffer == nullptr)
        pabyBuffer = reinterpret_cast<GByte *>(
            CPLMalloc(cpl::fits_on<int>(nPixelOffset * GetRasterXSize())));

    // Otherwise we just try to read the requested row. Do we need to rewind and
    // start over?
    if (nLine <= nLastLineRead)
    {
        Restart();
    }

    // Read till we get the desired row.
    png_bytep row = pabyBuffer;
    const GUInt32 nErrorCounter = CPLGetErrorCounter();
    while (nLine > nLastLineRead)
    {
        if (!safe_png_read_rows(hPNG, row, sSetJmpContext))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error while reading row %d%s", nLine,
                     (nErrorCounter != CPLGetErrorCounter())
                         ? CPLSPrintf(": %s", CPLGetLastErrorMsg())
                         : "");
            return CE_Failure;
        }
        nLastLineRead++;
    }

    nBufferStartLine = nLine;
    nBufferLines = 1;

    // Do swap on LSB machines. 16-bit PNG data is stored in MSB format.
    if (nBitDepth == 16
#ifdef CPL_LSB
        && !m_bByteOrderIsLittleEndian
#else
        && m_bByteOrderIsLittleEndian
#endif
    )
    {
        GDALSwapWords(row, 2, GetRasterXSize() * GetRasterCount(), 2);
    }

    return CE_None;
}

/************************************************************************/
/*                          CollectMetadata()                           */
/*                                                                      */
/*      We normally do this after reading up to the image, but be       */
/*      forewarned: we can miss text chunks this way.                   */
/*                                                                      */
/*      We turn each PNG text chunk into one metadata item.  It         */
/*      might be nice to preserve language information though we        */
/*      don't try to now.                                               */
/************************************************************************/

void PNGDataset::CollectMetadata()

{
    if (nBitDepth < 8)
    {
        for (int iBand = 0; iBand < nBands; iBand++)
        {
            GetRasterBand(iBand + 1)->SetMetadataItem(
                "NBITS", CPLString().Printf("%d", nBitDepth),
                "IMAGE_STRUCTURE");
        }
    }

    int nTextCount;
    png_textp text_ptr;
    if (png_get_text(hPNG, psPNGInfo, &text_ptr, &nTextCount) == 0)
        return;

    for (int iText = 0; iText < nTextCount; iText++)
    {
        char *pszTag = CPLStrdup(text_ptr[iText].key);

        for (int i = 0; pszTag[i] != '\0'; i++)
        {
            if (pszTag[i] == ' ' || pszTag[i] == '=' || pszTag[i] == ':')
                pszTag[i] = '_';
        }

        GDALDataset::SetMetadataItem(pszTag, text_ptr[iText].text);
        CPLFree(pszTag);
    }
}

/************************************************************************/
/*                       CollectXMPMetadata()                           */
/************************************************************************/

// See §2.1.5 of
// http://wwwimages.adobe.com/www.adobe.com/content/dam/Adobe/en/devnet/xmp/pdfs/XMPSpecificationPart3.pdf.

void PNGDataset::CollectXMPMetadata()

{
    if (fpImage == nullptr || bHasReadXMPMetadata)
        return;

    // Save current position to avoid disturbing PNG stream decoding.
    const vsi_l_offset nCurOffset = VSIFTellL(fpImage);

    vsi_l_offset nOffset = 8;
    VSIFSeekL(fpImage, nOffset, SEEK_SET);

    // Loop over chunks.
    while (true)
    {
        int nLength;

        if (VSIFReadL(&nLength, 4, 1, fpImage) != 1)
            break;
        nOffset += 4;
        CPL_MSBPTR32(&nLength);
        if (nLength <= 0)
            break;

        char pszChunkType[5];
        if (VSIFReadL(pszChunkType, 4, 1, fpImage) != 1)
            break;
        nOffset += 4;
        pszChunkType[4] = 0;

        if (strcmp(pszChunkType, "iTXt") == 0 && nLength > 22 &&
            // Does not make sense to have a XMP content larger than 10 MB
            // (XMP in JPEG must fit in 65 KB...)
            nLength < 10 * 1024 * 1024)
        {
            char *pszContent = reinterpret_cast<char *>(VSIMalloc(nLength + 1));
            if (pszContent == nullptr)
                break;
            if (VSIFReadL(pszContent, nLength, 1, fpImage) != 1)
            {
                VSIFree(pszContent);
                break;
            }
            nOffset += nLength;
            pszContent[nLength] = '\0';
            if (memcmp(pszContent, "XML:com.adobe.xmp\0\0\0\0\0", 22) == 0)
            {
                // Avoid setting the PAM dirty bit just for that.
                const int nOldPamFlags = nPamFlags;

                char *apszMDList[2] = {pszContent + 22, nullptr};
                SetMetadata(apszMDList, "xml:XMP");

                // cppcheck-suppress redundantAssignment
                nPamFlags = nOldPamFlags;

                VSIFree(pszContent);

                break;
            }
            else
            {
                VSIFree(pszContent);
            }
        }
        else
        {
            nOffset += nLength;
            VSIFSeekL(fpImage, nOffset, SEEK_SET);
        }

        nOffset += 4;
        int nCRC;
        if (VSIFReadL(&nCRC, 4, 1, fpImage) != 1)
            break;
    }

    VSIFSeekL(fpImage, nCurOffset, SEEK_SET);

    bHasReadXMPMetadata = TRUE;
}

/************************************************************************/
/*                           LoadICCProfile()                           */
/************************************************************************/

void PNGDataset::LoadICCProfile()
{
    if (hPNG == nullptr || bHasReadICCMetadata)
        return;
    bHasReadICCMetadata = TRUE;

    png_charp pszProfileName;
    png_uint_32 nProfileLength;
    png_bytep pProfileData;
    int nCompressionType;

    // Avoid setting the PAM dirty bit just for that.
    int nOldPamFlags = nPamFlags;

    if (png_get_iCCP(hPNG, psPNGInfo, &pszProfileName, &nCompressionType,
                     &pProfileData, &nProfileLength) != 0)
    {
        // Escape the profile.
        char *pszBase64Profile =
            CPLBase64Encode(static_cast<int>(nProfileLength),
                            reinterpret_cast<const GByte *>(pProfileData));

        // Set ICC profile metadata.
        SetMetadataItem("SOURCE_ICC_PROFILE", pszBase64Profile,
                        "COLOR_PROFILE");
        SetMetadataItem("SOURCE_ICC_PROFILE_NAME", pszProfileName,
                        "COLOR_PROFILE");

        nPamFlags = nOldPamFlags;

        CPLFree(pszBase64Profile);

        return;
    }

    int nsRGBIntent;
    if (png_get_sRGB(hPNG, psPNGInfo, &nsRGBIntent) != 0)
    {
        SetMetadataItem("SOURCE_ICC_PROFILE_NAME", "sRGB", "COLOR_PROFILE");

        nPamFlags = nOldPamFlags;

        return;
    }

    double dfGamma;
    bool bGammaAvailable = false;
    if (png_get_valid(hPNG, psPNGInfo, PNG_INFO_gAMA))
    {
        bGammaAvailable = true;

        png_get_gAMA(hPNG, psPNGInfo, &dfGamma);

        SetMetadataItem("PNG_GAMMA", CPLString().Printf("%.9f", dfGamma),
                        "COLOR_PROFILE");
    }

    // Check that both cHRM and gAMA are available.
    if (bGammaAvailable && png_get_valid(hPNG, psPNGInfo, PNG_INFO_cHRM))
    {
        double dfaWhitepoint[2];
        double dfaCHR[6];

        png_get_cHRM(hPNG, psPNGInfo, &dfaWhitepoint[0], &dfaWhitepoint[1],
                     &dfaCHR[0], &dfaCHR[1], &dfaCHR[2], &dfaCHR[3], &dfaCHR[4],
                     &dfaCHR[5]);

        // Set all the colorimetric metadata.
        SetMetadataItem(
            "SOURCE_PRIMARIES_RED",
            CPLString().Printf("%.9f, %.9f, 1.0", dfaCHR[0], dfaCHR[1]),
            "COLOR_PROFILE");
        SetMetadataItem(
            "SOURCE_PRIMARIES_GREEN",
            CPLString().Printf("%.9f, %.9f, 1.0", dfaCHR[2], dfaCHR[3]),
            "COLOR_PROFILE");
        SetMetadataItem(
            "SOURCE_PRIMARIES_BLUE",
            CPLString().Printf("%.9f, %.9f, 1.0", dfaCHR[4], dfaCHR[5]),
            "COLOR_PROFILE");

        SetMetadataItem("SOURCE_WHITEPOINT",
                        CPLString().Printf("%.9f, %.9f, 1.0", dfaWhitepoint[0],
                                           dfaWhitepoint[1]),
                        "COLOR_PROFILE");
    }

    nPamFlags = nOldPamFlags;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **PNGDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALPamDataset::GetMetadataDomainList(),
                                   TRUE, "xml:XMP", "COLOR_PROFILE", nullptr);
}

/************************************************************************/
/*                           GetMetadata()                              */
/************************************************************************/

char **PNGDataset::GetMetadata(const char *pszDomain)
{
    if (fpImage == nullptr)
        return nullptr;
    if (eAccess == GA_ReadOnly && !bHasReadXMPMetadata &&
        pszDomain != nullptr && EQUAL(pszDomain, "xml:XMP"))
        CollectXMPMetadata();
    if (eAccess == GA_ReadOnly && !bHasReadICCMetadata &&
        pszDomain != nullptr && EQUAL(pszDomain, "COLOR_PROFILE"))
        LoadICCProfile();
    return GDALPamDataset::GetMetadata(pszDomain);
}

/************************************************************************/
/*                       GetMetadataItem()                              */
/************************************************************************/
const char *PNGDataset::GetMetadataItem(const char *pszName,
                                        const char *pszDomain)
{
    if (eAccess == GA_ReadOnly && !bHasReadICCMetadata &&
        pszDomain != nullptr && EQUAL(pszDomain, "COLOR_PROFILE"))
        LoadICCProfile();
    return GDALPamDataset::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *PNGDataset::Open(GDALOpenInfo *poOpenInfo)

{
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    // During fuzzing, do not use Identify to reject crazy content.
    if (!PNGDriverIdentify(poOpenInfo))
        return nullptr;
#else
    if (poOpenInfo->fpL == nullptr)
        return nullptr;
#endif

    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("PNG");
        return nullptr;
    }

    // Create a corresponding GDALDataset.
    PNGDataset *poDS = new PNGDataset();
    return OpenStage2(poOpenInfo, poDS);
}

GDALDataset *PNGDataset::OpenStage2(GDALOpenInfo *poOpenInfo, PNGDataset *&poDS)

{
    poDS->fpImage = poOpenInfo->fpL;
    poOpenInfo->fpL = nullptr;
    poDS->eAccess = poOpenInfo->eAccess;

    poDS->hPNG =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, poDS, nullptr, nullptr);
    if (poDS->hPNG == nullptr)
    {
        int version = static_cast<int>(png_access_version_number());
        CPLError(CE_Failure, CPLE_NotSupported,
                 "The PNG driver failed to access libpng with version '%s',"
                 " library is actually version '%d'.\n",
                 PNG_LIBPNG_VER_STRING, version);
        delete poDS;
        return nullptr;
    }

#ifdef DISABLE_CRC_CHECK
    PNGDatasetDisableCRCCheck(poDS->hPNG);
#endif

    poDS->psPNGInfo = png_create_info_struct(poDS->hPNG);

    // Set up error handling.
    png_set_error_fn(poDS->hPNG, &poDS->sSetJmpContext, png_gdal_error,
                     png_gdal_warning);

    if (setjmp(poDS->sSetJmpContext) != 0)
    {
        delete poDS;
        return nullptr;
    }

    // Read pre-image data after ensuring the file is rewound.
    // We should likely do a setjmp() here.

    png_set_read_fn(poDS->hPNG, poDS->fpImage, png_vsi_read_data);
    png_read_info(poDS->hPNG, poDS->psPNGInfo);

    // Capture some information from the file that is of interest.
    poDS->nRasterXSize =
        static_cast<int>(png_get_image_width(poDS->hPNG, poDS->psPNGInfo));
    poDS->nRasterYSize =
        static_cast<int>(png_get_image_height(poDS->hPNG, poDS->psPNGInfo));

    poDS->nBands = png_get_channels(poDS->hPNG, poDS->psPNGInfo);
    poDS->nBitDepth = png_get_bit_depth(poDS->hPNG, poDS->psPNGInfo);
    poDS->bInterlaced = png_get_interlace_type(poDS->hPNG, poDS->psPNGInfo) !=
                        PNG_INTERLACE_NONE;

    poDS->nColorType = png_get_color_type(poDS->hPNG, poDS->psPNGInfo);

    if (poDS->nColorType == PNG_COLOR_TYPE_PALETTE && poDS->nBands > 1)
    {
        CPLDebug("GDAL",
                 "PNG Driver got %d from png_get_channels(),\n"
                 "but this kind of image (paletted) can only have one band.\n"
                 "Correcting and continuing, but this may indicate a bug!",
                 poDS->nBands);
        poDS->nBands = 1;
    }

    // We want to treat 1-, 2-, and 4-bit images as eight bit. This call causes
    // libpng to unpack the image.
    if (poDS->nBitDepth < 8)
        png_set_packing(poDS->hPNG);

    // Create band information objects.
    for (int iBand = 0; iBand < poDS->nBands; iBand++)
        poDS->SetBand(iBand + 1, new PNGRasterBand(poDS, iBand + 1));

    // Is there a palette?  Note: we should also read back and apply
    // transparency values if available.
    if (poDS->nColorType == PNG_COLOR_TYPE_PALETTE)
    {
        png_color *pasPNGPalette = nullptr;
        int nColorCount = 0;

        if (png_get_PLTE(poDS->hPNG, poDS->psPNGInfo, &pasPNGPalette,
                         &nColorCount) == 0)
            nColorCount = 0;

        unsigned char *trans = nullptr;
        png_color_16 *trans_values = nullptr;
        int num_trans = 0;
        png_get_tRNS(poDS->hPNG, poDS->psPNGInfo, &trans, &num_trans,
                     &trans_values);

        poDS->poColorTable = new GDALColorTable();

        GDALColorEntry oEntry;
        int nNoDataIndex = -1;
        for (int iColor = nColorCount - 1; iColor >= 0; iColor--)
        {
            oEntry.c1 = pasPNGPalette[iColor].red;
            oEntry.c2 = pasPNGPalette[iColor].green;
            oEntry.c3 = pasPNGPalette[iColor].blue;

            if (iColor < num_trans)
            {
                oEntry.c4 = trans[iColor];
                if (oEntry.c4 == 0)
                {
                    if (nNoDataIndex == -1)
                        nNoDataIndex = iColor;
                    else
                        nNoDataIndex = -2;
                }
            }
            else
                oEntry.c4 = 255;

            poDS->poColorTable->SetColorEntry(iColor, &oEntry);
        }

        // Special hack to use an index as the no data value, as long as it is
        // the only transparent color in the palette.
        if (nNoDataIndex > -1)
        {
            poDS->GetRasterBand(1)->SetNoDataValue(nNoDataIndex);
        }
    }

    // Check for transparency values in greyscale images.
    if (poDS->nColorType == PNG_COLOR_TYPE_GRAY)
    {
        png_color_16 *trans_values = nullptr;
        unsigned char *trans;
        int num_trans;

        if (png_get_tRNS(poDS->hPNG, poDS->psPNGInfo, &trans, &num_trans,
                         &trans_values) != 0 &&
            trans_values != nullptr)
        {
            poDS->GetRasterBand(1)->SetNoDataValue(trans_values->gray);
        }
    }

    // Check for nodata color for RGB images.
    if (poDS->nColorType == PNG_COLOR_TYPE_RGB)
    {
        png_color_16 *trans_values = nullptr;
        unsigned char *trans;
        int num_trans;

        if (png_get_tRNS(poDS->hPNG, poDS->psPNGInfo, &trans, &num_trans,
                         &trans_values) != 0 &&
            trans_values != nullptr)
        {
            CPLString oNDValue;

            oNDValue.Printf("%d %d %d", trans_values->red, trans_values->green,
                            trans_values->blue);
            poDS->SetMetadataItem("NODATA_VALUES", oNDValue.c_str());

            poDS->GetRasterBand(1)->SetNoDataValue(trans_values->red);
            poDS->GetRasterBand(2)->SetNoDataValue(trans_values->green);
            poDS->GetRasterBand(3)->SetNoDataValue(trans_values->blue);
        }
    }

    png_color_16 *backgroundColor = nullptr;
    if (png_get_bKGD(poDS->hPNG, poDS->psPNGInfo, &backgroundColor) ==
            PNG_INFO_bKGD &&
        backgroundColor)
    {
        if (poDS->nColorType == PNG_COLOR_TYPE_GRAY ||
            poDS->nColorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        {
            poDS->SetMetadataItem("BACKGROUND_COLOR",
                                  CPLSPrintf("%d", backgroundColor->gray));
        }
        else if (poDS->nColorType == PNG_COLOR_TYPE_PALETTE)
        {
            poDS->SetMetadataItem("BACKGROUND_COLOR",
                                  CPLSPrintf("%d", backgroundColor->index));
        }
        else if (poDS->nColorType == PNG_COLOR_TYPE_RGB ||
                 poDS->nColorType == PNG_COLOR_TYPE_RGB_ALPHA)
        {
            poDS->SetMetadataItem("BACKGROUND_COLOR",
                                  CPLSPrintf("%d,%d,%d", backgroundColor->red,
                                             backgroundColor->green,
                                             backgroundColor->blue));
        }
    }

    // Extract any text chunks as "metadata."
    poDS->CollectMetadata();

    // More metadata.
    if (poDS->nBands > 1)
    {
        poDS->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    }

    // Initialize any PAM information.
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML(poOpenInfo->GetSiblingFiles());

    // Open overviews.
    poDS->oOvManager.Initialize(poDS, poOpenInfo);

    // Used by JPEG FLIR
    poDS->m_bByteOrderIsLittleEndian = CPLTestBool(CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "BYTE_ORDER_LITTLE_ENDIAN", "NO"));

    return poDS;
}

/************************************************************************/
/*                        LoadWorldFile()                               */
/************************************************************************/

void PNGDataset::LoadWorldFile()
{
    if (bHasTriedLoadWorldFile)
        return;
    bHasTriedLoadWorldFile = TRUE;

    char *pszWldFilename = nullptr;
    bGeoTransformValid =
        GDALReadWorldFile2(GetDescription(), nullptr, m_gt,
                           oOvManager.GetSiblingFiles(), &pszWldFilename);

    if (!bGeoTransformValid)
        bGeoTransformValid =
            GDALReadWorldFile2(GetDescription(), ".wld", m_gt,
                               oOvManager.GetSiblingFiles(), &pszWldFilename);

    if (pszWldFilename)
    {
        osWldFilename = pszWldFilename;
        CPLFree(pszWldFilename);
    }
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **PNGDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    LoadWorldFile();

    if (!osWldFilename.empty() &&
        CSLFindString(papszFileList, osWldFilename) == -1)
    {
        papszFileList = CSLAddString(papszFileList, osWldFilename);
    }

    return papszFileList;
}

/************************************************************************/
/*                          WriteMetadataAsText()                       */
/************************************************************************/

static bool IsASCII(const char *pszStr)
{
    for (int i = 0; pszStr[i] != '\0'; i++)
    {
        if (reinterpret_cast<GByte *>(const_cast<char *>(pszStr))[i] >= 128)
            return false;
    }
    return true;
}

static bool safe_png_set_text(jmp_buf sSetJmpContext, png_structp png_ptr,
                              png_infop info_ptr, png_const_textp text_ptr,
                              int num_text)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_text(png_ptr, info_ptr, text_ptr, num_text);
    return true;
}

void PNGDataset::WriteMetadataAsText(jmp_buf sSetJmpContext, png_structp hPNG,
                                     png_infop psPNGInfo, const char *pszKey,
                                     const char *pszValue)
{
    png_text sText;
    memset(&sText, 0, sizeof(png_text));
    sText.compression = PNG_TEXT_COMPRESSION_NONE;
    sText.key = const_cast<png_charp>(pszKey);
    sText.text = const_cast<png_charp>(pszValue);

    // UTF-8 values should be written in iTXt, whereas TEXT should be LATIN-1.
    if (!IsASCII(pszValue) && CPLIsUTF8(pszValue, -1))
        sText.compression = PNG_ITXT_COMPRESSION_NONE;

    safe_png_set_text(sSetJmpContext, hPNG, psPNGInfo, &sText, 1);
}

static bool safe_png_set_IHDR(jmp_buf sSetJmpContext, png_structp png_ptr,
                              png_infop info_ptr, png_uint_32 width,
                              png_uint_32 height, int bit_depth, int color_type,
                              int interlace_type, int compression_type,
                              int filter_type)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_IHDR(png_ptr, info_ptr, width, height, bit_depth, color_type,
                 interlace_type, compression_type, filter_type);
    return true;
}

static bool safe_png_set_compression_level(jmp_buf sSetJmpContext,
                                           png_structp png_ptr, int level)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_compression_level(png_ptr, level);
    return true;
}

static bool safe_png_set_tRNS(jmp_buf sSetJmpContext, png_structp png_ptr,
                              png_infop info_ptr, png_const_bytep trans,
                              int num_trans, png_color_16p trans_values)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_tRNS(png_ptr, info_ptr, trans, num_trans, trans_values);
    return true;
}

static bool safe_png_set_bKGD(jmp_buf sSetJmpContext, png_structp png_ptr,
                              png_infop info_ptr,
                              png_const_color_16p background)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_bKGD(png_ptr, info_ptr, background);
    return true;
}

static bool safe_png_set_iCCP(jmp_buf sSetJmpContext, png_structp png_ptr,
                              png_infop info_ptr, png_const_charp name,
                              int compression_type, png_const_bytep profile,
                              png_uint_32 proflen)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_iCCP(png_ptr, info_ptr, name, compression_type, profile, proflen);
    return true;
}

static bool safe_png_set_PLTE(jmp_buf sSetJmpContext, png_structp png_ptr,
                              png_infop info_ptr, png_const_colorp palette,
                              int num_palette)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_set_PLTE(png_ptr, info_ptr, palette, num_palette);
    return true;
}

static bool safe_png_write_info(jmp_buf sSetJmpContext, png_structp png_ptr,
                                png_infop info_ptr)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_write_info(png_ptr, info_ptr);
    return true;
}

static bool safe_png_write_rows(jmp_buf sSetJmpContext, png_structp png_ptr,
                                png_bytepp row, png_uint_32 num_rows)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_write_rows(png_ptr, row, num_rows);
    return true;
}

static bool safe_png_write_end(jmp_buf sSetJmpContext, png_structp png_ptr,
                               png_infop info_ptr)
{
    if (setjmp(sSetJmpContext) != 0)
    {
        return false;
    }
    png_write_end(png_ptr, info_ptr);
    return true;
}

/************************************************************************/
/*                             CreateCopy()                             */
/************************************************************************/

GDALDataset *PNGDataset::CreateCopy(const char *pszFilename,
                                    GDALDataset *poSrcDS, int bStrict,
                                    char **papszOptions,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData)

{
    // Perform some rudimentary checks.
    const int nBands = poSrcDS->GetRasterCount();
    if (nBands != 1 && nBands != 2 && nBands != 3 && nBands != 4)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "PNG driver doesn't support %d bands.  Must be 1 (grey),\n"
                 "2 (grey+alpha), 3 (rgb) or 4 (rgba) bands.\n",
                 nBands);

        return nullptr;
    }

    if (poSrcDS->GetRasterBand(1)->GetRasterDataType() != GDT_Byte &&
        poSrcDS->GetRasterBand(1)->GetRasterDataType() != GDT_UInt16)
    {
        CPLError(
            (bStrict) ? CE_Failure : CE_Warning, CPLE_NotSupported,
            "PNG driver doesn't support data type %s. "
            "Only eight bit (Byte) and sixteen bit (UInt16) bands supported. "
            "%s\n",
            GDALGetDataTypeName(poSrcDS->GetRasterBand(1)->GetRasterDataType()),
            (bStrict) ? "" : "Defaulting to Byte");

        if (bStrict)
            return nullptr;
    }

    // Create the dataset.
    VSIVirtualHandleUniquePtr fpImage(
        CPLTestBool(CSLFetchNameValueDef(
            papszOptions, "@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME", "NO"))
            ? VSIFileManager::GetHandler(pszFilename)
                  ->CreateOnlyVisibleAtCloseTime(pszFilename, true, nullptr)
            : VSIFOpenL(pszFilename, "wb"));
    if (fpImage == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Unable to create png file %s: %s\n", pszFilename,
                 VSIStrerror(errno));
        return nullptr;
    }

    // Initialize PNG access to the file.
    jmp_buf sSetJmpContext;

    png_structp hPNG =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, &sSetJmpContext,
                                png_gdal_error, png_gdal_warning);
    png_infop psPNGInfo = png_create_info_struct(hPNG);

    // Set up some parameters.
    int nColorType = 0;

    if (nBands == 1 && poSrcDS->GetRasterBand(1)->GetColorTable() == nullptr)
        nColorType = PNG_COLOR_TYPE_GRAY;
    else if (nBands == 1)
        nColorType = PNG_COLOR_TYPE_PALETTE;
    else if (nBands == 2)
        nColorType = PNG_COLOR_TYPE_GRAY_ALPHA;
    else if (nBands == 3)
        nColorType = PNG_COLOR_TYPE_RGB;
    else if (nBands == 4)
        nColorType = PNG_COLOR_TYPE_RGB_ALPHA;

    int nBitDepth;
    GDALDataType eType;
    if (poSrcDS->GetRasterBand(1)->GetRasterDataType() != GDT_UInt16)
    {
        eType = GDT_Byte;
        nBitDepth = 8;
        if (nBands == 1)
        {
            const char *pszNbits = poSrcDS->GetRasterBand(1)->GetMetadataItem(
                "NBITS", "IMAGE_STRUCTURE");
            if (pszNbits != nullptr)
            {
                nBitDepth = atoi(pszNbits);
                if (!(nBitDepth == 1 || nBitDepth == 2 || nBitDepth == 4))
                    nBitDepth = 8;
            }
        }
    }
    else
    {
        eType = GDT_UInt16;
        nBitDepth = 16;
    }

    const char *pszNbits = CSLFetchNameValue(papszOptions, "NBITS");
    if (eType == GDT_Byte && pszNbits != nullptr)
    {
        nBitDepth = atoi(pszNbits);
        if (!(nBitDepth == 1 || nBitDepth == 2 || nBitDepth == 4 ||
              nBitDepth == 8))
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Invalid bit depth. Using 8");
            nBitDepth = 8;
        }
    }

    png_set_write_fn(hPNG, fpImage.get(), png_vsi_write_data, png_vsi_flush);

    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();

    if (!safe_png_set_IHDR(sSetJmpContext, hPNG, psPNGInfo, nXSize, nYSize,
                           nBitDepth, nColorType, PNG_INTERLACE_NONE,
                           PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE))
    {
        fpImage->CancelCreation();
        png_destroy_write_struct(&hPNG, &psPNGInfo);
        return nullptr;
    }

    // Do we want to control the compression level?
    const char *pszLevel = CSLFetchNameValue(papszOptions, "ZLEVEL");

    if (pszLevel)
    {
        const int nLevel = atoi(pszLevel);
        if (nLevel < 1 || nLevel > 9)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Illegal ZLEVEL value '%s', should be 1-9.", pszLevel);
            fpImage->CancelCreation();
            png_destroy_write_struct(&hPNG, &psPNGInfo);
            return nullptr;
        }

        if (!safe_png_set_compression_level(sSetJmpContext, hPNG, nLevel))
        {
            fpImage->CancelCreation();
            png_destroy_write_struct(&hPNG, &psPNGInfo);
            return nullptr;
        }
    }

    // Try to handle nodata values as a tRNS block (note that for paletted
    // images, we save the effect to apply as part of palette).
    png_color_16 sTRNSColor;

    // Gray nodata.
    if (nColorType == PNG_COLOR_TYPE_GRAY)
    {
        int bHaveNoData = FALSE;
        const double dfNoDataValue =
            poSrcDS->GetRasterBand(1)->GetNoDataValue(&bHaveNoData);

        if (bHaveNoData && dfNoDataValue >= 0 && dfNoDataValue < 65536)
        {
            sTRNSColor.gray = static_cast<png_uint_16>(dfNoDataValue);
            if (!safe_png_set_tRNS(sSetJmpContext, hPNG, psPNGInfo, nullptr, 0,
                                   &sTRNSColor))
            {
                fpImage->CancelCreation();
                png_destroy_write_struct(&hPNG, &psPNGInfo);
                return nullptr;
            }
        }
    }

    // RGB nodata.
    if (nColorType == PNG_COLOR_TYPE_RGB)
    {
        // First try to use the NODATA_VALUES metadata item.
        if (poSrcDS->GetMetadataItem("NODATA_VALUES") != nullptr)
        {
            char **papszValues =
                CSLTokenizeString(poSrcDS->GetMetadataItem("NODATA_VALUES"));

            if (CSLCount(papszValues) >= 3)
            {
                sTRNSColor.red = static_cast<png_uint_16>(atoi(papszValues[0]));
                sTRNSColor.green =
                    static_cast<png_uint_16>(atoi(papszValues[1]));
                sTRNSColor.blue =
                    static_cast<png_uint_16>(atoi(papszValues[2]));
                if (!safe_png_set_tRNS(sSetJmpContext, hPNG, psPNGInfo, nullptr,
                                       0, &sTRNSColor))
                {
                    fpImage->CancelCreation();
                    png_destroy_write_struct(&hPNG, &psPNGInfo);
                    CSLDestroy(papszValues);
                    return nullptr;
                }
            }

            CSLDestroy(papszValues);
        }
        // Otherwise, get the nodata value from the bands.
        else
        {
            int bHaveNoDataRed = FALSE;
            const double dfNoDataValueRed =
                poSrcDS->GetRasterBand(1)->GetNoDataValue(&bHaveNoDataRed);

            int bHaveNoDataGreen = FALSE;
            const double dfNoDataValueGreen =
                poSrcDS->GetRasterBand(2)->GetNoDataValue(&bHaveNoDataGreen);

            int bHaveNoDataBlue = FALSE;
            const double dfNoDataValueBlue =
                poSrcDS->GetRasterBand(3)->GetNoDataValue(&bHaveNoDataBlue);

            if ((bHaveNoDataRed && dfNoDataValueRed >= 0 &&
                 dfNoDataValueRed < 65536) &&
                (bHaveNoDataGreen && dfNoDataValueGreen >= 0 &&
                 dfNoDataValueGreen < 65536) &&
                (bHaveNoDataBlue && dfNoDataValueBlue >= 0 &&
                 dfNoDataValueBlue < 65536))
            {
                sTRNSColor.red = static_cast<png_uint_16>(dfNoDataValueRed);
                sTRNSColor.green = static_cast<png_uint_16>(dfNoDataValueGreen);
                sTRNSColor.blue = static_cast<png_uint_16>(dfNoDataValueBlue);
                if (!safe_png_set_tRNS(sSetJmpContext, hPNG, psPNGInfo, nullptr,
                                       0, &sTRNSColor))
                {
                    fpImage->CancelCreation();
                    png_destroy_write_struct(&hPNG, &psPNGInfo);
                    return nullptr;
                }
            }
        }
    }

    if (const char *pszBackgroundColor =
            poSrcDS->GetMetadataItem("BACKGROUND_COLOR"))
    {
        bool ret_set_bKGD = true;
        png_color_16 backgroundColor = {0, 0, 0, 0, 0};
        if (nColorType == PNG_COLOR_TYPE_GRAY ||
            nColorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        {
            backgroundColor.gray =
                static_cast<png_uint_16>(atoi(pszBackgroundColor));
            ret_set_bKGD = safe_png_set_bKGD(sSetJmpContext, hPNG, psPNGInfo,
                                             &backgroundColor);
        }
        else if (nColorType == PNG_COLOR_TYPE_PALETTE)
        {
            backgroundColor.index =
                static_cast<png_byte>(atoi(pszBackgroundColor));
            ret_set_bKGD = safe_png_set_bKGD(sSetJmpContext, hPNG, psPNGInfo,
                                             &backgroundColor);
        }
        else if (nColorType == PNG_COLOR_TYPE_RGB ||
                 nColorType == PNG_COLOR_TYPE_RGB_ALPHA)
        {
            const CPLStringList aosTokens(
                CSLTokenizeString2(pszBackgroundColor, " ,", 3));
            if (aosTokens.size() == 3)
            {
                backgroundColor.red =
                    static_cast<png_uint_16>(atoi(aosTokens[0]));
                backgroundColor.green =
                    static_cast<png_uint_16>(atoi(aosTokens[1]));
                backgroundColor.blue =
                    static_cast<png_uint_16>(atoi(aosTokens[2]));
                ret_set_bKGD = safe_png_set_bKGD(sSetJmpContext, hPNG,
                                                 psPNGInfo, &backgroundColor);
            }
        }
        if (!ret_set_bKGD)
        {
            fpImage->CancelCreation();
            png_destroy_write_struct(&hPNG, &psPNGInfo);
            return nullptr;
        }
    }

    // Copy color profile data.
    const char *pszICCProfile =
        CSLFetchNameValue(papszOptions, "SOURCE_ICC_PROFILE");
    const char *pszICCProfileName =
        CSLFetchNameValue(papszOptions, "SOURCE_ICC_PROFILE_NAME");
    if (pszICCProfileName == nullptr)
        pszICCProfileName = poSrcDS->GetMetadataItem("SOURCE_ICC_PROFILE_NAME",
                                                     "COLOR_PROFILE");

    if (pszICCProfile == nullptr)
        pszICCProfile =
            poSrcDS->GetMetadataItem("SOURCE_ICC_PROFILE", "COLOR_PROFILE");

    if ((pszICCProfileName != nullptr) && EQUAL(pszICCProfileName, "sRGB"))
    {
        pszICCProfile = nullptr;

        // assumes this can't fail ?
        png_set_sRGB(hPNG, psPNGInfo, PNG_sRGB_INTENT_PERCEPTUAL);
    }

    if (pszICCProfile != nullptr)
    {
        char *pEmbedBuffer = CPLStrdup(pszICCProfile);
        png_uint_32 nEmbedLen =
            CPLBase64DecodeInPlace(reinterpret_cast<GByte *>(pEmbedBuffer));
        const char *pszLocalICCProfileName =
            (pszICCProfileName != nullptr) ? pszICCProfileName : "ICC Profile";

        if (!safe_png_set_iCCP(
                sSetJmpContext, hPNG, psPNGInfo, pszLocalICCProfileName, 0,
                reinterpret_cast<png_const_bytep>(pEmbedBuffer), nEmbedLen))
        {
            CPLFree(pEmbedBuffer);
            fpImage->CancelCreation();
            png_destroy_write_struct(&hPNG, &psPNGInfo);
            return nullptr;
        }

        CPLFree(pEmbedBuffer);
    }
    else if ((pszICCProfileName == nullptr) ||
             !EQUAL(pszICCProfileName, "sRGB"))
    {
        // Output gamma, primaries and whitepoint.
        const char *pszGamma = CSLFetchNameValue(papszOptions, "PNG_GAMMA");
        if (pszGamma == nullptr)
            pszGamma = poSrcDS->GetMetadataItem("PNG_GAMMA", "COLOR_PROFILE");

        if (pszGamma != nullptr)
        {
            double dfGamma = CPLAtof(pszGamma);
            // assumes this can't fail ?
            png_set_gAMA(hPNG, psPNGInfo, dfGamma);
        }

        const char *pszPrimariesRed =
            CSLFetchNameValue(papszOptions, "SOURCE_PRIMARIES_RED");
        if (pszPrimariesRed == nullptr)
            pszPrimariesRed = poSrcDS->GetMetadataItem("SOURCE_PRIMARIES_RED",
                                                       "COLOR_PROFILE");
        const char *pszPrimariesGreen =
            CSLFetchNameValue(papszOptions, "SOURCE_PRIMARIES_GREEN");
        if (pszPrimariesGreen == nullptr)
            pszPrimariesGreen = poSrcDS->GetMetadataItem(
                "SOURCE_PRIMARIES_GREEN", "COLOR_PROFILE");
        const char *pszPrimariesBlue =
            CSLFetchNameValue(papszOptions, "SOURCE_PRIMARIES_BLUE");
        if (pszPrimariesBlue == nullptr)
            pszPrimariesBlue = poSrcDS->GetMetadataItem("SOURCE_PRIMARIES_BLUE",
                                                        "COLOR_PROFILE");
        const char *pszWhitepoint =
            CSLFetchNameValue(papszOptions, "SOURCE_WHITEPOINT");
        if (pszWhitepoint == nullptr)
            pszWhitepoint =
                poSrcDS->GetMetadataItem("SOURCE_WHITEPOINT", "COLOR_PROFILE");

        if ((pszPrimariesRed != nullptr) && (pszPrimariesGreen != nullptr) &&
            (pszPrimariesBlue != nullptr) && (pszWhitepoint != nullptr))
        {
            bool bOk = true;
            double faColour[8] = {0.0};
            char **apapszTokenList[4] = {nullptr};

            apapszTokenList[0] = CSLTokenizeString2(pszWhitepoint, ",",
                                                    CSLT_ALLOWEMPTYTOKENS |
                                                        CSLT_STRIPLEADSPACES |
                                                        CSLT_STRIPENDSPACES);
            apapszTokenList[1] = CSLTokenizeString2(pszPrimariesRed, ",",
                                                    CSLT_ALLOWEMPTYTOKENS |
                                                        CSLT_STRIPLEADSPACES |
                                                        CSLT_STRIPENDSPACES);
            apapszTokenList[2] = CSLTokenizeString2(pszPrimariesGreen, ",",
                                                    CSLT_ALLOWEMPTYTOKENS |
                                                        CSLT_STRIPLEADSPACES |
                                                        CSLT_STRIPENDSPACES);
            apapszTokenList[3] = CSLTokenizeString2(pszPrimariesBlue, ",",
                                                    CSLT_ALLOWEMPTYTOKENS |
                                                        CSLT_STRIPLEADSPACES |
                                                        CSLT_STRIPENDSPACES);

            if ((CSLCount(apapszTokenList[0]) == 3) &&
                (CSLCount(apapszTokenList[1]) == 3) &&
                (CSLCount(apapszTokenList[2]) == 3) &&
                (CSLCount(apapszTokenList[3]) == 3))
            {
                for (int i = 0; i < 4; i++)
                {
                    for (int j = 0; j < 3; j++)
                    {
                        const double v = CPLAtof(apapszTokenList[i][j]);

                        if (j == 2)
                        {
                            /* Last term of xyY colour must be 1.0 */
                            if (v != 1.0)
                            {
                                bOk = false;
                                break;
                            }
                        }
                        else
                        {
                            faColour[i * 2 + j] = v;
                        }
                    }
                    if (!bOk)
                        break;
                }

                if (bOk)
                {
                    // assumes this can't fail ?
                    png_set_cHRM(hPNG, psPNGInfo, faColour[0], faColour[1],
                                 faColour[2], faColour[3], faColour[4],
                                 faColour[5], faColour[6], faColour[7]);
                }
            }

            CSLDestroy(apapszTokenList[0]);
            CSLDestroy(apapszTokenList[1]);
            CSLDestroy(apapszTokenList[2]);
            CSLDestroy(apapszTokenList[3]);
        }
    }

    // Write the palette if there is one. Technically, it may be possible to
    // write 16-bit palettes for PNG, but for now, this is omitted.
    if (nColorType == PNG_COLOR_TYPE_PALETTE)
    {
        int bHaveNoData = FALSE;
        double dfNoDataValue =
            poSrcDS->GetRasterBand(1)->GetNoDataValue(&bHaveNoData);

        GDALColorTable *poCT = poSrcDS->GetRasterBand(1)->GetColorTable();

        int nEntryCount = poCT->GetColorEntryCount();
        int nMaxEntryCount = 1 << nBitDepth;
        if (nEntryCount > nMaxEntryCount)
            nEntryCount = nMaxEntryCount;

        png_color *pasPNGColors = reinterpret_cast<png_color *>(
            CPLMalloc(sizeof(png_color) * nEntryCount));

        GDALColorEntry sEntry;
        bool bFoundTrans = false;
        for (int iColor = 0; iColor < nEntryCount; iColor++)
        {
            poCT->GetColorEntryAsRGB(iColor, &sEntry);
            if (sEntry.c4 != 255)
                bFoundTrans = true;

            pasPNGColors[iColor].red = static_cast<png_byte>(sEntry.c1);
            pasPNGColors[iColor].green = static_cast<png_byte>(sEntry.c2);
            pasPNGColors[iColor].blue = static_cast<png_byte>(sEntry.c3);
        }

        if (!safe_png_set_PLTE(sSetJmpContext, hPNG, psPNGInfo, pasPNGColors,
                               nEntryCount))
        {
            CPLFree(pasPNGColors);
            fpImage->CancelCreation();
            png_destroy_write_struct(&hPNG, &psPNGInfo);
            return nullptr;
        }

        CPLFree(pasPNGColors);

        // If we have transparent elements in the palette, we need to write a
        // transparency block.
        if (bFoundTrans || bHaveNoData)
        {
            unsigned char *pabyAlpha =
                static_cast<unsigned char *>(CPLMalloc(nEntryCount));

            for (int iColor = 0; iColor < nEntryCount; iColor++)
            {
                poCT->GetColorEntryAsRGB(iColor, &sEntry);
                pabyAlpha[iColor] = static_cast<unsigned char>(sEntry.c4);

                if (bHaveNoData && iColor == static_cast<int>(dfNoDataValue))
                    pabyAlpha[iColor] = 0;
            }

            if (!safe_png_set_tRNS(sSetJmpContext, hPNG, psPNGInfo, pabyAlpha,
                                   nEntryCount, nullptr))
            {
                CPLFree(pabyAlpha);
                fpImage->CancelCreation();
                png_destroy_write_struct(&hPNG, &psPNGInfo);
                return nullptr;
            }

            CPLFree(pabyAlpha);
        }
    }

    // Add text info.
    // These are predefined keywords. See "4.2.7 tEXt Textual data" of
    // http://www.w3.org/TR/PNG-Chunks.html for more information.
    const char *apszKeywords[] = {"Title",      "Author",        "Description",
                                  "Copyright",  "Creation Time", "Software",
                                  "Disclaimer", "Warning",       "Source",
                                  "Comment",    nullptr};
    const bool bWriteMetadataAsText = CPLTestBool(
        CSLFetchNameValueDef(papszOptions, "WRITE_METADATA_AS_TEXT", "FALSE"));
    for (int i = 0; apszKeywords[i] != nullptr; i++)
    {
        const char *pszKey = apszKeywords[i];
        const char *pszValue = CSLFetchNameValue(papszOptions, pszKey);
        if (pszValue == nullptr && bWriteMetadataAsText)
            pszValue = poSrcDS->GetMetadataItem(pszKey);
        if (pszValue != nullptr)
        {
            WriteMetadataAsText(sSetJmpContext, hPNG, psPNGInfo, pszKey,
                                pszValue);
        }
    }
    if (bWriteMetadataAsText)
    {
        char **papszSrcMD = poSrcDS->GetMetadata();
        for (; papszSrcMD && *papszSrcMD; papszSrcMD++)
        {
            char *pszKey = nullptr;
            const char *pszValue = CPLParseNameValue(*papszSrcMD, &pszKey);
            if (pszKey && pszValue)
            {
                if (CSLFindString(const_cast<char **>(apszKeywords), pszKey) <
                        0 &&
                    !EQUAL(pszKey, "AREA_OR_POINT") &&
                    !EQUAL(pszKey, "NODATA_VALUES"))
                {
                    WriteMetadataAsText(sSetJmpContext, hPNG, psPNGInfo, pszKey,
                                        pszValue);
                }
                CPLFree(pszKey);
            }
        }
    }

    // Write the PNG info.
    if (!safe_png_write_info(sSetJmpContext, hPNG, psPNGInfo))
    {
        fpImage->CancelCreation();
        png_destroy_write_struct(&hPNG, &psPNGInfo);
        return nullptr;
    }

    if (nBitDepth < 8)
    {
        // Assumes this can't fail
        png_set_packing(hPNG);
    }

    // Loop over the image, copying image data.
    CPLErr eErr = CE_None;
    const int nWordSize = GDALGetDataTypeSizeBytes(eType);

    GByte *pabyScanline = reinterpret_cast<GByte *>(
        CPLMalloc(cpl::fits_on<int>(nBands * nXSize * nWordSize)));

    for (int iLine = 0; iLine < nYSize && eErr == CE_None; iLine++)
    {
        png_bytep row = pabyScanline;

        eErr = poSrcDS->RasterIO(
            GF_Read, 0, iLine, nXSize, 1, pabyScanline, nXSize, 1, eType,
            nBands, nullptr, static_cast<GSpacing>(nBands) * nWordSize,
            static_cast<GSpacing>(nBands) * nXSize * nWordSize, nWordSize,
            nullptr);

#ifdef CPL_LSB
        if (nBitDepth == 16)
            GDALSwapWords(row, 2, nXSize * nBands, 2);
#endif
        if (eErr == CE_None)
        {
            if (!safe_png_write_rows(sSetJmpContext, hPNG, &row, 1))
            {
                eErr = CE_Failure;
            }
        }

        if (eErr == CE_None &&
            !pfnProgress((iLine + 1) / static_cast<double>(nYSize), nullptr,
                         pProgressData))
        {
            eErr = CE_Failure;
            CPLError(CE_Failure, CPLE_UserInterrupt,
                     "User terminated CreateCopy()");
        }
    }

    CPLFree(pabyScanline);

    if (!safe_png_write_end(sSetJmpContext, hPNG, psPNGInfo))
    {
        eErr = CE_Failure;
    }
    png_destroy_write_struct(&hPNG, &psPNGInfo);

    if (eErr == CE_None)
    {
        if (fpImage->Close() != 0)
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Error at file closing of '%s': %s", pszFilename,
                     VSIStrerror(errno));
            eErr = CE_Failure;
        }
    }
    else
    {
        fpImage->CancelCreation();
        fpImage.reset();
    }

    if (eErr != CE_None)
        return nullptr;

    // Do we need a world file?
    if (CPLFetchBool(papszOptions, "WORLDFILE", false))
    {
        GDALGeoTransform gt;
        if (poSrcDS->GetGeoTransform(gt) == CE_None)
            GDALWriteWorldFile(pszFilename, "wld", gt.data());
    }

    // Re-open dataset and copy any auxiliary PAM information.

    /* If writing to stdout, we can't reopen it, so return */
    /* a fake dataset to make the caller happy */
    if (CPLTestBool(CPLGetConfigOption("GDAL_OPEN_AFTER_COPY", "YES")))
    {
        CPLPushErrorHandler(CPLQuietErrorHandler);
        GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
        PNGDataset *poDS =
            cpl::down_cast<PNGDataset *>(PNGDataset::Open(&oOpenInfo));
        CPLPopErrorHandler();
        if (poDS)
        {
            int nFlags = GCIF_PAM_DEFAULT & ~GCIF_METADATA;
            poDS->CloneInfo(poSrcDS, nFlags);

            char **papszExcludedDomains =
                CSLAddString(nullptr, "COLOR_PROFILE");
            if (bWriteMetadataAsText)
                papszExcludedDomains = CSLAddString(papszExcludedDomains, "");
            GDALDriver::DefaultCopyMetadata(poSrcDS, poDS, papszOptions,
                                            papszExcludedDomains);
            CSLDestroy(papszExcludedDomains);

            return poDS;
        }
        CPLErrorReset();
    }

    PNGDataset *poPNG_DS = new PNGDataset();
    poPNG_DS->nRasterXSize = nXSize;
    poPNG_DS->nRasterYSize = nYSize;
    poPNG_DS->nBitDepth = nBitDepth;
    for (int i = 0; i < nBands; i++)
        poPNG_DS->SetBand(i + 1, new PNGRasterBand(poPNG_DS, i + 1));
    return poPNG_DS;
}

/************************************************************************/
/*                         png_vsi_read_data()                          */
/*                                                                      */
/*      Read data callback through VSI.                                 */
/************************************************************************/
static void png_vsi_read_data(png_structp png_ptr, png_bytep data,
                              png_size_t length)

{
    // fread() returns 0 on error, so it is OK to store this in a png_size_t
    // instead of an int, which is what fread() actually returns.
    const png_size_t check = static_cast<png_size_t>(
        VSIFReadL(data, 1, length,
                  reinterpret_cast<VSILFILE *>(png_get_io_ptr(png_ptr))));

    if (check != length)
        png_error(png_ptr, "Read Error");
}

/************************************************************************/
/*                         png_vsi_write_data()                         */
/************************************************************************/

static void png_vsi_write_data(png_structp png_ptr, png_bytep data,
                               png_size_t length)
{
    const size_t check = VSIFWriteL(
        data, 1, length, reinterpret_cast<VSILFILE *>(png_get_io_ptr(png_ptr)));

    if (check != length)
        png_error(png_ptr, "Write Error");
}

/************************************************************************/
/*                           png_vsi_flush()                            */
/************************************************************************/
static void png_vsi_flush(png_structp png_ptr)
{
    VSIFFlushL(reinterpret_cast<VSILFILE *>(png_get_io_ptr(png_ptr)));
}

/************************************************************************/
/*                           png_gdal_error()                           */
/************************************************************************/

static void png_gdal_error(png_structp png_ptr, const char *error_message)
{
    CPLError(CE_Failure, CPLE_AppDefined, "libpng: %s", error_message);

    // Use longjmp instead of a C++ exception, because libpng is generally not
    // built as C++ and so will not honor unwind semantics.

    jmp_buf *psSetJmpContext =
        reinterpret_cast<jmp_buf *>(png_get_error_ptr(png_ptr));
    if (psSetJmpContext)
    {
        longjmp(*psSetJmpContext, 1);
    }
}

/************************************************************************/
/*                          png_gdal_warning()                          */
/************************************************************************/

static void png_gdal_warning(CPL_UNUSED png_structp png_ptr,
                             const char *error_message)
{
    CPLError(CE_Warning, CPLE_AppDefined, "libpng: %s", error_message);
}

/************************************************************************/
/*                          GDALRegister_PNG()                          */
/************************************************************************/

void GDALRegister_PNG()

{
    if (GDALGetDriverByName(DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();
    PNGDriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = PNGDataset::Open;
    poDriver->pfnCreateCopy = PNGDataset::CreateCopy;
#ifdef SUPPORT_CREATE
    poDriver->pfnCreate = PNGDataset::Create;
#endif

    GetGDALDriverManager()->RegisterDriver(poDriver);
}

#ifdef SUPPORT_CREATE
/************************************************************************/
/*                         IWriteBlock()                                */
/************************************************************************/

CPLErr PNGRasterBand::IWriteBlock(int x, int y, void *pvData)
{
    PNGDataset &ds = *cpl::down_cast<PNGDataset *>(poDS);

    // Write the block (or consolidate into multichannel block) and then write.

    const GDALDataType dt = GetRasterDataType();
    const size_t wordsize = ds.m_nBitDepth / 8;
    GDALCopyWords(pvData, dt, wordsize,
                  ds.m_pabyBuffer + (nBand - 1) * wordsize, dt,
                  ds.nBands * wordsize, nBlockXSize);

    // See if we have all the bands.
    m_bBandProvided[nBand - 1] = TRUE;
    for (size_t i = 0; i < static_cast<size_t>(ds.nBands); i++)
    {
        if (!m_bBandProvided[i])
            return CE_None;
    }

    // We received all the bands, so reset band flags and write pixels out.
    this->reset_band_provision_flags();

    // If it is the first block, write out the file header.
    if (x == 0 && y == 0)
    {
        CPLErr err = ds.write_png_header();
        if (err != CE_None)
            return err;
    }

#ifdef CPL_LSB
    if (ds.m_nBitDepth == 16)
        GDALSwapWords(ds.m_pabyBuffer, 2, nBlockXSize * ds.nBands, 2);
#endif
    png_write_rows(ds.m_hPNG, &ds.m_pabyBuffer, 1);

    return CE_None;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr PNGDataset::SetGeoTransform(const GDALGeoTransform &gt)
{
    m_gt = gt;

    if (m_pszFilename)
    {
        if (GDALWriteWorldFile(m_pszFilename, "wld", m_gt.data()) == FALSE)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Can't write world file.");
            return CE_Failure;
        }
    }

    return CE_None;
}

/************************************************************************/
/*                           SetColorTable()                            */
/************************************************************************/

CPLErr PNGRasterBand::SetColorTable(GDALColorTable *poCT)
{
    if (poCT == NULL)
        return CE_Failure;

    // We get called even for grayscale files, since some formats need a palette
    // even then. PNG doesn't, so if a gray palette is given, just ignore it.

    GDALColorEntry sEntry;
    for (size_t i = 0; i < static_cast<size_t>(poCT->GetColorEntryCount()); i++)
    {
        poCT->GetColorEntryAsRGB(i, &sEntry);
        if (sEntry.c1 != sEntry.c2 || sEntry.c1 != sEntry.c3)
        {
            CPLErr err = GDALPamRasterBand::SetColorTable(poCT);
            if (err != CE_None)
                return err;

            PNGDataset &ds = *cpl::down_cast<PNGDataset *>(poDS);
            ds.m_nColorType = PNG_COLOR_TYPE_PALETTE;
            break;
            // band::IWriteBlock will emit color table as part of the header
            // preceding the first block write.
        }
    }

    return CE_None;
}

/************************************************************************/
/*                  PNGDataset::write_png_header()                      */
/************************************************************************/

CPLErr PNGDataset::write_png_header()
{
    // Initialize PNG access to the file.
    m_hPNG = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
                                     png_gdal_error, png_gdal_warning);

    m_psPNGInfo = png_create_info_struct(m_hPNG);

    png_set_write_fn(m_hPNG, m_fpImage, png_vsi_write_data, png_vsi_flush);

    png_set_IHDR(m_hPNG, m_psPNGInfo, nRasterXSize, nRasterYSize, m_nBitDepth,
                 m_nColorType, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    png_set_compression_level(m_hPNG, Z_BEST_COMPRESSION);

    // png_set_swap_alpha(m_hPNG); // Use RGBA order, not ARGB.

    // Try to handle nodata values as a tRNS block (note that for paletted
    // images, we save the effect to apply as part of the palette).
    // m_bHaveNoData = FALSE;
    // m_dfNoDataValue = -1;
    png_color_16 sTRNSColor;

    int bHaveNoData = FALSE;
    double dfNoDataValue = -1;

    if (m_nColorType == PNG_COLOR_TYPE_GRAY)
    {
        dfNoDataValue = GetRasterBand(1)->GetNoDataValue(&bHaveNoData);

        if (bHaveNoData && dfNoDataValue >= 0 && dfNoDataValue < 65536)
        {
            sTRNSColor.gray = static_cast<png_uint_16>(dfNoDataValue);
            png_set_tRNS(m_hPNG, m_psPNGInfo, NULL, 0, &sTRNSColor);
        }
    }

    // RGB nodata.
    if (nColorType == PNG_COLOR_TYPE_RGB)
    {
        // First, try to use the NODATA_VALUES metadata item.
        if (GetMetadataItem("NODATA_VALUES") != NULL)
        {
            char **papszValues =
                CSLTokenizeString(GetMetadataItem("NODATA_VALUES"));

            if (CSLCount(papszValues) >= 3)
            {
                sTRNSColor.red = static_cast<png_uint_16>(atoi(papszValues[0]));
                sTRNSColor.green =
                    static_cast<png_uint_16>(atoi(papszValues[1]));
                sTRNSColor.blue =
                    static_cast<png_uint_16>(atoi(papszValues[2]));
                png_set_tRNS(m_hPNG, m_psPNGInfo, NULL, 0, &sTRNSColor);
            }

            CSLDestroy(papszValues);
        }
        // Otherwise, get the nodata value from the bands.
        else
        {
            int bHaveNoDataRed = FALSE;
            const double dfNoDataValueRed =
                GetRasterBand(1)->GetNoDataValue(&bHaveNoDataRed);

            int bHaveNoDataGreen = FALSE;
            const double dfNoDataValueGreen =
                GetRasterBand(2)->GetNoDataValue(&bHaveNoDataGreen);

            int bHaveNoDataBlue = FALSE;
            const double dfNoDataValueBlue =
                GetRasterBand(3)->GetNoDataValue(&bHaveNoDataBlue);

            if ((bHaveNoDataRed && dfNoDataValueRed >= 0 &&
                 dfNoDataValueRed < 65536) &&
                (bHaveNoDataGreen && dfNoDataValueGreen >= 0 &&
                 dfNoDataValueGreen < 65536) &&
                (bHaveNoDataBlue && dfNoDataValueBlue >= 0 &&
                 dfNoDataValueBlue < 65536))
            {
                sTRNSColor.red = static_cast<png_uint_16>(dfNoDataValueRed);
                sTRNSColor.green = static_cast<png_uint_16>(dfNoDataValueGreen);
                sTRNSColor.blue = static_cast<png_uint_16>(dfNoDataValueBlue);
                png_set_tRNS(m_hPNG, m_psPNGInfo, NULL, 0, &sTRNSColor);
            }
        }
    }

    // Write the palette if there is one. Technically, it may be possible
    // to write 16-bit palettes for PNG, but for now, doing so is omitted.
    if (nColorType == PNG_COLOR_TYPE_PALETTE)
    {
        GDALColorTable *poCT = GetRasterBand(1)->GetColorTable();

        int bHaveNoData = FALSE;
        double dfNoDataValue = GetRasterBand(1)->GetNoDataValue(&bHaveNoData);

        m_pasPNGColors = reinterpret_cast<png_color *>(
            CPLMalloc(sizeof(png_color) * poCT->GetColorEntryCount()));

        GDALColorEntry sEntry;
        bool bFoundTrans = false;
        for (int iColor = 0; iColor < poCT->GetColorEntryCount(); iColor++)
        {
            poCT->GetColorEntryAsRGB(iColor, &sEntry);
            if (sEntry.c4 != 255)
                bFoundTrans = true;

            m_pasPNGColors[iColor].red = static_cast<png_byte>(sEntry.c1);
            m_pasPNGColors[iColor].green = static_cast<png_byte>(sEntry.c2);
            m_pasPNGColors[iColor].blue = static_cast<png_byte>(sEntry.c3);
        }

        png_set_PLTE(m_hPNG, m_psPNGInfo, m_pasPNGColors,
                     poCT->GetColorEntryCount());

        // If we have transparent elements in the palette, we need to write a
        // transparency block.
        if (bFoundTrans || bHaveNoData)
        {
            m_pabyAlpha = reinterpret_cast<unsigned char *>(
                CPLMalloc(poCT->GetColorEntryCount()));

            for (int iColor = 0; iColor < poCT->GetColorEntryCount(); iColor++)
            {
                poCT->GetColorEntryAsRGB(iColor, &sEntry);
                m_pabyAlpha[iColor] = static_cast<unsigned char>(sEntry.c4);

                if (bHaveNoData && iColor == static_cast<int>(dfNoDataValue))
                    m_pabyAlpha[iColor] = 0;
            }

            png_set_tRNS(m_hPNG, m_psPNGInfo, m_pabyAlpha,
                         poCT->GetColorEntryCount(), NULL);
        }
    }

    png_write_info(m_hPNG, m_psPNGInfo);
    return CE_None;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

GDALDataset *PNGDataset::Create(const char *pszFilename, int nXSize, int nYSize,
                                int nBands, GDALDataType eType,
                                char **papszOptions)
{
    if (eType != GDT_Byte && eType != GDT_UInt16)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to create PNG dataset with an illegal\n"
            "data type (%s), only Byte and UInt16 supported by the format.\n",
            GDALGetDataTypeName(eType));

        return NULL;
    }

    if (nBands < 1 || nBands > 4)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "PNG driver doesn't support %d bands. "
                 "Must be 1 (gray/indexed color),\n"
                 "2 (gray+alpha), 3 (rgb) or 4 (rgba) bands.\n",
                 nBands);

        return NULL;
    }

    // Bands are:
    // 1: Grayscale or indexed color.
    // 2: Gray plus alpha.
    // 3: RGB.
    // 4: RGB plus alpha.

    if (nXSize < 1 || nYSize < 1)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Specified pixel dimensions (% d x %d) are bad.\n", nXSize,
                 nYSize);
    }

    // Set up some parameters.
    PNGDataset *poDS = new PNGDataset();

    poDS->nRasterXSize = nXSize;
    poDS->nRasterYSize = nYSize;
    poDS->eAccess = GA_Update;
    poDS->nBands = nBands;

    switch (nBands)
    {
        case 1:
            poDS->m_nColorType = PNG_COLOR_TYPE_GRAY;
            break;  // If a non-gray palette is set, we'll change this.

        case 2:
            poDS->m_nColorType = PNG_COLOR_TYPE_GRAY_ALPHA;
            break;

        case 3:
            poDS->m_nColorType = PNG_COLOR_TYPE_RGB;
            break;

        case 4:
            poDS->m_nColorType = PNG_COLOR_TYPE_RGB_ALPHA;
            break;
    }

    poDS->m_nBitDepth = (eType == GDT_Byte ? 8 : 16);

    poDS->m_pabyBuffer = reinterpret_cast<GByte *>(
        CPLMalloc(nBands * nXSize * poDS->m_nBitDepth / 8));

    // Create band information objects.
    for (int iBand = 1; iBand <= poDS->nBands; iBand++)
        poDS->SetBand(iBand, new PNGRasterBand(poDS, iBand));

    // Do we need a world file?
    if (CPLFetchBool(papszOptions, "WORLDFILE", false))
        poDS->m_bGeoTransformValid = TRUE;

    // Create the file.

    poDS->m_fpImage = VSIFOpenL(pszFilename, "wb");
    if (poDS->m_fpImage == NULL)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Unable to create PNG file %s: %s\n", pszFilename,
                 VSIStrerror(errno));
        delete poDS;
        return NULL;
    }

    poDS->m_pszFilename = CPLStrdup(pszFilename);

    return poDS;
}

#endif
