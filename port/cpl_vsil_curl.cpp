/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Implement VSI large file api for HTTP/FTP files
 * Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2010-2018, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_vsil_curl_priv.h"
#include "cpl_vsil_curl_class.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <set>

#include "cpl_aws.h"
#include "cpl_json.h"
#include "cpl_json_header.h"
#include "cpl_minixml.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "cpl_time.h"
#include "cpl_vsi.h"
#include "cpl_vsi_virtual.h"
#include "cpl_http.h"
#include "cpl_mem_cache.h"

#ifndef S_IRUSR
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#endif

#ifndef HAVE_CURL

void VSIInstallCurlFileHandler(void)
{
    // Not supported.
}

void VSICurlClearCache(void)
{
    // Not supported.
}

void VSICurlPartialClearCache(const char *)
{
    // Not supported.
}

void VSICurlAuthParametersChanged()
{
    // Not supported.
}

void VSINetworkStatsReset(void)
{
    // Not supported
}

char *VSINetworkStatsGetAsSerializedJSON(char ** /* papszOptions */)
{
    // Not supported
    return nullptr;
}

/************************************************************************/
/*                      VSICurlInstallReadCbk()                         */
/************************************************************************/

int VSICurlInstallReadCbk(VSILFILE * /* fp */,
                          VSICurlReadCbkFunc /* pfnReadCbk */,
                          void * /* pfnUserData */,
                          int /* bStopOnInterruptUntilUninstall */)
{
    return FALSE;
}

/************************************************************************/
/*                    VSICurlUninstallReadCbk()                         */
/************************************************************************/

int VSICurlUninstallReadCbk(VSILFILE * /* fp */)
{
    return FALSE;
}

#else

//! @cond Doxygen_Suppress
#ifndef DOXYGEN_SKIP

#define ENABLE_DEBUG 1
#define ENABLE_DEBUG_VERBOSE 0

#define unchecked_curl_easy_setopt(handle, opt, param)                         \
    CPL_IGNORE_RET_VAL(curl_easy_setopt(handle, opt, param))

/***********************************************************ù************/
/*                    VSICurlAuthParametersChanged()                    */
/************************************************************************/

static unsigned int gnGenerationAuthParameters = 0;

void VSICurlAuthParametersChanged()
{
    gnGenerationAuthParameters++;
}

// Do not access those variables directly !
// Use VSICURLGetDownloadChunkSize() and GetMaxRegions()
static int N_MAX_REGIONS_DO_NOT_USE_DIRECTLY = 0;
static int DOWNLOAD_CHUNK_SIZE_DO_NOT_USE_DIRECTLY = 0;

/************************************************************************/
/*                    VSICURLReadGlobalEnvVariables()                   */
/************************************************************************/

static void VSICURLReadGlobalEnvVariables()
{
    struct Initializer
    {
        Initializer()
        {
            constexpr int DOWNLOAD_CHUNK_SIZE_DEFAULT = 16384;
            const char *pszChunkSize =
                CPLGetConfigOption("CPL_VSIL_CURL_CHUNK_SIZE", nullptr);
            GIntBig nChunkSize = DOWNLOAD_CHUNK_SIZE_DEFAULT;

            if (pszChunkSize)
            {
                if (CPLParseMemorySize(pszChunkSize, &nChunkSize, nullptr) !=
                    CE_None)
                {
                    CPLError(
                        CE_Warning, CPLE_AppDefined,
                        "Could not parse value for CPL_VSIL_CURL_CHUNK_SIZE. "
                        "Using default value of %d instead.",
                        DOWNLOAD_CHUNK_SIZE_DEFAULT);
                }
            }

            constexpr int MIN_CHUNK_SIZE = 1024;
            constexpr int MAX_CHUNK_SIZE = 10 * 1024 * 1024;
            if (nChunkSize < MIN_CHUNK_SIZE || nChunkSize > MAX_CHUNK_SIZE)
            {
                nChunkSize = DOWNLOAD_CHUNK_SIZE_DEFAULT;
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Invalid value for CPL_VSIL_CURL_CHUNK_SIZE. "
                         "Allowed range is [%d, %d]. "
                         "Using CPL_VSIL_CURL_CHUNK_SIZE=%d instead",
                         MIN_CHUNK_SIZE, MAX_CHUNK_SIZE,
                         DOWNLOAD_CHUNK_SIZE_DEFAULT);
            }
            DOWNLOAD_CHUNK_SIZE_DO_NOT_USE_DIRECTLY =
                static_cast<int>(nChunkSize);

            constexpr int N_MAX_REGIONS_DEFAULT = 1000;
            constexpr int CACHE_SIZE_DEFAULT =
                N_MAX_REGIONS_DEFAULT * DOWNLOAD_CHUNK_SIZE_DEFAULT;

            const char *pszCacheSize =
                CPLGetConfigOption("CPL_VSIL_CURL_CACHE_SIZE", nullptr);
            GIntBig nCacheSize = CACHE_SIZE_DEFAULT;

            if (pszCacheSize)
            {
                if (CPLParseMemorySize(pszCacheSize, &nCacheSize, nullptr) !=
                    CE_None)
                {
                    CPLError(
                        CE_Warning, CPLE_AppDefined,
                        "Could not parse value for CPL_VSIL_CURL_CACHE_SIZE. "
                        "Using default value of " CPL_FRMT_GIB " instead.",
                        nCacheSize);
                }
            }

            const auto nMaxRAM = CPLGetUsablePhysicalRAM();
            const auto nMinVal = DOWNLOAD_CHUNK_SIZE_DO_NOT_USE_DIRECTLY;
            auto nMaxVal = static_cast<GIntBig>(INT_MAX) *
                           DOWNLOAD_CHUNK_SIZE_DO_NOT_USE_DIRECTLY;
            if (nMaxRAM > 0 && nMaxVal > nMaxRAM)
                nMaxVal = nMaxRAM;
            if (nCacheSize < nMinVal || nCacheSize > nMaxVal)
            {
                nCacheSize = nCacheSize < nMinVal ? nMinVal : nMaxVal;
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Invalid value for CPL_VSIL_CURL_CACHE_SIZE. "
                         "Allowed range is [%d, " CPL_FRMT_GIB "]. "
                         "Using CPL_VSIL_CURL_CACHE_SIZE=" CPL_FRMT_GIB
                         " instead",
                         nMinVal, nMaxVal, nCacheSize);
            }
            N_MAX_REGIONS_DO_NOT_USE_DIRECTLY = std::max(
                1, static_cast<int>(nCacheSize /
                                    DOWNLOAD_CHUNK_SIZE_DO_NOT_USE_DIRECTLY));
        }
    };

    static Initializer initializer;
}

/************************************************************************/
/*                     VSICURLGetDownloadChunkSize()                    */
/************************************************************************/

int VSICURLGetDownloadChunkSize()
{
    VSICURLReadGlobalEnvVariables();
    return DOWNLOAD_CHUNK_SIZE_DO_NOT_USE_DIRECTLY;
}

/************************************************************************/
/*                            GetMaxRegions()                           */
/************************************************************************/

static int GetMaxRegions()
{
    VSICURLReadGlobalEnvVariables();
    return N_MAX_REGIONS_DO_NOT_USE_DIRECTLY;
}

/************************************************************************/
/*          VSICurlFindStringSensitiveExceptEscapeSequences()           */
/************************************************************************/

static int
VSICurlFindStringSensitiveExceptEscapeSequences(char **papszList,
                                                const char *pszTarget)

{
    if (papszList == nullptr)
        return -1;

    for (int i = 0; papszList[i] != nullptr; i++)
    {
        const char *pszIter1 = papszList[i];
        const char *pszIter2 = pszTarget;
        char ch1 = '\0';
        char ch2 = '\0';
        /* The comparison is case-sensitive, escape for escaped */
        /* sequences where letters of the hexadecimal sequence */
        /* can be uppercase or lowercase depending on the quoting algorithm */
        while (true)
        {
            ch1 = *pszIter1;
            ch2 = *pszIter2;
            if (ch1 == '\0' || ch2 == '\0')
                break;
            if (ch1 == '%' && ch2 == '%' && pszIter1[1] != '\0' &&
                pszIter1[2] != '\0' && pszIter2[1] != '\0' &&
                pszIter2[2] != '\0')
            {
                if (!EQUALN(pszIter1 + 1, pszIter2 + 1, 2))
                    break;
                pszIter1 += 2;
                pszIter2 += 2;
            }
            if (ch1 != ch2)
                break;
            pszIter1++;
            pszIter2++;
        }
        if (ch1 == ch2 && ch1 == '\0')
            return i;
    }

    return -1;
}

/************************************************************************/
/*                      VSICurlIsFileInList()                           */
/************************************************************************/

static int VSICurlIsFileInList(char **papszList, const char *pszTarget)
{
    int nRet =
        VSICurlFindStringSensitiveExceptEscapeSequences(papszList, pszTarget);
    if (nRet >= 0)
        return nRet;

    // If we didn't find anything, try to URL-escape the target filename.
    char *pszEscaped = CPLEscapeString(pszTarget, -1, CPLES_URL);
    if (strcmp(pszTarget, pszEscaped) != 0)
    {
        nRet = VSICurlFindStringSensitiveExceptEscapeSequences(papszList,
                                                               pszEscaped);
    }
    CPLFree(pszEscaped);
    return nRet;
}

/************************************************************************/
/*                      VSICurlGetURLFromFilename()                     */
/************************************************************************/

static std::string VSICurlGetURLFromFilename(
    const char *pszFilename, CPLHTTPRetryParameters *poRetryParameters,
    bool *pbUseHead, bool *pbUseRedirectURLIfNoQueryStringParams,
    bool *pbListDir, bool *pbEmptyDir, CPLStringList *paosHTTPOptions,
    bool *pbPlanetaryComputerURLSigning, char **ppszPlanetaryComputerCollection)
{
    if (ppszPlanetaryComputerCollection)
        *ppszPlanetaryComputerCollection = nullptr;

    if (!STARTS_WITH(pszFilename, "/vsicurl/") &&
        !STARTS_WITH(pszFilename, "/vsicurl?"))
        return pszFilename;

    if (pbPlanetaryComputerURLSigning)
    {
        // It may be more convenient sometimes to store Planetary Computer URL
        // signing as a per-path specific option rather than capturing it in
        // the filename with the &pc_url_signing=yes option.
        if (CPLTestBool(VSIGetPathSpecificOption(
                pszFilename, "VSICURL_PC_URL_SIGNING", "FALSE")))
        {
            *pbPlanetaryComputerURLSigning = true;
        }
    }

    pszFilename += strlen("/vsicurl/");
    if (!STARTS_WITH(pszFilename, "http://") &&
        !STARTS_WITH(pszFilename, "https://") &&
        !STARTS_WITH(pszFilename, "ftp://") &&
        !STARTS_WITH(pszFilename, "file://"))
    {
        if (*pszFilename == '?')
            pszFilename++;
        char **papszTokens = CSLTokenizeString2(pszFilename, "&", 0);
        for (int i = 0; papszTokens[i] != nullptr; i++)
        {
            char *pszUnescaped =
                CPLUnescapeString(papszTokens[i], nullptr, CPLES_URL);
            CPLFree(papszTokens[i]);
            papszTokens[i] = pszUnescaped;
        }

        std::string osURL;
        std::string osHeaders;
        for (int i = 0; papszTokens[i]; i++)
        {
            char *pszKey = nullptr;
            const char *pszValue = CPLParseNameValue(papszTokens[i], &pszKey);
            if (pszKey && pszValue)
            {
                if (EQUAL(pszKey, "max_retry"))
                {
                    if (poRetryParameters)
                        poRetryParameters->nMaxRetry = atoi(pszValue);
                }
                else if (EQUAL(pszKey, "retry_delay"))
                {
                    if (poRetryParameters)
                        poRetryParameters->dfInitialDelay = CPLAtof(pszValue);
                }
                else if (EQUAL(pszKey, "retry_codes"))
                {
                    if (poRetryParameters)
                        poRetryParameters->osRetryCodes = pszValue;
                }
                else if (EQUAL(pszKey, "use_head"))
                {
                    if (pbUseHead)
                        *pbUseHead = CPLTestBool(pszValue);
                }
                else if (EQUAL(pszKey,
                               "use_redirect_url_if_no_query_string_params"))
                {
                    /* Undocumented. Used by PLScenes driver */
                    if (pbUseRedirectURLIfNoQueryStringParams)
                        *pbUseRedirectURLIfNoQueryStringParams =
                            CPLTestBool(pszValue);
                }
                else if (EQUAL(pszKey, "list_dir"))
                {
                    if (pbListDir)
                        *pbListDir = CPLTestBool(pszValue);
                }
                else if (EQUAL(pszKey, "empty_dir"))
                {
                    if (pbEmptyDir)
                        *pbEmptyDir = CPLTestBool(pszValue);
                }
                else if (EQUAL(pszKey, "useragent") ||
                         EQUAL(pszKey, "referer") || EQUAL(pszKey, "cookie") ||
                         EQUAL(pszKey, "header_file") ||
                         EQUAL(pszKey, "unsafessl") ||
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
                         EQUAL(pszKey, "timeout") ||
                         EQUAL(pszKey, "connecttimeout") ||
#endif
                         EQUAL(pszKey, "low_speed_time") ||
                         EQUAL(pszKey, "low_speed_limit") ||
                         EQUAL(pszKey, "proxy") || EQUAL(pszKey, "proxyauth") ||
                         EQUAL(pszKey, "proxyuserpwd"))
                {
                    // Above names are the ones supported by
                    // CPLHTTPSetOptions()
                    if (paosHTTPOptions)
                    {
                        paosHTTPOptions->SetNameValue(pszKey, pszValue);
                    }
                }
                else if (EQUAL(pszKey, "url"))
                {
                    osURL = pszValue;
                }
                else if (EQUAL(pszKey, "pc_url_signing"))
                {
                    if (pbPlanetaryComputerURLSigning)
                        *pbPlanetaryComputerURLSigning = CPLTestBool(pszValue);
                }
                else if (EQUAL(pszKey, "pc_collection"))
                {
                    if (ppszPlanetaryComputerCollection)
                    {
                        CPLFree(*ppszPlanetaryComputerCollection);
                        *ppszPlanetaryComputerCollection = CPLStrdup(pszValue);
                    }
                }
                else if (STARTS_WITH(pszKey, "header."))
                {
                    osHeaders += (pszKey + strlen("header."));
                    osHeaders += ':';
                    osHeaders += pszValue;
                    osHeaders += "\r\n";
                }
                else
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Unsupported option: %s", pszKey);
                }
            }
            CPLFree(pszKey);
        }

        if (paosHTTPOptions && !osHeaders.empty())
            paosHTTPOptions->SetNameValue("HEADERS", osHeaders.c_str());

        CSLDestroy(papszTokens);
        if (osURL.empty())
        {
            CPLError(CE_Failure, CPLE_IllegalArg, "Missing url parameter");
            return pszFilename;
        }

        return osURL;
    }

    return pszFilename;
}

namespace cpl
{

/************************************************************************/
/*                           VSICurlHandle()                            */
/************************************************************************/

VSICurlHandle::VSICurlHandle(VSICurlFilesystemHandlerBase *poFSIn,
                             const char *pszFilename, const char *pszURLIn)
    : poFS(poFSIn), m_osFilename(pszFilename),
      m_aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszFilename)),
      m_oRetryParameters(m_aosHTTPOptions),
      m_bUseHead(
          CPLTestBool(CPLGetConfigOption("CPL_VSIL_CURL_USE_HEAD", "YES")))
{
    if (pszURLIn)
    {
        m_pszURL = CPLStrdup(pszURLIn);
    }
    else
    {
        char *pszPCCollection = nullptr;
        m_pszURL =
            CPLStrdup(VSICurlGetURLFromFilename(
                          pszFilename, &m_oRetryParameters, &m_bUseHead,
                          &m_bUseRedirectURLIfNoQueryStringParams, nullptr,
                          nullptr, &m_aosHTTPOptions,
                          &m_bPlanetaryComputerURLSigning, &pszPCCollection)
                          .c_str());
        if (pszPCCollection)
            m_osPlanetaryComputerCollection = pszPCCollection;
        CPLFree(pszPCCollection);
    }

    m_bCached = poFSIn->AllowCachedDataFor(pszFilename);
    poFS->GetCachedFileProp(m_pszURL, oFileProp);
}

/************************************************************************/
/*                          ~VSICurlHandle()                            */
/************************************************************************/

VSICurlHandle::~VSICurlHandle()
{
    if (m_oThreadAdviseRead.joinable())
    {
        m_oThreadAdviseRead.join();
    }
    if (m_hCurlMultiHandleForAdviseRead)
    {
        curl_multi_cleanup(m_hCurlMultiHandleForAdviseRead);
    }

    if (!m_bCached)
    {
        poFS->InvalidateCachedData(m_pszURL);
        poFS->InvalidateDirContent(CPLGetDirnameSafe(m_osFilename.c_str()));
    }
    CPLFree(m_pszURL);
}

/************************************************************************/
/*                            SetURL()                                  */
/************************************************************************/

void VSICurlHandle::SetURL(const char *pszURLIn)
{
    CPLFree(m_pszURL);
    m_pszURL = CPLStrdup(pszURLIn);
}

/************************************************************************/
/*                          InstallReadCbk()                            */
/************************************************************************/

int VSICurlHandle::InstallReadCbk(VSICurlReadCbkFunc pfnReadCbkIn,
                                  void *pfnUserDataIn,
                                  int bStopOnInterruptUntilUninstallIn)
{
    if (pfnReadCbk != nullptr)
        return FALSE;

    pfnReadCbk = pfnReadCbkIn;
    pReadCbkUserData = pfnUserDataIn;
    bStopOnInterruptUntilUninstall =
        CPL_TO_BOOL(bStopOnInterruptUntilUninstallIn);
    bInterrupted = false;
    return TRUE;
}

/************************************************************************/
/*                         UninstallReadCbk()                           */
/************************************************************************/

int VSICurlHandle::UninstallReadCbk()
{
    if (pfnReadCbk == nullptr)
        return FALSE;

    pfnReadCbk = nullptr;
    pReadCbkUserData = nullptr;
    bStopOnInterruptUntilUninstall = false;
    bInterrupted = false;
    return TRUE;
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int VSICurlHandle::Seek(vsi_l_offset nOffset, int nWhence)
{
    if (nWhence == SEEK_SET)
    {
        curOffset = nOffset;
    }
    else if (nWhence == SEEK_CUR)
    {
        curOffset = curOffset + nOffset;
    }
    else
    {
        curOffset = GetFileSize(false) + nOffset;
    }
    bEOF = false;
    return 0;
}

}  // namespace cpl

/************************************************************************/
/*                 VSICurlGetTimeStampFromRFC822DateTime()              */
/************************************************************************/

static GIntBig VSICurlGetTimeStampFromRFC822DateTime(const char *pszDT)
{
    // Sun, 03 Apr 2016 12:07:27 GMT
    if (strlen(pszDT) >= 5 && pszDT[3] == ',' && pszDT[4] == ' ')
        pszDT += 5;
    int nDay = 0;
    int nYear = 0;
    int nHour = 0;
    int nMinute = 0;
    int nSecond = 0;
    char szMonth[4] = {};
    szMonth[3] = 0;
    if (sscanf(pszDT, "%02d %03s %04d %02d:%02d:%02d GMT", &nDay, szMonth,
               &nYear, &nHour, &nMinute, &nSecond) == 6)
    {
        static const char *const aszMonthStr[] = {"Jan", "Feb", "Mar", "Apr",
                                                  "May", "Jun", "Jul", "Aug",
                                                  "Sep", "Oct", "Nov", "Dec"};

        int nMonthIdx0 = -1;
        for (int i = 0; i < 12; i++)
        {
            if (EQUAL(szMonth, aszMonthStr[i]))
            {
                nMonthIdx0 = i;
                break;
            }
        }
        if (nMonthIdx0 >= 0)
        {
            struct tm brokendowntime;
            brokendowntime.tm_year = nYear - 1900;
            brokendowntime.tm_mon = nMonthIdx0;
            brokendowntime.tm_mday = nDay;
            brokendowntime.tm_hour = nHour;
            brokendowntime.tm_min = nMinute;
            brokendowntime.tm_sec = nSecond;
            return CPLYMDHMSToUnixTime(&brokendowntime);
        }
    }
    return 0;
}

/************************************************************************/
/*                    VSICURLInitWriteFuncStruct()                      */
/************************************************************************/

void VSICURLInitWriteFuncStruct(cpl::WriteFuncStruct *psStruct, VSILFILE *fp,
                                VSICurlReadCbkFunc pfnReadCbk,
                                void *pReadCbkUserData)
{
    psStruct->pBuffer = nullptr;
    psStruct->nSize = 0;
    psStruct->bIsHTTP = false;
    psStruct->bMultiRange = false;
    psStruct->nStartOffset = 0;
    psStruct->nEndOffset = 0;
    psStruct->nHTTPCode = 0;
    psStruct->nFirstHTTPCode = 0;
    psStruct->nContentLength = 0;
    psStruct->bFoundContentRange = false;
    psStruct->bError = false;
    psStruct->bDetectRangeDownloadingError = true;
    psStruct->nTimestampDate = 0;

    psStruct->fp = fp;
    psStruct->pfnReadCbk = pfnReadCbk;
    psStruct->pReadCbkUserData = pReadCbkUserData;
    psStruct->bInterrupted = false;
}

/************************************************************************/
/*                       VSICurlHandleWriteFunc()                       */
/************************************************************************/

size_t VSICurlHandleWriteFunc(void *buffer, size_t count, size_t nmemb,
                              void *req)
{
    cpl::WriteFuncStruct *psStruct = static_cast<cpl::WriteFuncStruct *>(req);
    const size_t nSize = count * nmemb;

    if (psStruct->bInterrupted)
    {
        return 0;
    }

    char *pNewBuffer = static_cast<char *>(
        VSIRealloc(psStruct->pBuffer, psStruct->nSize + nSize + 1));
    if (pNewBuffer)
    {
        psStruct->pBuffer = pNewBuffer;
        memcpy(psStruct->pBuffer + psStruct->nSize, buffer, nSize);
        psStruct->pBuffer[psStruct->nSize + nSize] = '\0';
        if (psStruct->bIsHTTP)
        {
            char *pszLine = psStruct->pBuffer + psStruct->nSize;
            if (STARTS_WITH_CI(pszLine, "HTTP/"))
            {
                char *pszSpace = strchr(pszLine, ' ');
                if (pszSpace)
                {
                    const int nHTTPCode = atoi(pszSpace + 1);
                    if (psStruct->nFirstHTTPCode == 0)
                        psStruct->nFirstHTTPCode = nHTTPCode;
                    psStruct->nHTTPCode = nHTTPCode;
                }
            }
            else if (STARTS_WITH_CI(pszLine, "Content-Length: "))
            {
                psStruct->nContentLength = CPLScanUIntBig(
                    pszLine + 16, static_cast<int>(strlen(pszLine + 16)));
            }
            else if (STARTS_WITH_CI(pszLine, "Content-Range: "))
            {
                psStruct->bFoundContentRange = true;
            }
            else if (STARTS_WITH_CI(pszLine, "Date: "))
            {
                CPLString osDate = pszLine + strlen("Date: ");
                size_t nSizeLine = osDate.size();
                while (nSizeLine && (osDate[nSizeLine - 1] == '\r' ||
                                     osDate[nSizeLine - 1] == '\n'))
                {
                    osDate.resize(nSizeLine - 1);
                    nSizeLine--;
                }
                osDate.Trim();

                GIntBig nTimestampDate =
                    VSICurlGetTimeStampFromRFC822DateTime(osDate.c_str());
#if DEBUG_VERBOSE
                CPLDebug("VSICURL", "Timestamp = " CPL_FRMT_GIB,
                         nTimestampDate);
#endif
                psStruct->nTimestampDate = nTimestampDate;
            }
            /*if( nSize > 2 && pszLine[nSize - 2] == '\r' &&
                  pszLine[nSize - 1] == '\n' )
            {
                pszLine[nSize - 2] = 0;
                CPLDebug("VSICURL", "%s", pszLine);
                pszLine[nSize - 2] = '\r';
            }*/

            if (pszLine[0] == '\r' && pszLine[1] == '\n')
            {
                // Detect servers that don't support range downloading.
                if (psStruct->nHTTPCode == 200 &&
                    psStruct->bDetectRangeDownloadingError &&
                    !psStruct->bMultiRange && !psStruct->bFoundContentRange &&
                    (psStruct->nStartOffset != 0 ||
                     psStruct->nContentLength >
                         10 * (psStruct->nEndOffset - psStruct->nStartOffset +
                               1)))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Range downloading not supported by this "
                             "server!");
                    psStruct->bError = true;
                    return 0;
                }
            }
        }
        else
        {
            if (psStruct->pfnReadCbk)
            {
                if (!psStruct->pfnReadCbk(psStruct->fp, buffer, nSize,
                                          psStruct->pReadCbkUserData))
                {
                    psStruct->bInterrupted = true;
                    return 0;
                }
            }
        }
        psStruct->nSize += nSize;
        return nmemb;
    }
    else
    {
        return 0;
    }
}

/************************************************************************/
/*                    VSICurlIsS3LikeSignedURL()                        */
/************************************************************************/

static bool VSICurlIsS3LikeSignedURL(const char *pszURL)
{
    return ((strstr(pszURL, ".s3.amazonaws.com/") != nullptr ||
             strstr(pszURL, ".s3.amazonaws.com:") != nullptr ||
             strstr(pszURL, ".storage.googleapis.com/") != nullptr ||
             strstr(pszURL, ".storage.googleapis.com:") != nullptr ||
             strstr(pszURL, ".cloudfront.net/") != nullptr ||
             strstr(pszURL, ".cloudfront.net:") != nullptr) &&
            (strstr(pszURL, "&Signature=") != nullptr ||
             strstr(pszURL, "?Signature=") != nullptr)) ||
           strstr(pszURL, "&X-Amz-Signature=") != nullptr ||
           strstr(pszURL, "?X-Amz-Signature=") != nullptr;
}

/************************************************************************/
/*                  VSICurlGetExpiresFromS3LikeSignedURL()              */
/************************************************************************/

static GIntBig VSICurlGetExpiresFromS3LikeSignedURL(const char *pszURL)
{
    const auto GetParamValue = [pszURL](const char *pszKey) -> const char *
    {
        for (const char *pszPrefix : {"&", "?"})
        {
            std::string osNeedle(pszPrefix);
            osNeedle += pszKey;
            osNeedle += '=';
            const char *pszStr = strstr(pszURL, osNeedle.c_str());
            if (pszStr)
                return pszStr + osNeedle.size();
        }
        return nullptr;
    };

    {
        // Expires= is a Unix timestamp
        const char *pszExpires = GetParamValue("Expires");
        if (pszExpires != nullptr)
            return CPLAtoGIntBig(pszExpires);
    }

    // X-Amz-Expires= is a delay, to be combined with X-Amz-Date=
    const char *pszAmzExpires = GetParamValue("X-Amz-Expires");
    if (pszAmzExpires == nullptr)
        return 0;
    const int nDelay = atoi(pszAmzExpires);

    const char *pszAmzDate = GetParamValue("X-Amz-Date");
    if (pszAmzDate == nullptr)
        return 0;
    // pszAmzDate should be YYYYMMDDTHHMMSSZ
    if (strlen(pszAmzDate) < strlen("YYYYMMDDTHHMMSSZ"))
        return 0;
    if (pszAmzDate[strlen("YYYYMMDDTHHMMSSZ") - 1] != 'Z')
        return 0;
    struct tm brokendowntime;
    brokendowntime.tm_year =
        atoi(std::string(pszAmzDate).substr(0, 4).c_str()) - 1900;
    brokendowntime.tm_mon =
        atoi(std::string(pszAmzDate).substr(4, 2).c_str()) - 1;
    brokendowntime.tm_mday = atoi(std::string(pszAmzDate).substr(6, 2).c_str());
    brokendowntime.tm_hour = atoi(std::string(pszAmzDate).substr(9, 2).c_str());
    brokendowntime.tm_min = atoi(std::string(pszAmzDate).substr(11, 2).c_str());
    brokendowntime.tm_sec = atoi(std::string(pszAmzDate).substr(13, 2).c_str());
    return CPLYMDHMSToUnixTime(&brokendowntime) + nDelay;
}

/************************************************************************/
/*                       VSICURLMultiPerform()                          */
/************************************************************************/

void VSICURLMultiPerform(CURLM *hCurlMultiHandle, CURL *hEasyHandle,
                         std::atomic<bool> *pbInterrupt)
{
    int repeats = 0;

    if (hEasyHandle)
        curl_multi_add_handle(hCurlMultiHandle, hEasyHandle);

    void *old_handler = CPLHTTPIgnoreSigPipe();
    while (true)
    {
        int still_running;
        while (curl_multi_perform(hCurlMultiHandle, &still_running) ==
               CURLM_CALL_MULTI_PERFORM)
        {
            // loop
        }
        if (!still_running)
        {
            break;
        }

#ifdef undef
        CURLMsg *msg;
        do
        {
            int msgq = 0;
            msg = curl_multi_info_read(hCurlMultiHandle, &msgq);
            if (msg && (msg->msg == CURLMSG_DONE))
            {
                CURL *e = msg->easy_handle;
            }
        } while (msg);
#endif

        CPLMultiPerformWait(hCurlMultiHandle, repeats);

        if (pbInterrupt && *pbInterrupt)
            break;
    }
    CPLHTTPRestoreSigPipeHandler(old_handler);

    if (hEasyHandle)
        curl_multi_remove_handle(hCurlMultiHandle, hEasyHandle);
}

/************************************************************************/
/*                       VSICurlDummyWriteFunc()                        */
/************************************************************************/

static size_t VSICurlDummyWriteFunc(void *, size_t, size_t, void *)
{
    return 0;
}

/************************************************************************/
/*                  VSICURLResetHeaderAndWriterFunctions()              */
/************************************************************************/

void VSICURLResetHeaderAndWriterFunctions(CURL *hCurlHandle)
{
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                               VSICurlDummyWriteFunc);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlDummyWriteFunc);
}

/************************************************************************/
/*                        Iso8601ToUnixTime()                           */
/************************************************************************/

static bool Iso8601ToUnixTime(const char *pszDT, GIntBig *pnUnixTime)
{
    int nYear;
    int nMonth;
    int nDay;
    int nHour;
    int nMinute;
    int nSecond;
    if (sscanf(pszDT, "%04d-%02d-%02dT%02d:%02d:%02d", &nYear, &nMonth, &nDay,
               &nHour, &nMinute, &nSecond) == 6)
    {
        struct tm brokendowntime;
        brokendowntime.tm_year = nYear - 1900;
        brokendowntime.tm_mon = nMonth - 1;
        brokendowntime.tm_mday = nDay;
        brokendowntime.tm_hour = nHour;
        brokendowntime.tm_min = nMinute;
        brokendowntime.tm_sec = nSecond;
        *pnUnixTime = CPLYMDHMSToUnixTime(&brokendowntime);
        return true;
    }
    return false;
}

namespace cpl
{

/************************************************************************/
/*                   ManagePlanetaryComputerSigning()                   */
/************************************************************************/

void VSICurlHandle::ManagePlanetaryComputerSigning() const
{
    // Take global lock
    static std::mutex goMutex;
    std::lock_guard<std::mutex> oLock(goMutex);

    struct PCSigningInfo
    {
        std::string osQueryString{};
        GIntBig nExpireTimestamp = 0;
    };

    PCSigningInfo sSigningInfo;
    constexpr int knExpirationDelayMargin = 60;

    if (!m_osPlanetaryComputerCollection.empty())
    {
        // key is the name of a collection
        static lru11::Cache<std::string, PCSigningInfo> goCacheCollection{1024};

        if (goCacheCollection.tryGet(m_osPlanetaryComputerCollection,
                                     sSigningInfo) &&
            time(nullptr) + knExpirationDelayMargin <=
                sSigningInfo.nExpireTimestamp)
        {
            m_osQueryString = sSigningInfo.osQueryString;
        }
        else
        {
            const auto psResult =
                CPLHTTPFetch((std::string(CPLGetConfigOption(
                                  "VSICURL_PC_SAS_TOKEN_URL",
                                  "https://planetarycomputer.microsoft.com/api/"
                                  "sas/v1/token/")) +
                              m_osPlanetaryComputerCollection)
                                 .c_str(),
                             nullptr);
            if (psResult)
            {
                const auto aosKeyVals = CPLParseKeyValueJson(
                    reinterpret_cast<const char *>(psResult->pabyData));
                const char *pszToken = aosKeyVals.FetchNameValue("token");
                if (pszToken)
                {
                    m_osQueryString = '?';
                    m_osQueryString += pszToken;

                    sSigningInfo.osQueryString = m_osQueryString;
                    sSigningInfo.nExpireTimestamp = 0;
                    const char *pszExpiry =
                        aosKeyVals.FetchNameValue("msft:expiry");
                    if (pszExpiry)
                    {
                        Iso8601ToUnixTime(pszExpiry,
                                          &sSigningInfo.nExpireTimestamp);
                    }
                    goCacheCollection.insert(m_osPlanetaryComputerCollection,
                                             sSigningInfo);

                    CPLDebug("VSICURL", "Got token from Planetary Computer: %s",
                             m_osQueryString.c_str());
                }
                CPLHTTPDestroyResult(psResult);
            }
        }
    }
    else
    {
        // key is a URL
        static lru11::Cache<std::string, PCSigningInfo> goCacheURL{1024};

        if (goCacheURL.tryGet(m_pszURL, sSigningInfo) &&
            time(nullptr) + knExpirationDelayMargin <=
                sSigningInfo.nExpireTimestamp)
        {
            m_osQueryString = sSigningInfo.osQueryString;
        }
        else
        {
            const auto psResult =
                CPLHTTPFetch((std::string(CPLGetConfigOption(
                                  "VSICURL_PC_SAS_SIGN_HREF_URL",
                                  "https://planetarycomputer.microsoft.com/api/"
                                  "sas/v1/sign?href=")) +
                              m_pszURL)
                                 .c_str(),
                             nullptr);
            if (psResult)
            {
                const auto aosKeyVals = CPLParseKeyValueJson(
                    reinterpret_cast<const char *>(psResult->pabyData));
                const char *pszHref = aosKeyVals.FetchNameValue("href");
                if (pszHref && STARTS_WITH(pszHref, m_pszURL))
                {
                    m_osQueryString = pszHref + strlen(m_pszURL);

                    sSigningInfo.osQueryString = m_osQueryString;
                    sSigningInfo.nExpireTimestamp = 0;
                    const char *pszExpiry =
                        aosKeyVals.FetchNameValue("msft:expiry");
                    if (pszExpiry)
                    {
                        Iso8601ToUnixTime(pszExpiry,
                                          &sSigningInfo.nExpireTimestamp);
                    }
                    goCacheURL.insert(m_pszURL, sSigningInfo);

                    CPLDebug("VSICURL",
                             "Got signature from Planetary Computer: %s",
                             m_osQueryString.c_str());
                }
                CPLHTTPDestroyResult(psResult);
            }
        }
    }
}

/************************************************************************/
/*                        UpdateQueryString()                           */
/************************************************************************/

void VSICurlHandle::UpdateQueryString() const
{
    if (m_bPlanetaryComputerURLSigning)
    {
        ManagePlanetaryComputerSigning();
    }
    else
    {
        const char *pszQueryString = VSIGetPathSpecificOption(
            m_osFilename.c_str(), "VSICURL_QUERY_STRING", nullptr);
        if (pszQueryString)
        {
            if (m_osFilename.back() == '?')
            {
                if (pszQueryString[0] == '?')
                    m_osQueryString = pszQueryString + 1;
                else
                    m_osQueryString = pszQueryString;
            }
            else
            {
                if (pszQueryString[0] == '?')
                    m_osQueryString = pszQueryString;
                else
                {
                    m_osQueryString = "?";
                    m_osQueryString.append(pszQueryString);
                }
            }
        }
    }
}

/************************************************************************/
/*                     GetFileSizeOrHeaders()                           */
/************************************************************************/

vsi_l_offset VSICurlHandle::GetFileSizeOrHeaders(bool bSetError,
                                                 bool bGetHeaders)
{
    if (oFileProp.bHasComputedFileSize && !bGetHeaders)
        return oFileProp.fileSize;

    NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("GetFileSize");

    oFileProp.bHasComputedFileSize = true;

    CURLM *hCurlMultiHandle = poFS->GetCurlMultiHandleFor(m_pszURL);

    UpdateQueryString();

    std::string osURL(m_pszURL + m_osQueryString);
    bool bRetryWithGet = false;
    bool bS3LikeRedirect = false;
    CPLHTTPRetryContext oRetryContext(m_oRetryParameters);

retry:
    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), m_aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncHeaderData;
    VSICURLInitWriteFuncStruct(&sWriteFuncHeaderData, nullptr, nullptr,
                               nullptr);
    sWriteFuncHeaderData.bDetectRangeDownloadingError = false;
    sWriteFuncHeaderData.bIsHTTP = STARTS_WITH(osURL.c_str(), "http");

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);

    std::string osVerb;
    std::string osRange;  // leave in this scope !
    int nRoundedBufSize = 0;
    const int knDOWNLOAD_CHUNK_SIZE = VSICURLGetDownloadChunkSize();
    if (UseLimitRangeGetInsteadOfHead())
    {
        osVerb = "GET";
        const int nBufSize = std::max(
            1024, std::min(10 * 1024 * 1024,
                           atoi(CPLGetConfigOption(
                               "GDAL_INGESTED_BYTES_AT_OPEN", "1024"))));
        nRoundedBufSize = cpl::div_round_up(nBufSize, knDOWNLOAD_CHUNK_SIZE) *
                          knDOWNLOAD_CHUNK_SIZE;

        // so it gets included in Azure signature
        osRange = CPLSPrintf("Range: bytes=0-%d", nRoundedBufSize - 1);
        headers = curl_slist_append(headers, osRange.c_str());
    }
    // HACK for mbtiles driver: http://a.tiles.mapbox.com/v3/ doesn't accept
    // HEAD, as it is a redirect to AWS S3 signed URL, but those are only valid
    // for a given type of HTTP request, and thus GET. This is valid for any
    // signed URL for AWS S3.
    else if (bRetryWithGet ||
             strstr(osURL.c_str(), ".tiles.mapbox.com/") != nullptr ||
             VSICurlIsS3LikeSignedURL(osURL.c_str()) || !m_bUseHead)
    {
        sWriteFuncData.bInterrupted = true;
        osVerb = "GET";
    }
    else
    {
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_NOBODY, 1);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPGET, 0);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADER, 1);
        osVerb = "HEAD";
    }

    if (!AllowAutomaticRedirection())
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FOLLOWLOCATION, 0);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                               &sWriteFuncHeaderData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                               VSICurlHandleWriteFunc);

    // Bug with older curl versions (<=7.16.4) and FTP.
    // See http://curl.haxx.se/mail/lib-2007-08/0312.html
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
    szCurlErrBuf[0] = '\0';
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    headers = GetCurlHeaders(osVerb, headers);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FILETIME, 1);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle, &m_bInterrupt);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    oFileProp.eExists = EXIST_UNKNOWN;

    long mtime = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_FILETIME, &mtime);

    if (osVerb == "GET")
        NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);
    else
        NetworkStatisticsLogger::LogHEAD();

    if (STARTS_WITH(osURL.c_str(), "ftp"))
    {
        if (sWriteFuncData.pBuffer != nullptr)
        {
            const char *pszContentLength =
                strstr(const_cast<const char *>(sWriteFuncData.pBuffer),
                       "Content-Length: ");
            if (pszContentLength)
            {
                pszContentLength += strlen("Content-Length: ");
                oFileProp.eExists = EXIST_YES;
                oFileProp.fileSize =
                    CPLScanUIntBig(pszContentLength,
                                   static_cast<int>(strlen(pszContentLength)));
                if (ENABLE_DEBUG)
                    CPLDebug(poFS->GetDebugKey(),
                             "GetFileSize(%s)=" CPL_FRMT_GUIB, osURL.c_str(),
                             oFileProp.fileSize);
            }
        }
    }

    double dfSize = 0;
    if (oFileProp.eExists != EXIST_YES)
    {
        long response_code = 0;
        curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

        bool bAlreadyLogged = false;
        if (response_code >= 400 && szCurlErrBuf[0] == '\0')
        {
            const bool bLogResponse =
                CPLTestBool(CPLGetConfigOption("CPL_CURL_VERBOSE", "NO"));
            if (bLogResponse && sWriteFuncData.pBuffer)
            {
                const char *pszErrorMsg =
                    static_cast<const char *>(sWriteFuncData.pBuffer);
                bAlreadyLogged = true;
                CPLDebug(
                    poFS->GetDebugKey(),
                    "GetFileSize(%s): response_code=%d, server error msg=%s",
                    osURL.c_str(), static_cast<int>(response_code),
                    pszErrorMsg[0] ? pszErrorMsg : "(no message provided)");
            }
        }
        else if (szCurlErrBuf[0] != '\0')
        {
            bAlreadyLogged = true;
            CPLDebug(poFS->GetDebugKey(),
                     "GetFileSize(%s): response_code=%d, curl error msg=%s",
                     osURL.c_str(), static_cast<int>(response_code),
                     szCurlErrBuf);
        }

        std::string osEffectiveURL;
        {
            char *pszEffectiveURL = nullptr;
            curl_easy_getinfo(hCurlHandle, CURLINFO_EFFECTIVE_URL,
                              &pszEffectiveURL);
            if (pszEffectiveURL)
                osEffectiveURL = pszEffectiveURL;
        }

        if (!osEffectiveURL.empty() &&
            strstr(osEffectiveURL.c_str(), osURL.c_str()) == nullptr)
        {
            // Moved permanently ?
            if (sWriteFuncHeaderData.nFirstHTTPCode == 301 ||
                (m_bUseRedirectURLIfNoQueryStringParams &&
                 osEffectiveURL.find('?') == std::string::npos))
            {
                CPLDebug(poFS->GetDebugKey(),
                         "Using effective URL %s permanently",
                         osEffectiveURL.c_str());
                oFileProp.osRedirectURL = osEffectiveURL;
                poFS->SetCachedFileProp(m_pszURL, oFileProp);
            }
            else
            {
                CPLDebug(poFS->GetDebugKey(),
                         "Using effective URL %s temporarily",
                         osEffectiveURL.c_str());
            }

            // Is this is a redirect to a S3 URL?
            if (VSICurlIsS3LikeSignedURL(osEffectiveURL.c_str()) &&
                !VSICurlIsS3LikeSignedURL(osURL.c_str()))
            {
                // Note that this is a redirect as we won't notice after the
                // retry.
                bS3LikeRedirect = true;

                if (!bRetryWithGet && osVerb == "HEAD" && response_code == 403)
                {
                    CPLDebug(poFS->GetDebugKey(),
                             "Redirected to a AWS S3 signed URL. Retrying "
                             "with GET request instead of HEAD since the URL "
                             "might be valid only for GET");
                    bRetryWithGet = true;
                    osURL = std::move(osEffectiveURL);
                    CPLFree(sWriteFuncData.pBuffer);
                    CPLFree(sWriteFuncHeaderData.pBuffer);
                    curl_easy_cleanup(hCurlHandle);
                    goto retry;
                }
            }
        }

        if (bS3LikeRedirect && response_code >= 200 && response_code < 300 &&
            sWriteFuncHeaderData.nTimestampDate > 0 &&
            !osEffectiveURL.empty() &&
            CPLTestBool(
                CPLGetConfigOption("CPL_VSIL_CURL_USE_S3_REDIRECT", "TRUE")))
        {
            const GIntBig nExpireTimestamp =
                VSICurlGetExpiresFromS3LikeSignedURL(osEffectiveURL.c_str());
            if (nExpireTimestamp > sWriteFuncHeaderData.nTimestampDate + 10)
            {
                const int nValidity = static_cast<int>(
                    nExpireTimestamp - sWriteFuncHeaderData.nTimestampDate);
                CPLDebug(poFS->GetDebugKey(),
                         "Will use redirect URL for the next %d seconds",
                         nValidity);
                // As our local clock might not be in sync with server clock,
                // figure out the expiration timestamp in local time
                oFileProp.bS3LikeRedirect = true;
                oFileProp.nExpireTimestampLocal = time(nullptr) + nValidity;
                oFileProp.osRedirectURL = osEffectiveURL;
                poFS->SetCachedFileProp(m_pszURL, oFileProp);
            }
        }

        curl_off_t nSizeTmp = 0;
        const CURLcode code = curl_easy_getinfo(
            hCurlHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &nSizeTmp);
        CPL_IGNORE_RET_VAL(dfSize);
        dfSize = static_cast<double>(nSizeTmp);
        if (code == 0)
        {
            oFileProp.eExists = EXIST_YES;
            if (dfSize < 0)
            {
                if (osVerb == "HEAD" && !bRetryWithGet && response_code == 200)
                {
                    CPLDebug(
                        poFS->GetDebugKey(),
                        "HEAD did not provide file size. Retrying with GET");
                    bRetryWithGet = true;
                    CPLFree(sWriteFuncData.pBuffer);
                    CPLFree(sWriteFuncHeaderData.pBuffer);
                    curl_easy_cleanup(hCurlHandle);
                    goto retry;
                }
                oFileProp.fileSize = 0;
            }
            else
                oFileProp.fileSize = static_cast<GUIntBig>(dfSize);
        }

        if (sWriteFuncHeaderData.pBuffer != nullptr &&
            (response_code == 200 || response_code == 206))
        {
            {
                char **papszHeaders =
                    CSLTokenizeString2(sWriteFuncHeaderData.pBuffer, "\r\n", 0);
                for (int i = 0; papszHeaders[i]; ++i)
                {
                    char *pszKey = nullptr;
                    const char *pszValue =
                        CPLParseNameValue(papszHeaders[i], &pszKey);
                    if (pszKey && pszValue)
                    {
                        if (bGetHeaders)
                        {
                            m_aosHeaders.SetNameValue(pszKey, pszValue);
                        }
                        if (EQUAL(pszKey, "Cache-Control") &&
                            EQUAL(pszValue, "no-cache") &&
                            CPLTestBool(CPLGetConfigOption(
                                "CPL_VSIL_CURL_HONOR_CACHE_CONTROL", "YES")))
                        {
                            m_bCached = false;
                        }

                        else if (EQUAL(pszKey, "ETag"))
                        {
                            std::string osValue(pszValue);
                            if (osValue.size() >= 2 && osValue.front() == '"' &&
                                osValue.back() == '"')
                                osValue = osValue.substr(1, osValue.size() - 2);
                            oFileProp.ETag = std::move(osValue);
                        }

                        // Azure Data Lake Storage
                        else if (EQUAL(pszKey, "x-ms-resource-type"))
                        {
                            if (EQUAL(pszValue, "file"))
                            {
                                oFileProp.nMode |= S_IFREG;
                            }
                            else if (EQUAL(pszValue, "directory"))
                            {
                                oFileProp.bIsDirectory = true;
                                oFileProp.nMode |= S_IFDIR;
                            }
                        }
                        else if (EQUAL(pszKey, "x-ms-permissions"))
                        {
                            oFileProp.nMode |=
                                VSICurlParseUnixPermissions(pszValue);
                        }

                        // https://overturemapswestus2.blob.core.windows.net/release/2024-11-13.0/theme%3Ddivisions/type%3Ddivision_area
                        // returns a x-ms-meta-hdi_isfolder: true header
                        else if (EQUAL(pszKey, "x-ms-meta-hdi_isfolder") &&
                                 EQUAL(pszValue, "true"))
                        {
                            oFileProp.bIsAzureFolder = true;
                            oFileProp.bIsDirectory = true;
                            oFileProp.nMode |= S_IFDIR;
                        }
                    }
                    CPLFree(pszKey);
                }
                CSLDestroy(papszHeaders);
            }
        }

        if (UseLimitRangeGetInsteadOfHead() && response_code == 206)
        {
            oFileProp.eExists = EXIST_NO;
            oFileProp.fileSize = 0;
            if (sWriteFuncHeaderData.pBuffer != nullptr)
            {
                const char *pszContentRange = strstr(
                    sWriteFuncHeaderData.pBuffer, "Content-Range: bytes ");
                if (pszContentRange == nullptr)
                    pszContentRange = strstr(sWriteFuncHeaderData.pBuffer,
                                             "content-range: bytes ");
                if (pszContentRange)
                    pszContentRange = strchr(pszContentRange, '/');
                if (pszContentRange)
                {
                    oFileProp.eExists = EXIST_YES;
                    oFileProp.fileSize = static_cast<GUIntBig>(
                        CPLAtoGIntBig(pszContentRange + 1));
                }

                // Add first bytes to cache
                if (sWriteFuncData.pBuffer != nullptr)
                {
                    size_t nOffset = 0;
                    while (nOffset < sWriteFuncData.nSize)
                    {
                        const size_t nToCache =
                            std::min<size_t>(sWriteFuncData.nSize - nOffset,
                                             knDOWNLOAD_CHUNK_SIZE);
                        poFS->AddRegion(m_pszURL, nOffset, nToCache,
                                        sWriteFuncData.pBuffer + nOffset);
                        nOffset += nToCache;
                    }
                }
            }
        }
        else if (IsDirectoryFromExists(osVerb.c_str(),
                                       static_cast<int>(response_code)))
        {
            oFileProp.eExists = EXIST_YES;
            oFileProp.fileSize = 0;
            oFileProp.bIsDirectory = true;
        }
        // 405 = Method not allowed
        else if (response_code == 405 && !bRetryWithGet && osVerb == "HEAD")
        {
            CPLDebug(poFS->GetDebugKey(),
                     "HEAD not allowed. Retrying with GET");
            bRetryWithGet = true;
            CPLFree(sWriteFuncData.pBuffer);
            CPLFree(sWriteFuncHeaderData.pBuffer);
            curl_easy_cleanup(hCurlHandle);
            goto retry;
        }
        else if (response_code == 416)
        {
            oFileProp.eExists = EXIST_YES;
            oFileProp.fileSize = 0;
        }
        else if (response_code != 200)
        {
            // Look if we should attempt a retry
            if (oRetryContext.CanRetry(static_cast<int>(response_code),
                                       sWriteFuncHeaderData.pBuffer,
                                       szCurlErrBuf))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "HTTP error code: %d - %s. "
                         "Retrying again in %.1f secs",
                         static_cast<int>(response_code), m_pszURL,
                         oRetryContext.GetCurrentDelay());
                CPLSleep(oRetryContext.GetCurrentDelay());
                CPLFree(sWriteFuncData.pBuffer);
                CPLFree(sWriteFuncHeaderData.pBuffer);
                curl_easy_cleanup(hCurlHandle);
                goto retry;
            }

            if (sWriteFuncData.pBuffer != nullptr)
            {
                if (UseLimitRangeGetInsteadOfHead() &&
                    CanRestartOnError(sWriteFuncData.pBuffer,
                                      sWriteFuncHeaderData.pBuffer, bSetError))
                {
                    oFileProp.bHasComputedFileSize = false;
                    CPLFree(sWriteFuncData.pBuffer);
                    CPLFree(sWriteFuncHeaderData.pBuffer);
                    curl_easy_cleanup(hCurlHandle);
                    return GetFileSizeOrHeaders(bSetError, bGetHeaders);
                }
                else
                {
                    CPL_IGNORE_RET_VAL(CanRestartOnError(
                        sWriteFuncData.pBuffer, sWriteFuncHeaderData.pBuffer,
                        bSetError));
                }
            }

            // If there was no VSI error thrown in the process,
            // fail by reporting the HTTP response code.
            if (bSetError && VSIGetLastErrorNo() == 0)
            {
                if (strlen(szCurlErrBuf) > 0)
                {
                    if (response_code == 0)
                    {
                        VSIError(VSIE_HttpError, "CURL error: %s",
                                 szCurlErrBuf);
                    }
                    else
                    {
                        VSIError(VSIE_HttpError, "HTTP response code: %d - %s",
                                 static_cast<int>(response_code), szCurlErrBuf);
                    }
                }
                else
                {
                    VSIError(VSIE_HttpError, "HTTP response code: %d",
                             static_cast<int>(response_code));
                }
            }
            else
            {
                if (response_code != 400 && response_code != 404)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "HTTP response code on %s: %d", osURL.c_str(),
                             static_cast<int>(response_code));
                }
                // else a CPLDebug() is emitted below
            }

            oFileProp.eExists = EXIST_NO;
            oFileProp.nHTTPCode = static_cast<int>(response_code);
            oFileProp.fileSize = 0;
        }
        else if (sWriteFuncData.pBuffer != nullptr)
        {
            ProcessGetFileSizeResult(
                reinterpret_cast<const char *>(sWriteFuncData.pBuffer));
        }

        // Try to guess if this is a directory. Generally if this is a
        // directory, curl will retry with an URL with slash added.
        if (!osEffectiveURL.empty() &&
            strncmp(osURL.c_str(), osEffectiveURL.c_str(), osURL.size()) == 0 &&
            osEffectiveURL[osURL.size()] == '/')
        {
            oFileProp.eExists = EXIST_YES;
            oFileProp.fileSize = 0;
            oFileProp.bIsDirectory = true;
        }
        else if (osURL.back() == '/')
        {
            oFileProp.bIsDirectory = true;
        }

        if (!bAlreadyLogged)
        {
            CPLDebug(poFS->GetDebugKey(),
                     "GetFileSize(%s)=" CPL_FRMT_GUIB "  response_code=%d",
                     osURL.c_str(), oFileProp.fileSize,
                     static_cast<int>(response_code));
        }
    }

    CPLFree(sWriteFuncData.pBuffer);
    CPLFree(sWriteFuncHeaderData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    oFileProp.bHasComputedFileSize = true;
    if (mtime > 0)
        oFileProp.mTime = mtime;
    poFS->SetCachedFileProp(m_pszURL, oFileProp);

    return oFileProp.fileSize;
}

/************************************************************************/
/*                                 Exists()                             */
/************************************************************************/

bool VSICurlHandle::Exists(bool bSetError)
{
    if (oFileProp.eExists == EXIST_UNKNOWN)
    {
        GetFileSize(bSetError);
    }
    else if (oFileProp.eExists == EXIST_NO)
    {
        // If there was no VSI error thrown in the process,
        // and we know the HTTP error code of the first request where the
        // file could not be retrieved, fail by reporting the HTTP code.
        if (bSetError && VSIGetLastErrorNo() == 0 && oFileProp.nHTTPCode)
        {
            VSIError(VSIE_HttpError, "HTTP response code: %d",
                     oFileProp.nHTTPCode);
        }
    }

    return oFileProp.eExists == EXIST_YES;
}

/************************************************************************/
/*                                  Tell()                              */
/************************************************************************/

vsi_l_offset VSICurlHandle::Tell()
{
    return curOffset;
}

/************************************************************************/
/*                       GetRedirectURLIfValid()                        */
/************************************************************************/

std::string
VSICurlHandle::GetRedirectURLIfValid(bool &bHasExpired,
                                     CPLStringList &aosHTTPOptions) const
{
    bHasExpired = false;
    poFS->GetCachedFileProp(m_pszURL, oFileProp);

    std::string osURL(m_pszURL + m_osQueryString);
    if (oFileProp.bS3LikeRedirect)
    {
        if (time(nullptr) + 1 < oFileProp.nExpireTimestampLocal)
        {
            CPLDebug(poFS->GetDebugKey(),
                     "Using redirect URL as it looks to be still valid "
                     "(%d seconds left)",
                     static_cast<int>(oFileProp.nExpireTimestampLocal -
                                      time(nullptr)));
            osURL = oFileProp.osRedirectURL;
        }
        else
        {
            CPLDebug(poFS->GetDebugKey(),
                     "Redirect URL has expired. Using original URL");
            oFileProp.bS3LikeRedirect = false;
            poFS->SetCachedFileProp(m_pszURL, oFileProp);
            bHasExpired = true;
        }
    }
    else if (!oFileProp.osRedirectURL.empty())
    {
        osURL = oFileProp.osRedirectURL;
        bHasExpired = false;
    }

    if (m_pszURL != osURL)
    {
        const char *pszAuthorizationHeaderAllowed = CPLGetConfigOption(
            "CPL_VSIL_CURL_AUTHORIZATION_HEADER_ALLOWED_IF_REDIRECT",
            "IF_SAME_HOST");
        if (EQUAL(pszAuthorizationHeaderAllowed, "IF_SAME_HOST"))
        {
            const auto ExtractServer = [](const std::string &s)
            {
                size_t afterHTTPPos = 0;
                if (STARTS_WITH(s.c_str(), "http://"))
                    afterHTTPPos = strlen("http://");
                else if (STARTS_WITH(s.c_str(), "https://"))
                    afterHTTPPos = strlen("https://");
                const auto posSlash = s.find('/', afterHTTPPos);
                if (posSlash != std::string::npos)
                    return s.substr(afterHTTPPos, posSlash - afterHTTPPos);
                else
                    return s.substr(afterHTTPPos);
            };

            if (ExtractServer(osURL) != ExtractServer(m_pszURL))
            {
                aosHTTPOptions.SetNameValue("AUTHORIZATION_HEADER_ALLOWED",
                                            "NO");
            }
        }
        else if (!CPLTestBool(pszAuthorizationHeaderAllowed))
        {
            aosHTTPOptions.SetNameValue("AUTHORIZATION_HEADER_ALLOWED", "NO");
        }
    }

    return osURL;
}

/************************************************************************/
/*                          CurrentDownload                             */
/************************************************************************/

namespace
{
struct CurrentDownload
{
    VSICurlFilesystemHandlerBase *m_poFS = nullptr;
    std::string m_osURL{};
    vsi_l_offset m_nStartOffset = 0;
    int m_nBlocks = 0;
    std::string m_osAlreadyDownloadedData{};
    bool m_bHasAlreadyDownloadedData = false;

    CurrentDownload(VSICurlFilesystemHandlerBase *poFS, const char *pszURL,
                    vsi_l_offset startOffset, int nBlocks)
        : m_poFS(poFS), m_osURL(pszURL), m_nStartOffset(startOffset),
          m_nBlocks(nBlocks)
    {
        auto res = m_poFS->NotifyStartDownloadRegion(m_osURL, m_nStartOffset,
                                                     m_nBlocks);
        m_bHasAlreadyDownloadedData = res.first;
        m_osAlreadyDownloadedData = std::move(res.second);
    }

    bool HasAlreadyDownloadedData() const
    {
        return m_bHasAlreadyDownloadedData;
    }

    const std::string &GetAlreadyDownloadedData() const
    {
        return m_osAlreadyDownloadedData;
    }

    void SetData(const std::string &osData)
    {
        CPLAssert(!m_bHasAlreadyDownloadedData);
        m_bHasAlreadyDownloadedData = true;
        m_poFS->NotifyStopDownloadRegion(m_osURL, m_nStartOffset, m_nBlocks,
                                         osData);
    }

    ~CurrentDownload()
    {
        if (!m_bHasAlreadyDownloadedData)
            m_poFS->NotifyStopDownloadRegion(m_osURL, m_nStartOffset, m_nBlocks,
                                             std::string());
    }

    CurrentDownload(const CurrentDownload &) = delete;
    CurrentDownload &operator=(const CurrentDownload &) = delete;
};
}  // namespace

/************************************************************************/
/*                      NotifyStartDownloadRegion()                     */
/************************************************************************/

/** Indicate intent at downloading a new region.
 *
 * If the region is already in download in another thread, then wait for its
 * completion.
 *
 * Returns:
 * - (false, empty string) if a new download is needed
 * - (true, region_content) if we have been waiting for a download of the same
 *   region to be completed and got its result. Note that region_content will be
 *   empty if the download of that region failed.
 */
std::pair<bool, std::string>
VSICurlFilesystemHandlerBase::NotifyStartDownloadRegion(
    const std::string &osURL, vsi_l_offset startOffset, int nBlocks)
{
    std::string osId(osURL);
    osId += '_';
    osId += std::to_string(startOffset);
    osId += '_';
    osId += std::to_string(nBlocks);

    m_oMutex.lock();
    auto oIter = m_oMapRegionInDownload.find(osId);
    if (oIter != m_oMapRegionInDownload.end())
    {
        auto &region = *(oIter->second);
        std::unique_lock<std::mutex> oRegionLock(region.oMutex);
        m_oMutex.unlock();
        region.nWaiters++;
        while (region.bDownloadInProgress)
        {
            region.oCond.wait(oRegionLock);
        }
        std::string osRet = region.osData;
        region.nWaiters--;
        region.oCond.notify_one();
        return std::pair<bool, std::string>(true, osRet);
    }
    else
    {
        auto poRegionInDownload = std::make_unique<RegionInDownload>();
        poRegionInDownload->bDownloadInProgress = true;
        m_oMapRegionInDownload[osId] = std::move(poRegionInDownload);
        m_oMutex.unlock();
        return std::pair<bool, std::string>(false, std::string());
    }
}

/************************************************************************/
/*                      NotifyStopDownloadRegion()                      */
/************************************************************************/

void VSICurlFilesystemHandlerBase::NotifyStopDownloadRegion(
    const std::string &osURL, vsi_l_offset startOffset, int nBlocks,
    const std::string &osData)
{
    std::string osId(osURL);
    osId += '_';
    osId += std::to_string(startOffset);
    osId += '_';
    osId += std::to_string(nBlocks);

    m_oMutex.lock();
    auto oIter = m_oMapRegionInDownload.find(osId);
    CPLAssert(oIter != m_oMapRegionInDownload.end());
    auto &region = *(oIter->second);
    {
        std::unique_lock<std::mutex> oRegionLock(region.oMutex);
        if (region.nWaiters)
        {
            region.osData = osData;
            region.bDownloadInProgress = false;
            region.oCond.notify_all();

            while (region.nWaiters)
            {
                region.oCond.wait(oRegionLock);
            }
        }
    }
    m_oMapRegionInDownload.erase(oIter);
    m_oMutex.unlock();
}

/************************************************************************/
/*                          DownloadRegion()                            */
/************************************************************************/

std::string VSICurlHandle::DownloadRegion(const vsi_l_offset startOffset,
                                          const int nBlocks)
{
    if (bInterrupted && bStopOnInterruptUntilUninstall)
        return std::string();

    if (oFileProp.eExists == EXIST_NO)
        return std::string();

    // Check if there is not a download of the same region in progress in
    // another thread, and if so wait for it to be completed
    CurrentDownload currentDownload(poFS, m_pszURL, startOffset, nBlocks);
    if (currentDownload.HasAlreadyDownloadedData())
    {
        return currentDownload.GetAlreadyDownloadedData();
    }

begin:
    CURLM *hCurlMultiHandle = poFS->GetCurlMultiHandleFor(m_pszURL);

    UpdateQueryString();

    bool bHasExpired = false;

    CPLStringList aosHTTPOptions(m_aosHTTPOptions);
    std::string osURL(GetRedirectURLIfValid(bHasExpired, aosHTTPOptions));
    bool bUsedRedirect = osURL != m_pszURL;

    WriteFuncStruct sWriteFuncData;
    WriteFuncStruct sWriteFuncHeaderData;
    CPLHTTPRetryContext oRetryContext(m_oRetryParameters);

retry:
    CURL *hCurlHandle = curl_easy_init();
    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

    if (!AllowAutomaticRedirection())
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FOLLOWLOCATION, 0);

    VSICURLInitWriteFuncStruct(&sWriteFuncData, this, pfnReadCbk,
                               pReadCbkUserData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    VSICURLInitWriteFuncStruct(&sWriteFuncHeaderData, nullptr, nullptr,
                               nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                               &sWriteFuncHeaderData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                               VSICurlHandleWriteFunc);
    sWriteFuncHeaderData.bIsHTTP = STARTS_WITH(m_pszURL, "http");
    sWriteFuncHeaderData.nStartOffset = startOffset;
    sWriteFuncHeaderData.nEndOffset =
        startOffset +
        static_cast<vsi_l_offset>(nBlocks) * VSICURLGetDownloadChunkSize() - 1;
    // Some servers don't like we try to read after end-of-file (#5786).
    if (oFileProp.bHasComputedFileSize &&
        sWriteFuncHeaderData.nEndOffset >= oFileProp.fileSize)
    {
        sWriteFuncHeaderData.nEndOffset = oFileProp.fileSize - 1;
    }

    char rangeStr[512] = {};
    snprintf(rangeStr, sizeof(rangeStr), CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
             startOffset, sWriteFuncHeaderData.nEndOffset);

    if (ENABLE_DEBUG)
        CPLDebug(poFS->GetDebugKey(), "Downloading %s (%s)...", rangeStr,
                 osURL.c_str());

    std::string osHeaderRange;  // leave in this scope
    if (sWriteFuncHeaderData.bIsHTTP)
    {
        osHeaderRange = CPLSPrintf("Range: bytes=%s", rangeStr);
        // So it gets included in Azure signature
        headers = curl_slist_append(headers, osHeaderRange.c_str());
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, nullptr);
    }
    else
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, rangeStr);

    char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
    szCurlErrBuf[0] = '\0';
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    headers = GetCurlHeaders("GET", headers);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FILETIME, 1);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle, &m_bInterrupt);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

    if (sWriteFuncData.bInterrupted || m_bInterrupt)
    {
        bInterrupted = true;

        // Notify that the download of the current region is finished
        currentDownload.SetData(std::string());

        CPLFree(sWriteFuncData.pBuffer);
        CPLFree(sWriteFuncHeaderData.pBuffer);
        curl_easy_cleanup(hCurlHandle);

        return std::string();
    }

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    if (ENABLE_DEBUG && szCurlErrBuf[0] != '\0')
    {
        CPLDebug(poFS->GetDebugKey(),
                 "DownloadRegion(%s): response_code=%d, msg=%s", osURL.c_str(),
                 static_cast<int>(response_code), szCurlErrBuf);
    }

    long mtime = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_FILETIME, &mtime);
    if (mtime > 0)
    {
        oFileProp.mTime = mtime;
        poFS->SetCachedFileProp(m_pszURL, oFileProp);
    }

    if (ENABLE_DEBUG)
        CPLDebug(poFS->GetDebugKey(), "Got response_code=%ld", response_code);

    if (bUsedRedirect &&
        (response_code == 403 ||
         // Below case is in particular for
         // gdalinfo
         // /vsicurl/https://lpdaac.earthdata.nasa.gov/lp-prod-protected/HLSS30.015/HLS.S30.T10TEK.2020273T190109.v1.5.B8A.tif
         // --config GDAL_DISABLE_READDIR_ON_OPEN EMPTY_DIR --config
         // GDAL_HTTP_COOKIEFILE /tmp/cookie.txt --config GDAL_HTTP_COOKIEJAR
         // /tmp/cookie.txt We got the redirect URL from a HEAD request, but it
         // is not valid for a GET. So retry with GET on original URL to get a
         // redirect URL valid for it.
         (response_code == 400 &&
          osURL.find(".cloudfront.net") != std::string::npos)))
    {
        CPLDebug(poFS->GetDebugKey(),
                 "Got an error with redirect URL. Retrying with original one");
        oFileProp.bS3LikeRedirect = false;
        poFS->SetCachedFileProp(m_pszURL, oFileProp);
        bUsedRedirect = false;
        osURL = m_pszURL;
        CPLFree(sWriteFuncData.pBuffer);
        CPLFree(sWriteFuncHeaderData.pBuffer);
        curl_easy_cleanup(hCurlHandle);
        goto retry;
    }

    if (response_code == 401 && oRetryContext.CanRetry())
    {
        CPLDebug(poFS->GetDebugKey(), "Unauthorized, trying to authenticate");
        CPLFree(sWriteFuncData.pBuffer);
        CPLFree(sWriteFuncHeaderData.pBuffer);
        curl_easy_cleanup(hCurlHandle);
        if (Authenticate(m_osFilename.c_str()))
            goto retry;
        return std::string();
    }

    UpdateRedirectInfo(hCurlHandle, sWriteFuncHeaderData);

    if ((response_code != 200 && response_code != 206 && response_code != 225 &&
         response_code != 226 && response_code != 426) ||
        sWriteFuncHeaderData.bError)
    {
        if (sWriteFuncData.pBuffer != nullptr &&
            CanRestartOnError(
                reinterpret_cast<const char *>(sWriteFuncData.pBuffer),
                reinterpret_cast<const char *>(sWriteFuncHeaderData.pBuffer),
                true))
        {
            CPLFree(sWriteFuncData.pBuffer);
            CPLFree(sWriteFuncHeaderData.pBuffer);
            curl_easy_cleanup(hCurlHandle);
            goto begin;
        }

        // Look if we should attempt a retry
        if (oRetryContext.CanRetry(static_cast<int>(response_code),
                                   sWriteFuncHeaderData.pBuffer, szCurlErrBuf))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "HTTP error code: %d - %s. "
                     "Retrying again in %.1f secs",
                     static_cast<int>(response_code), m_pszURL,
                     oRetryContext.GetCurrentDelay());
            CPLSleep(oRetryContext.GetCurrentDelay());
            CPLFree(sWriteFuncData.pBuffer);
            CPLFree(sWriteFuncHeaderData.pBuffer);
            curl_easy_cleanup(hCurlHandle);
            goto retry;
        }

        if (response_code >= 400 && szCurlErrBuf[0] != '\0')
        {
            if (strcmp(szCurlErrBuf, "Couldn't use REST") == 0)
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "%d: %s, Range downloading not supported by this server!",
                    static_cast<int>(response_code), szCurlErrBuf);
            else
                CPLError(CE_Failure, CPLE_AppDefined, "%d: %s",
                         static_cast<int>(response_code), szCurlErrBuf);
        }
        else if (response_code == 416) /* Range Not Satisfiable */
        {
            if (sWriteFuncData.pBuffer)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "%d: Range downloading not supported by this server: %s",
                    static_cast<int>(response_code), sWriteFuncData.pBuffer);
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "%d: Range downloading not supported by this server",
                         static_cast<int>(response_code));
            }
        }
        if (!oFileProp.bHasComputedFileSize && startOffset == 0)
        {
            oFileProp.bHasComputedFileSize = true;
            oFileProp.fileSize = 0;
            oFileProp.eExists = EXIST_NO;
            poFS->SetCachedFileProp(m_pszURL, oFileProp);
        }
        CPLFree(sWriteFuncData.pBuffer);
        CPLFree(sWriteFuncHeaderData.pBuffer);
        curl_easy_cleanup(hCurlHandle);
        return std::string();
    }

    if (!oFileProp.bHasComputedFileSize && sWriteFuncHeaderData.pBuffer)
    {
        // Try to retrieve the filesize from the HTTP headers
        // if in the form: "Content-Range: bytes x-y/filesize".
        char *pszContentRange =
            strstr(sWriteFuncHeaderData.pBuffer, "Content-Range: bytes ");
        if (pszContentRange == nullptr)
            pszContentRange =
                strstr(sWriteFuncHeaderData.pBuffer, "content-range: bytes ");
        if (pszContentRange)
        {
            char *pszEOL = strchr(pszContentRange, '\n');
            if (pszEOL)
            {
                *pszEOL = 0;
                pszEOL = strchr(pszContentRange, '\r');
                if (pszEOL)
                    *pszEOL = 0;
                char *pszSlash = strchr(pszContentRange, '/');
                if (pszSlash)
                {
                    pszSlash++;
                    oFileProp.fileSize = CPLScanUIntBig(
                        pszSlash, static_cast<int>(strlen(pszSlash)));
                }
            }
        }
        else if (STARTS_WITH(m_pszURL, "ftp"))
        {
            // Parse 213 answer for FTP protocol.
            char *pszSize = strstr(sWriteFuncHeaderData.pBuffer, "213 ");
            if (pszSize)
            {
                pszSize += 4;
                char *pszEOL = strchr(pszSize, '\n');
                if (pszEOL)
                {
                    *pszEOL = 0;
                    pszEOL = strchr(pszSize, '\r');
                    if (pszEOL)
                        *pszEOL = 0;

                    oFileProp.fileSize = CPLScanUIntBig(
                        pszSize, static_cast<int>(strlen(pszSize)));
                }
            }
        }

        if (oFileProp.fileSize != 0)
        {
            oFileProp.eExists = EXIST_YES;

            if (ENABLE_DEBUG)
                CPLDebug(poFS->GetDebugKey(),
                         "GetFileSize(%s)=" CPL_FRMT_GUIB "  response_code=%d",
                         m_pszURL, oFileProp.fileSize,
                         static_cast<int>(response_code));

            oFileProp.bHasComputedFileSize = true;
            poFS->SetCachedFileProp(m_pszURL, oFileProp);
        }
    }

    DownloadRegionPostProcess(startOffset, nBlocks, sWriteFuncData.pBuffer,
                              sWriteFuncData.nSize);

    std::string osRet;
    osRet.assign(sWriteFuncData.pBuffer, sWriteFuncData.nSize);

    // Notify that the download of the current region is finished
    currentDownload.SetData(osRet);

    CPLFree(sWriteFuncData.pBuffer);
    CPLFree(sWriteFuncHeaderData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    return osRet;
}

/************************************************************************/
/*                      UpdateRedirectInfo()                            */
/************************************************************************/

void VSICurlHandle::UpdateRedirectInfo(
    CURL *hCurlHandle, const WriteFuncStruct &sWriteFuncHeaderData)
{
    std::string osEffectiveURL;
    {
        char *pszEffectiveURL = nullptr;
        curl_easy_getinfo(hCurlHandle, CURLINFO_EFFECTIVE_URL,
                          &pszEffectiveURL);
        if (pszEffectiveURL)
            osEffectiveURL = pszEffectiveURL;
    }

    if (!oFileProp.bS3LikeRedirect && !osEffectiveURL.empty() &&
        strstr(osEffectiveURL.c_str(), m_pszURL) == nullptr)
    {
        CPLDebug(poFS->GetDebugKey(), "Effective URL: %s",
                 osEffectiveURL.c_str());

        long response_code = 0;
        curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);
        if (response_code >= 200 && response_code < 300 &&
            sWriteFuncHeaderData.nTimestampDate > 0 &&
            VSICurlIsS3LikeSignedURL(osEffectiveURL.c_str()) &&
            !VSICurlIsS3LikeSignedURL(m_pszURL) &&
            CPLTestBool(
                CPLGetConfigOption("CPL_VSIL_CURL_USE_S3_REDIRECT", "TRUE")))
        {
            GIntBig nExpireTimestamp =
                VSICurlGetExpiresFromS3LikeSignedURL(osEffectiveURL.c_str());
            if (nExpireTimestamp > sWriteFuncHeaderData.nTimestampDate + 10)
            {
                const int nValidity = static_cast<int>(
                    nExpireTimestamp - sWriteFuncHeaderData.nTimestampDate);
                CPLDebug(poFS->GetDebugKey(),
                         "Will use redirect URL for the next %d seconds",
                         nValidity);
                // As our local clock might not be in sync with server clock,
                // figure out the expiration timestamp in local time.
                oFileProp.bS3LikeRedirect = true;
                oFileProp.nExpireTimestampLocal = time(nullptr) + nValidity;
                oFileProp.osRedirectURL = std::move(osEffectiveURL);
                poFS->SetCachedFileProp(m_pszURL, oFileProp);
            }
        }
    }
}

/************************************************************************/
/*                      DownloadRegionPostProcess()                     */
/************************************************************************/

void VSICurlHandle::DownloadRegionPostProcess(const vsi_l_offset startOffset,
                                              const int nBlocks,
                                              const char *pBuffer, size_t nSize)
{
    const int knDOWNLOAD_CHUNK_SIZE = VSICURLGetDownloadChunkSize();
    lastDownloadedOffset = startOffset + static_cast<vsi_l_offset>(nBlocks) *
                                             knDOWNLOAD_CHUNK_SIZE;

    if (nSize > static_cast<size_t>(nBlocks) * knDOWNLOAD_CHUNK_SIZE)
    {
        if (ENABLE_DEBUG)
            CPLDebug(
                poFS->GetDebugKey(),
                "Got more data than expected : %u instead of %u",
                static_cast<unsigned int>(nSize),
                static_cast<unsigned int>(nBlocks * knDOWNLOAD_CHUNK_SIZE));
    }

    vsi_l_offset l_startOffset = startOffset;
    while (nSize > 0)
    {
#if DEBUG_VERBOSE
        if (ENABLE_DEBUG)
            CPLDebug(poFS->GetDebugKey(), "Add region %u - %u",
                     static_cast<unsigned int>(startOffset),
                     static_cast<unsigned int>(std::min(
                         static_cast<size_t>(knDOWNLOAD_CHUNK_SIZE), nSize)));
#endif
        const size_t nChunkSize =
            std::min(static_cast<size_t>(knDOWNLOAD_CHUNK_SIZE), nSize);
        poFS->AddRegion(m_pszURL, l_startOffset, nChunkSize, pBuffer);
        l_startOffset += nChunkSize;
        pBuffer += nChunkSize;
        nSize -= nChunkSize;
    }
}

/************************************************************************/
/*                                Read()                                */
/************************************************************************/

size_t VSICurlHandle::Read(void *const pBufferIn, size_t const nSize,
                           size_t const nMemb)
{
    NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("Read");

    size_t nBufferRequestSize = nSize * nMemb;
    if (nBufferRequestSize == 0)
        return 0;

    void *pBuffer = pBufferIn;

#if DEBUG_VERBOSE
    CPLDebug(poFS->GetDebugKey(), "offset=%d, size=%d",
             static_cast<int>(curOffset), static_cast<int>(nBufferRequestSize));
#endif

    vsi_l_offset iterOffset = curOffset;
    const int knMAX_REGIONS = GetMaxRegions();
    const int knDOWNLOAD_CHUNK_SIZE = VSICURLGetDownloadChunkSize();
    while (nBufferRequestSize)
    {
        // Don't try to read after end of file.
        poFS->GetCachedFileProp(m_pszURL, oFileProp);
        if (oFileProp.bHasComputedFileSize && iterOffset >= oFileProp.fileSize)
        {
            if (iterOffset == curOffset)
            {
                CPLDebug(poFS->GetDebugKey(),
                         "Request at offset " CPL_FRMT_GUIB
                         ", after end of file",
                         iterOffset);
            }
            break;
        }

        const vsi_l_offset nOffsetToDownload =
            (iterOffset / knDOWNLOAD_CHUNK_SIZE) * knDOWNLOAD_CHUNK_SIZE;
        std::string osRegion;
        std::shared_ptr<std::string> psRegion =
            poFS->GetRegion(m_pszURL, nOffsetToDownload);
        if (psRegion != nullptr)
        {
            osRegion = *psRegion;
        }
        else
        {
            if (nOffsetToDownload == lastDownloadedOffset)
            {
                // In case of consecutive reads (of small size), we use a
                // heuristic that we will read the file sequentially, so
                // we double the requested size to decrease the number of
                // client/server roundtrips.
                constexpr int MAX_CHUNK_SIZE_INCREASE_FACTOR = 128;
                if (nBlocksToDownload < MAX_CHUNK_SIZE_INCREASE_FACTOR)
                    nBlocksToDownload *= 2;
            }
            else
            {
                // Random reads. Cancel the above heuristics.
                nBlocksToDownload = 1;
            }

            // Ensure that we will request at least the number of blocks
            // to satisfy the remaining buffer size to read.
            const vsi_l_offset nEndOffsetToDownload =
                ((iterOffset + nBufferRequestSize + knDOWNLOAD_CHUNK_SIZE - 1) /
                 knDOWNLOAD_CHUNK_SIZE) *
                knDOWNLOAD_CHUNK_SIZE;
            const int nMinBlocksToDownload =
                static_cast<int>((nEndOffsetToDownload - nOffsetToDownload) /
                                 knDOWNLOAD_CHUNK_SIZE);
            if (nBlocksToDownload < nMinBlocksToDownload)
                nBlocksToDownload = nMinBlocksToDownload;

            // Avoid reading already cached data.
            // Note: this might get evicted if concurrent reads are done, but
            // this should not cause bugs. Just missed optimization.
            for (int i = 1; i < nBlocksToDownload; i++)
            {
                if (poFS->GetRegion(m_pszURL, nOffsetToDownload +
                                                  static_cast<vsi_l_offset>(i) *
                                                      knDOWNLOAD_CHUNK_SIZE) !=
                    nullptr)
                {
                    nBlocksToDownload = i;
                    break;
                }
            }

            // We can't download more than knMAX_REGIONS chunks at a time,
            // otherwise the cache will not be big enough to store them and
            // copy their content to the target buffer.
            if (nBlocksToDownload > knMAX_REGIONS)
                nBlocksToDownload = knMAX_REGIONS;

            osRegion = DownloadRegion(nOffsetToDownload, nBlocksToDownload);
            if (osRegion.empty())
            {
                if (!bInterrupted)
                    bError = true;
                return 0;
            }
        }

        const vsi_l_offset nRegionOffset = iterOffset - nOffsetToDownload;
        if (osRegion.size() < nRegionOffset)
        {
            if (iterOffset == curOffset)
            {
                CPLDebug(poFS->GetDebugKey(),
                         "Request at offset " CPL_FRMT_GUIB
                         ", after end of file",
                         iterOffset);
            }
            break;
        }

        const int nToCopy = static_cast<int>(
            std::min(static_cast<vsi_l_offset>(nBufferRequestSize),
                     osRegion.size() - nRegionOffset));
        memcpy(pBuffer, osRegion.data() + nRegionOffset, nToCopy);
        pBuffer = static_cast<char *>(pBuffer) + nToCopy;
        iterOffset += nToCopy;
        nBufferRequestSize -= nToCopy;
        if (osRegion.size() < static_cast<size_t>(knDOWNLOAD_CHUNK_SIZE) &&
            nBufferRequestSize != 0)
        {
            break;
        }
    }

    const size_t ret = static_cast<size_t>((iterOffset - curOffset) / nSize);
    if (ret != nMemb)
        bEOF = true;

    curOffset = iterOffset;

    return ret;
}

/************************************************************************/
/*                           ReadMultiRange()                           */
/************************************************************************/

int VSICurlHandle::ReadMultiRange(int const nRanges, void **const ppData,
                                  const vsi_l_offset *const panOffsets,
                                  const size_t *const panSizes)
{
    if (bInterrupted && bStopOnInterruptUntilUninstall)
        return FALSE;

    poFS->GetCachedFileProp(m_pszURL, oFileProp);
    if (oFileProp.eExists == EXIST_NO)
        return -1;

    NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("ReadMultiRange");

    const char *pszMultiRangeStrategy =
        CPLGetConfigOption("GDAL_HTTP_MULTIRANGE", "");
    if (EQUAL(pszMultiRangeStrategy, "SINGLE_GET"))
    {
        // Just in case someone needs it, but the interest of this mode is
        // rather dubious now. We could probably remove it
        return ReadMultiRangeSingleGet(nRanges, ppData, panOffsets, panSizes);
    }
    else if (nRanges == 1 || EQUAL(pszMultiRangeStrategy, "SERIAL"))
    {
        return VSIVirtualHandle::ReadMultiRange(nRanges, ppData, panOffsets,
                                                panSizes);
    }

    UpdateQueryString();

    bool bHasExpired = false;

    CPLStringList aosHTTPOptions(m_aosHTTPOptions);
    std::string osURL(GetRedirectURLIfValid(bHasExpired, aosHTTPOptions));
    if (bHasExpired)
    {
        return VSIVirtualHandle::ReadMultiRange(nRanges, ppData, panOffsets,
                                                panSizes);
    }

    CURLM *hMultiHandle = poFS->GetCurlMultiHandleFor(osURL);
#ifdef CURLPIPE_MULTIPLEX
    // Enable HTTP/2 multiplexing (ignored if an older version of HTTP is
    // used)
    // Not that this does not enable HTTP/1.1 pipeling, which is not
    // recommended for example by Google Cloud Storage.
    // For HTTP/1.1, parallel connections work better since you can get
    // results out of order.
    if (CPLTestBool(CPLGetConfigOption("GDAL_HTTP_MULTIPLEX", "YES")))
    {
        curl_multi_setopt(hMultiHandle, CURLMOPT_PIPELINING,
                          CURLPIPE_MULTIPLEX);
    }
#endif

    std::vector<CURL *> aHandles;
    std::vector<WriteFuncStruct> asWriteFuncData(nRanges);
    std::vector<WriteFuncStruct> asWriteFuncHeaderData(nRanges);
    std::vector<char *> apszRanges;
    std::vector<struct curl_slist *> aHeaders;

    struct CurlErrBuffer
    {
        std::array<char, CURL_ERROR_SIZE + 1> szCurlErrBuf;
    };

    std::vector<CurlErrBuffer> asCurlErrors(nRanges);

    const bool bMergeConsecutiveRanges = CPLTestBool(
        CPLGetConfigOption("GDAL_HTTP_MERGE_CONSECUTIVE_RANGES", "TRUE"));

    for (int i = 0, iRequest = 0; i < nRanges;)
    {
        size_t nSize = 0;
        int iNext = i;
        // Identify consecutive ranges
        while (bMergeConsecutiveRanges && iNext + 1 < nRanges &&
               panOffsets[iNext] + panSizes[iNext] == panOffsets[iNext + 1])
        {
            nSize += panSizes[iNext];
            iNext++;
        }
        nSize += panSizes[iNext];

        if (nSize == 0)
        {
            i = iNext + 1;
            continue;
        }

        CURL *hCurlHandle = curl_easy_init();
        aHandles.push_back(hCurlHandle);

        // As the multi-range request is likely not the first one, we don't
        // need to wait as we already know if pipelining is possible
        // unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_PIPEWAIT, 1);

        struct curl_slist *headers = VSICurlSetOptions(
            hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

        VSICURLInitWriteFuncStruct(&asWriteFuncData[iRequest], this, pfnReadCbk,
                                   pReadCbkUserData);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA,
                                   &asWriteFuncData[iRequest]);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                                   VSICurlHandleWriteFunc);

        VSICURLInitWriteFuncStruct(&asWriteFuncHeaderData[iRequest], nullptr,
                                   nullptr, nullptr);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                                   &asWriteFuncHeaderData[iRequest]);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                                   VSICurlHandleWriteFunc);
        asWriteFuncHeaderData[iRequest].bIsHTTP = STARTS_WITH(m_pszURL, "http");
        asWriteFuncHeaderData[iRequest].nStartOffset = panOffsets[i];

        asWriteFuncHeaderData[iRequest].nEndOffset = panOffsets[i] + nSize - 1;

        char rangeStr[512] = {};
        snprintf(rangeStr, sizeof(rangeStr), CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
                 asWriteFuncHeaderData[iRequest].nStartOffset,
                 asWriteFuncHeaderData[iRequest].nEndOffset);

        if (ENABLE_DEBUG)
            CPLDebug(poFS->GetDebugKey(), "Downloading %s (%s)...", rangeStr,
                     osURL.c_str());

        if (asWriteFuncHeaderData[iRequest].bIsHTTP)
        {
            // So it gets included in Azure signature
            char *pszRange = CPLStrdup(CPLSPrintf("Range: bytes=%s", rangeStr));
            apszRanges.push_back(pszRange);
            headers = curl_slist_append(headers, pszRange);
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, nullptr);
        }
        else
        {
            apszRanges.push_back(nullptr);
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, rangeStr);
        }

        asCurlErrors[iRequest].szCurlErrBuf[0] = '\0';
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER,
                                   &asCurlErrors[iRequest].szCurlErrBuf[0]);

        headers = GetCurlHeaders("GET", headers);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);
        aHeaders.push_back(headers);
        curl_multi_add_handle(hMultiHandle, hCurlHandle);

        i = iNext + 1;
        iRequest++;
    }

    if (!aHandles.empty())
    {
        VSICURLMultiPerform(hMultiHandle);
    }

    int nRet = 0;
    size_t iReq = 0;
    int iRange = 0;
    size_t nTotalDownloaded = 0;
    for (; iReq < aHandles.size(); iReq++, iRange++)
    {
        while (iRange < nRanges && panSizes[iRange] == 0)
        {
            iRange++;
        }
        if (iRange == nRanges)
            break;

        long response_code = 0;
        curl_easy_getinfo(aHandles[iReq], CURLINFO_HTTP_CODE, &response_code);

        if (ENABLE_DEBUG && asCurlErrors[iRange].szCurlErrBuf[0] != '\0')
        {
            char rangeStr[512] = {};
            snprintf(rangeStr, sizeof(rangeStr),
                     CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
                     asWriteFuncHeaderData[iReq].nStartOffset,
                     asWriteFuncHeaderData[iReq].nEndOffset);

            const char *pszErrorMsg = &asCurlErrors[iRange].szCurlErrBuf[0];
            CPLDebug(poFS->GetDebugKey(),
                     "ReadMultiRange(%s), %s: response_code=%d, msg=%s",
                     osURL.c_str(), rangeStr, static_cast<int>(response_code),
                     pszErrorMsg);
        }

        if ((response_code != 206 && response_code != 225) ||
            asWriteFuncHeaderData[iReq].nEndOffset + 1 !=
                asWriteFuncHeaderData[iReq].nStartOffset +
                    asWriteFuncData[iReq].nSize)
        {
            char rangeStr[512] = {};
            snprintf(rangeStr, sizeof(rangeStr),
                     CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
                     asWriteFuncHeaderData[iReq].nStartOffset,
                     asWriteFuncHeaderData[iReq].nEndOffset);

            CPLError(CE_Failure, CPLE_AppDefined,
                     "Request for %s failed with response_code=%ld", rangeStr,
                     response_code);
            nRet = -1;
        }
        else if (nRet == 0)
        {
            size_t nOffset = 0;
            size_t nRemainingSize = asWriteFuncData[iReq].nSize;
            nTotalDownloaded += nRemainingSize;
            CPLAssert(iRange < nRanges);
            while (true)
            {
                if (nRemainingSize < panSizes[iRange])
                {
                    nRet = -1;
                    break;
                }

                if (panSizes[iRange] > 0)
                {
                    memcpy(ppData[iRange],
                           asWriteFuncData[iReq].pBuffer + nOffset,
                           panSizes[iRange]);
                }

                if (bMergeConsecutiveRanges && iRange + 1 < nRanges &&
                    panOffsets[iRange] + panSizes[iRange] ==
                        panOffsets[iRange + 1])
                {
                    nOffset += panSizes[iRange];
                    nRemainingSize -= panSizes[iRange];
                    iRange++;
                }
                else
                {
                    break;
                }
            }
        }

        curl_multi_remove_handle(hMultiHandle, aHandles[iReq]);
        VSICURLResetHeaderAndWriterFunctions(aHandles[iReq]);
        curl_easy_cleanup(aHandles[iReq]);
        CPLFree(apszRanges[iReq]);
        CPLFree(asWriteFuncData[iReq].pBuffer);
        CPLFree(asWriteFuncHeaderData[iReq].pBuffer);
        curl_slist_free_all(aHeaders[iReq]);
    }

    NetworkStatisticsLogger::LogGET(nTotalDownloaded);

    if (ENABLE_DEBUG)
        CPLDebug(poFS->GetDebugKey(), "Download completed");

    return nRet;
}

/************************************************************************/
/*                       ReadMultiRangeSingleGet()                      */
/************************************************************************/

// TODO: the interest of this mode is rather dubious now. We could probably
// remove it
int VSICurlHandle::ReadMultiRangeSingleGet(int const nRanges,
                                           void **const ppData,
                                           const vsi_l_offset *const panOffsets,
                                           const size_t *const panSizes)
{
    std::string osRanges;
    std::string osFirstRange;
    std::string osLastRange;
    int nMergedRanges = 0;
    vsi_l_offset nTotalReqSize = 0;
    for (int i = 0; i < nRanges; i++)
    {
        std::string osCurRange;
        if (i != 0)
            osRanges.append(",");
        osCurRange = CPLSPrintf(CPL_FRMT_GUIB "-", panOffsets[i]);
        while (i + 1 < nRanges &&
               panOffsets[i] + panSizes[i] == panOffsets[i + 1])
        {
            nTotalReqSize += panSizes[i];
            i++;
        }
        nTotalReqSize += panSizes[i];
        osCurRange.append(
            CPLSPrintf(CPL_FRMT_GUIB, panOffsets[i] + panSizes[i] - 1));
        nMergedRanges++;

        osRanges += osCurRange;

        if (nMergedRanges == 1)
            osFirstRange = osCurRange;
        osLastRange = std::move(osCurRange);
    }

    const char *pszMaxRanges =
        CPLGetConfigOption("CPL_VSIL_CURL_MAX_RANGES", "250");
    int nMaxRanges = atoi(pszMaxRanges);
    if (nMaxRanges <= 0)
        nMaxRanges = 250;
    if (nMergedRanges > nMaxRanges)
    {
        const int nHalf = nRanges / 2;
        const int nRet = ReadMultiRange(nHalf, ppData, panOffsets, panSizes);
        if (nRet != 0)
            return nRet;
        return ReadMultiRange(nRanges - nHalf, ppData + nHalf,
                              panOffsets + nHalf, panSizes + nHalf);
    }

    CURLM *hCurlMultiHandle = poFS->GetCurlMultiHandleFor(m_pszURL);
    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, m_pszURL, m_aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncData;
    WriteFuncStruct sWriteFuncHeaderData;

    VSICURLInitWriteFuncStruct(&sWriteFuncData, this, pfnReadCbk,
                               pReadCbkUserData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    VSICURLInitWriteFuncStruct(&sWriteFuncHeaderData, nullptr, nullptr,
                               nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                               &sWriteFuncHeaderData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                               VSICurlHandleWriteFunc);
    sWriteFuncHeaderData.bIsHTTP = STARTS_WITH(m_pszURL, "http");
    sWriteFuncHeaderData.bMultiRange = nMergedRanges > 1;
    if (nMergedRanges == 1)
    {
        sWriteFuncHeaderData.nStartOffset = panOffsets[0];
        sWriteFuncHeaderData.nEndOffset = panOffsets[0] + nTotalReqSize - 1;
    }

    if (ENABLE_DEBUG)
    {
        if (nMergedRanges == 1)
            CPLDebug(poFS->GetDebugKey(), "Downloading %s (%s)...",
                     osRanges.c_str(), m_pszURL);
        else
            CPLDebug(poFS->GetDebugKey(),
                     "Downloading %s, ..., %s (" CPL_FRMT_GUIB " bytes, %s)...",
                     osFirstRange.c_str(), osLastRange.c_str(),
                     static_cast<GUIntBig>(nTotalReqSize), m_pszURL);
    }

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, osRanges.c_str());

    char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    headers = GetCurlHeaders("GET", headers);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

    if (sWriteFuncData.bInterrupted)
    {
        bInterrupted = true;

        CPLFree(sWriteFuncData.pBuffer);
        CPLFree(sWriteFuncHeaderData.pBuffer);
        curl_easy_cleanup(hCurlHandle);

        return -1;
    }

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    if ((response_code != 200 && response_code != 206 && response_code != 225 &&
         response_code != 226 && response_code != 426) ||
        sWriteFuncHeaderData.bError)
    {
        if (response_code >= 400 && szCurlErrBuf[0] != '\0')
        {
            if (strcmp(szCurlErrBuf, "Couldn't use REST") == 0)
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "%d: %s, Range downloading not supported by this server!",
                    static_cast<int>(response_code), szCurlErrBuf);
            else
                CPLError(CE_Failure, CPLE_AppDefined, "%d: %s",
                         static_cast<int>(response_code), szCurlErrBuf);
        }
        /*
        if( !bHasComputedFileSize && startOffset == 0 )
        {
            cachedFileProp->bHasComputedFileSize = bHasComputedFileSize = true;
            cachedFileProp->fileSize = fileSize = 0;
            cachedFileProp->eExists = eExists = EXIST_NO;
        }
        */
        CPLFree(sWriteFuncData.pBuffer);
        CPLFree(sWriteFuncHeaderData.pBuffer);
        curl_easy_cleanup(hCurlHandle);
        return -1;
    }

    char *pBuffer = sWriteFuncData.pBuffer;
    size_t nSize = sWriteFuncData.nSize;

    // TODO(schwehr): Localize after removing gotos.
    int nRet = -1;
    char *pszBoundary;
    std::string osBoundary;
    char *pszNext = nullptr;
    int iRange = 0;
    int iPart = 0;
    char *pszEOL = nullptr;

    /* -------------------------------------------------------------------- */
    /*      No multipart if a single range has been requested               */
    /* -------------------------------------------------------------------- */

    if (nMergedRanges == 1)
    {
        size_t nAccSize = 0;
        if (static_cast<vsi_l_offset>(nSize) < nTotalReqSize)
            goto end;

        for (int i = 0; i < nRanges; i++)
        {
            memcpy(ppData[i], pBuffer + nAccSize, panSizes[i]);
            nAccSize += panSizes[i];
        }

        nRet = 0;
        goto end;
    }

    /* -------------------------------------------------------------------- */
    /*      Extract boundary name                                           */
    /* -------------------------------------------------------------------- */

    pszBoundary = strstr(sWriteFuncHeaderData.pBuffer,
                         "Content-Type: multipart/byteranges; boundary=");
    if (pszBoundary == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Could not find '%s'",
                 "Content-Type: multipart/byteranges; boundary=");
        goto end;
    }

    pszBoundary += strlen("Content-Type: multipart/byteranges; boundary=");

    pszEOL = strchr(pszBoundary, '\r');
    if (pszEOL)
        *pszEOL = 0;
    pszEOL = strchr(pszBoundary, '\n');
    if (pszEOL)
        *pszEOL = 0;

    /* Remove optional double-quote character around boundary name */
    if (pszBoundary[0] == '"')
    {
        pszBoundary++;
        char *pszLastDoubleQuote = strrchr(pszBoundary, '"');
        if (pszLastDoubleQuote)
            *pszLastDoubleQuote = 0;
    }

    osBoundary = "--";
    osBoundary += pszBoundary;

    /* -------------------------------------------------------------------- */
    /*      Find the start of the first chunk.                              */
    /* -------------------------------------------------------------------- */
    pszNext = strstr(pBuffer, osBoundary.c_str());
    if (pszNext == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "No parts found.");
        goto end;
    }

    pszNext += osBoundary.size();
    while (*pszNext != '\n' && *pszNext != '\r' && *pszNext != '\0')
        pszNext++;
    if (*pszNext == '\r')
        pszNext++;
    if (*pszNext == '\n')
        pszNext++;

    /* -------------------------------------------------------------------- */
    /*      Loop over parts...                                              */
    /* -------------------------------------------------------------------- */
    while (iPart < nRanges)
    {
        /* --------------------------------------------------------------------
         */
        /*      Collect headers. */
        /* --------------------------------------------------------------------
         */
        bool bExpectedRange = false;

        while (*pszNext != '\n' && *pszNext != '\r' && *pszNext != '\0')
        {
            pszEOL = strstr(pszNext, "\n");

            if (pszEOL == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Error while parsing multipart content (at line %d)",
                         __LINE__);
                goto end;
            }

            *pszEOL = '\0';
            bool bRestoreAntislashR = false;
            if (pszEOL - pszNext > 1 && pszEOL[-1] == '\r')
            {
                bRestoreAntislashR = true;
                pszEOL[-1] = '\0';
            }

            if (STARTS_WITH_CI(pszNext, "Content-Range: bytes "))
            {
                bExpectedRange = true; /* FIXME */
            }

            if (bRestoreAntislashR)
                pszEOL[-1] = '\r';
            *pszEOL = '\n';

            pszNext = pszEOL + 1;
        }

        if (!bExpectedRange)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error while parsing multipart content (at line %d)",
                     __LINE__);
            goto end;
        }

        if (*pszNext == '\r')
            pszNext++;
        if (*pszNext == '\n')
            pszNext++;

        /* --------------------------------------------------------------------
         */
        /*      Work out the data block size. */
        /* --------------------------------------------------------------------
         */
        size_t nBytesAvail = nSize - (pszNext - pBuffer);

        while (true)
        {
            if (nBytesAvail < panSizes[iRange])
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Error while parsing multipart content (at line %d)",
                         __LINE__);
                goto end;
            }

            memcpy(ppData[iRange], pszNext, panSizes[iRange]);
            pszNext += panSizes[iRange];
            nBytesAvail -= panSizes[iRange];
            if (iRange + 1 < nRanges &&
                panOffsets[iRange] + panSizes[iRange] == panOffsets[iRange + 1])
            {
                iRange++;
            }
            else
            {
                break;
            }
        }

        iPart++;
        iRange++;

        while (nBytesAvail > 0 &&
               (*pszNext != '-' ||
                strncmp(pszNext, osBoundary.c_str(), osBoundary.size()) != 0))
        {
            pszNext++;
            nBytesAvail--;
        }

        if (nBytesAvail == 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error while parsing multipart content (at line %d)",
                     __LINE__);
            goto end;
        }

        pszNext += osBoundary.size();
        if (STARTS_WITH(pszNext, "--"))
        {
            // End of multipart.
            break;
        }

        if (*pszNext == '\r')
            pszNext++;
        if (*pszNext == '\n')
            pszNext++;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Error while parsing multipart content (at line %d)",
                     __LINE__);
            goto end;
        }
    }

    if (iPart == nMergedRanges)
        nRet = 0;
    else
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Got only %d parts, where %d were expected", iPart,
                 nMergedRanges);

end:
    CPLFree(sWriteFuncData.pBuffer);
    CPLFree(sWriteFuncHeaderData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    return nRet;
}

/************************************************************************/
/*                              PRead()                                 */
/************************************************************************/

size_t VSICurlHandle::PRead(void *pBuffer, size_t nSize,
                            vsi_l_offset nOffset) const
{
    // Try to use AdviseRead ranges fetched asynchronously
    if (!m_aoAdviseReadRanges.empty())
    {
        for (auto &poRange : m_aoAdviseReadRanges)
        {
            if (nOffset >= poRange->nStartOffset &&
                nOffset + nSize <= poRange->nStartOffset + poRange->nSize)
            {
                {
                    std::unique_lock<std::mutex> oLock(poRange->oMutex);
                    // coverity[missing_lock:FALSE]
                    while (!poRange->bDone)
                    {
                        poRange->oCV.wait(oLock);
                    }
                }
                if (poRange->abyData.empty())
                    return 0;

                auto nEndOffset =
                    poRange->nStartOffset + poRange->abyData.size();
                if (nOffset >= nEndOffset)
                    return 0;
                const size_t nToCopy = static_cast<size_t>(
                    std::min<vsi_l_offset>(nSize, nEndOffset - nOffset));
                memcpy(pBuffer,
                       poRange->abyData.data() +
                           static_cast<size_t>(nOffset - poRange->nStartOffset),
                       nToCopy);
                return nToCopy;
            }
        }
    }

    // poFS has a global mutex
    poFS->GetCachedFileProp(m_pszURL, oFileProp);
    if (oFileProp.eExists == EXIST_NO)
        return static_cast<size_t>(-1);

    NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("PRead");

    CPLStringList aosHTTPOptions(m_aosHTTPOptions);
    std::string osURL;
    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        UpdateQueryString();
        bool bHasExpired;
        osURL = GetRedirectURLIfValid(bHasExpired, aosHTTPOptions);
    }

    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    WriteFuncStruct sWriteFuncHeaderData;
    VSICURLInitWriteFuncStruct(&sWriteFuncHeaderData, nullptr, nullptr,
                               nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                               &sWriteFuncHeaderData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                               VSICurlHandleWriteFunc);
    sWriteFuncHeaderData.bIsHTTP = STARTS_WITH(m_pszURL, "http");
    sWriteFuncHeaderData.nStartOffset = nOffset;

    sWriteFuncHeaderData.nEndOffset = nOffset + nSize - 1;

    char rangeStr[512] = {};
    snprintf(rangeStr, sizeof(rangeStr), CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
             sWriteFuncHeaderData.nStartOffset,
             sWriteFuncHeaderData.nEndOffset);

#if 0
    if( ENABLE_DEBUG )
        CPLDebug(poFS->GetDebugKey(),
                 "Downloading %s (%s)...", rangeStr, osURL.c_str());
#endif

    std::string osHeaderRange;
    if (sWriteFuncHeaderData.bIsHTTP)
    {
        osHeaderRange = CPLSPrintf("Range: bytes=%s", rangeStr);
        // So it gets included in Azure signature
        headers = curl_slist_append(headers, osHeaderRange.data());
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, nullptr);
    }
    else
    {
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, rangeStr);
    }

    std::array<char, CURL_ERROR_SIZE + 1> szCurlErrBuf;
    szCurlErrBuf[0] = '\0';
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER,
                               &szCurlErrBuf[0]);

    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        headers =
            const_cast<VSICurlHandle *>(this)->GetCurlHeaders("GET", headers);
    }
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    CURLM *hMultiHandle = poFS->GetCurlMultiHandleFor(osURL);
    VSICURLMultiPerform(hMultiHandle, hCurlHandle, &m_bInterrupt);

    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        const_cast<VSICurlHandle *>(this)->UpdateRedirectInfo(
            hCurlHandle, sWriteFuncHeaderData);
    }

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    if (ENABLE_DEBUG && szCurlErrBuf[0] != '\0')
    {
        const char *pszErrorMsg = &szCurlErrBuf[0];
        CPLDebug(poFS->GetDebugKey(), "PRead(%s), %s: response_code=%d, msg=%s",
                 osURL.c_str(), rangeStr, static_cast<int>(response_code),
                 pszErrorMsg);
    }

    size_t nRet;
    if ((response_code != 206 && response_code != 225) ||
        sWriteFuncData.nSize == 0)
    {
        if (!m_bInterrupt)
        {
            CPLDebug(poFS->GetDebugKey(),
                     "Request for %s failed with response_code=%ld", rangeStr,
                     response_code);
        }
        nRet = static_cast<size_t>(-1);
    }
    else
    {
        nRet = std::min(sWriteFuncData.nSize, nSize);
        if (nRet > 0)
            memcpy(pBuffer, sWriteFuncData.pBuffer, nRet);
    }

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);
    curl_easy_cleanup(hCurlHandle);
    CPLFree(sWriteFuncData.pBuffer);
    CPLFree(sWriteFuncHeaderData.pBuffer);
    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

#if 0
    if( ENABLE_DEBUG )
        CPLDebug(poFS->GetDebugKey(), "Download completed");
#endif

    return nRet;
}

/************************************************************************/
/*                  GetAdviseReadTotalBytesLimit()                      */
/************************************************************************/

size_t VSICurlHandle::GetAdviseReadTotalBytesLimit() const
{
    return static_cast<size_t>(std::min<unsigned long long>(
        std::numeric_limits<size_t>::max(),
        // 100 MB
        std::strtoull(
            CPLGetConfigOption("CPL_VSIL_CURL_ADVISE_READ_TOTAL_BYTES_LIMIT",
                               "104857600"),
            nullptr, 10)));
}

/************************************************************************/
/*                       VSICURLMultiInit()                             */
/************************************************************************/

static CURLM *VSICURLMultiInit()
{
    CURLM *hCurlMultiHandle = curl_multi_init();

    if (const char *pszMAXCONNECTS =
            CPLGetConfigOption("GDAL_HTTP_MAX_CACHED_CONNECTIONS", nullptr))
    {
        curl_multi_setopt(hCurlMultiHandle, CURLMOPT_MAXCONNECTS,
                          atoi(pszMAXCONNECTS));
    }

    if (const char *pszMAX_TOTAL_CONNECTIONS =
            CPLGetConfigOption("GDAL_HTTP_MAX_TOTAL_CONNECTIONS", nullptr))
    {
        curl_multi_setopt(hCurlMultiHandle, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                          atoi(pszMAX_TOTAL_CONNECTIONS));
    }

    return hCurlMultiHandle;
}

/************************************************************************/
/*                         AdviseRead()                                 */
/************************************************************************/

void VSICurlHandle::AdviseRead(int nRanges, const vsi_l_offset *panOffsets,
                               const size_t *panSizes)
{
    if (!CPLTestBool(
            CPLGetConfigOption("GDAL_HTTP_ENABLE_ADVISE_READ", "TRUE")))
        return;

    if (m_oThreadAdviseRead.joinable())
    {
        m_oThreadAdviseRead.join();
    }

    // Give up if we need to allocate too much memory
    vsi_l_offset nMaxSize = 0;
    const size_t nLimit = GetAdviseReadTotalBytesLimit();
    for (int i = 0; i < nRanges; ++i)
    {
        if (panSizes[i] > nLimit - nMaxSize)
        {
            CPLDebug(poFS->GetDebugKey(),
                     "Trying to request too many bytes in AdviseRead()");
            return;
        }
        nMaxSize += panSizes[i];
    }

    UpdateQueryString();

    bool bHasExpired = false;
    CPLStringList aosHTTPOptions(m_aosHTTPOptions);
    const std::string l_osURL(
        GetRedirectURLIfValid(bHasExpired, aosHTTPOptions));
    if (bHasExpired)
    {
        return;
    }

    const bool bMergeConsecutiveRanges = CPLTestBool(
        CPLGetConfigOption("GDAL_HTTP_MERGE_CONSECUTIVE_RANGES", "TRUE"));

    try
    {
        m_aoAdviseReadRanges.clear();
        m_aoAdviseReadRanges.reserve(nRanges);
        for (int i = 0; i < nRanges;)
        {
            int iNext = i;
            // Identify consecutive ranges
            constexpr size_t SIZE_COG_MARKERS = 2 * sizeof(uint32_t);
            auto nEndOffset = panOffsets[iNext] + panSizes[iNext];
            while (bMergeConsecutiveRanges && iNext + 1 < nRanges &&
                   panOffsets[iNext + 1] > panOffsets[iNext] &&
                   panOffsets[iNext] + panSizes[iNext] + SIZE_COG_MARKERS >=
                       panOffsets[iNext + 1] &&
                   panOffsets[iNext + 1] + panSizes[iNext + 1] > nEndOffset)
            {
                iNext++;
                nEndOffset = panOffsets[iNext] + panSizes[iNext];
            }
            CPLAssert(panOffsets[i] <= nEndOffset);
            const size_t nSize =
                static_cast<size_t>(nEndOffset - panOffsets[i]);

            if (nSize == 0)
            {
                i = iNext + 1;
                continue;
            }

            auto newAdviseReadRange =
                std::make_unique<AdviseReadRange>(m_oRetryParameters);
            newAdviseReadRange->nStartOffset = panOffsets[i];
            newAdviseReadRange->nSize = nSize;
            newAdviseReadRange->abyData.resize(nSize);
            m_aoAdviseReadRanges.push_back(std::move(newAdviseReadRange));

            i = iNext + 1;
        }
    }
    catch (const std::exception &)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "Out of memory in VSICurlHandle::AdviseRead()");
        m_aoAdviseReadRanges.clear();
    }

    if (m_aoAdviseReadRanges.empty())
        return;

#ifdef DEBUG
    CPLDebug(poFS->GetDebugKey(), "AdviseRead(): fetching %u ranges",
             static_cast<unsigned>(m_aoAdviseReadRanges.size()));
#endif

    const auto task = [this, aosHTTPOptions = std::move(aosHTTPOptions)](
                          const std::string &osURL)
    {
        if (!m_hCurlMultiHandleForAdviseRead)
            m_hCurlMultiHandleForAdviseRead = VSICURLMultiInit();

        NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
        NetworkStatisticsFile oContextFile(m_osFilename.c_str());
        NetworkStatisticsAction oContextAction("AdviseRead");

#ifdef CURLPIPE_MULTIPLEX
        // Enable HTTP/2 multiplexing (ignored if an older version of HTTP is
        // used)
        // Not that this does not enable HTTP/1.1 pipeling, which is not
        // recommended for example by Google Cloud Storage.
        // For HTTP/1.1, parallel connections work better since you can get
        // results out of order.
        if (CPLTestBool(CPLGetConfigOption("GDAL_HTTP_MULTIPLEX", "YES")))
        {
            curl_multi_setopt(m_hCurlMultiHandleForAdviseRead,
                              CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        }
#endif

        size_t nTotalDownloaded = 0;

        while (true)
        {

            std::vector<CURL *> aHandles;
            std::vector<WriteFuncStruct> asWriteFuncData(
                m_aoAdviseReadRanges.size());
            std::vector<WriteFuncStruct> asWriteFuncHeaderData(
                m_aoAdviseReadRanges.size());
            std::vector<char *> apszRanges;
            std::vector<struct curl_slist *> aHeaders;

            struct CurlErrBuffer
            {
                std::array<char, CURL_ERROR_SIZE + 1> szCurlErrBuf;
            };
            std::vector<CurlErrBuffer> asCurlErrors(
                m_aoAdviseReadRanges.size());

            std::map<CURL *, size_t> oMapHandleToIdx;
            for (size_t i = 0; i < m_aoAdviseReadRanges.size(); ++i)
            {
                if (!m_aoAdviseReadRanges[i]->bToRetry)
                {
                    aHandles.push_back(nullptr);
                    apszRanges.push_back(nullptr);
                    aHeaders.push_back(nullptr);
                    continue;
                }
                m_aoAdviseReadRanges[i]->bToRetry = false;

                CURL *hCurlHandle = curl_easy_init();
                oMapHandleToIdx[hCurlHandle] = i;
                aHandles.push_back(hCurlHandle);

                // As the multi-range request is likely not the first one, we don't
                // need to wait as we already know if pipelining is possible
                // unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_PIPEWAIT, 1);

                struct curl_slist *headers = VSICurlSetOptions(
                    hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

                VSICURLInitWriteFuncStruct(&asWriteFuncData[i], this,
                                           pfnReadCbk, pReadCbkUserData);
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA,
                                           &asWriteFuncData[i]);
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                                           VSICurlHandleWriteFunc);

                VSICURLInitWriteFuncStruct(&asWriteFuncHeaderData[i], nullptr,
                                           nullptr, nullptr);
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                                           &asWriteFuncHeaderData[i]);
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                                           VSICurlHandleWriteFunc);
                asWriteFuncHeaderData[i].bIsHTTP =
                    STARTS_WITH(m_pszURL, "http");
                asWriteFuncHeaderData[i].nStartOffset =
                    m_aoAdviseReadRanges[i]->nStartOffset;

                asWriteFuncHeaderData[i].nEndOffset =
                    m_aoAdviseReadRanges[i]->nStartOffset +
                    m_aoAdviseReadRanges[i]->nSize - 1;

                char rangeStr[512] = {};
                snprintf(rangeStr, sizeof(rangeStr),
                         CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
                         asWriteFuncHeaderData[i].nStartOffset,
                         asWriteFuncHeaderData[i].nEndOffset);

                if (ENABLE_DEBUG)
                    CPLDebug(poFS->GetDebugKey(), "Downloading %s (%s)...",
                             rangeStr, osURL.c_str());

                if (asWriteFuncHeaderData[i].bIsHTTP)
                {
                    std::string osHeaderRange(
                        CPLSPrintf("Range: bytes=%s", rangeStr));
                    // So it gets included in Azure signature
                    char *pszRange = CPLStrdup(osHeaderRange.c_str());
                    apszRanges.push_back(pszRange);
                    headers = curl_slist_append(headers, pszRange);
                    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE,
                                               nullptr);
                }
                else
                {
                    apszRanges.push_back(nullptr);
                    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE,
                                               rangeStr);
                }

                asCurlErrors[i].szCurlErrBuf[0] = '\0';
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER,
                                           &asCurlErrors[i].szCurlErrBuf[0]);

                headers = GetCurlHeaders("GET", headers);
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER,
                                           headers);
                aHeaders.push_back(headers);
                curl_multi_add_handle(m_hCurlMultiHandleForAdviseRead,
                                      hCurlHandle);
            }

            const auto DealWithRequest = [this, &osURL, &nTotalDownloaded,
                                          &oMapHandleToIdx, &asCurlErrors,
                                          &asWriteFuncHeaderData,
                                          &asWriteFuncData](CURL *hCurlHandle)
            {
                auto oIter = oMapHandleToIdx.find(hCurlHandle);
                CPLAssert(oIter != oMapHandleToIdx.end());
                const auto iReq = oIter->second;

                long response_code = 0;
                curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE,
                                  &response_code);

                if (ENABLE_DEBUG && asCurlErrors[iReq].szCurlErrBuf[0] != '\0')
                {
                    char rangeStr[512] = {};
                    snprintf(rangeStr, sizeof(rangeStr),
                             CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
                             asWriteFuncHeaderData[iReq].nStartOffset,
                             asWriteFuncHeaderData[iReq].nEndOffset);

                    const char *pszErrorMsg =
                        &asCurlErrors[iReq].szCurlErrBuf[0];
                    CPLDebug(poFS->GetDebugKey(),
                             "ReadMultiRange(%s), %s: response_code=%d, msg=%s",
                             osURL.c_str(), rangeStr,
                             static_cast<int>(response_code), pszErrorMsg);
                }

                bool bToRetry = false;
                if ((response_code != 206 && response_code != 225) ||
                    asWriteFuncHeaderData[iReq].nEndOffset + 1 !=
                        asWriteFuncHeaderData[iReq].nStartOffset +
                            asWriteFuncData[iReq].nSize)
                {
                    char rangeStr[512] = {};
                    snprintf(rangeStr, sizeof(rangeStr),
                             CPL_FRMT_GUIB "-" CPL_FRMT_GUIB,
                             asWriteFuncHeaderData[iReq].nStartOffset,
                             asWriteFuncHeaderData[iReq].nEndOffset);

                    // Look if we should attempt a retry
                    if (m_aoAdviseReadRanges[iReq]->retryContext.CanRetry(
                            static_cast<int>(response_code),
                            asWriteFuncData[iReq].pBuffer,
                            &asCurlErrors[iReq].szCurlErrBuf[0]))
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "HTTP error code for %s range %s: %d. "
                                 "Retrying again in %.1f secs",
                                 osURL.c_str(), rangeStr,
                                 static_cast<int>(response_code),
                                 m_aoAdviseReadRanges[iReq]
                                     ->retryContext.GetCurrentDelay());
                        m_aoAdviseReadRanges[iReq]->dfSleepDelay =
                            m_aoAdviseReadRanges[iReq]
                                ->retryContext.GetCurrentDelay();
                        bToRetry = true;
                    }
                    else
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Request for %s range %s failed with "
                                 "response_code=%ld",
                                 osURL.c_str(), rangeStr, response_code);
                    }
                }
                else
                {
                    const size_t nSize = asWriteFuncData[iReq].nSize;
                    memcpy(&m_aoAdviseReadRanges[iReq]->abyData[0],
                           asWriteFuncData[iReq].pBuffer, nSize);
                    m_aoAdviseReadRanges[iReq]->abyData.resize(nSize);

                    nTotalDownloaded += nSize;
                }

                m_aoAdviseReadRanges[iReq]->bToRetry = bToRetry;

                if (!bToRetry)
                {
                    std::lock_guard<std::mutex> oLock(
                        m_aoAdviseReadRanges[iReq]->oMutex);
                    m_aoAdviseReadRanges[iReq]->bDone = true;
                    m_aoAdviseReadRanges[iReq]->oCV.notify_all();
                }
            };

            int repeats = 0;

            void *old_handler = CPLHTTPIgnoreSigPipe();
            while (true)
            {
                int still_running;
                while (curl_multi_perform(m_hCurlMultiHandleForAdviseRead,
                                          &still_running) ==
                       CURLM_CALL_MULTI_PERFORM)
                {
                    // loop
                }
                if (!still_running)
                {
                    break;
                }

                CURLMsg *msg;
                do
                {
                    int msgq = 0;
                    msg = curl_multi_info_read(m_hCurlMultiHandleForAdviseRead,
                                               &msgq);
                    if (msg && (msg->msg == CURLMSG_DONE))
                    {
                        DealWithRequest(msg->easy_handle);
                    }
                } while (msg);

                CPLMultiPerformWait(m_hCurlMultiHandleForAdviseRead, repeats);
            }
            CPLHTTPRestoreSigPipeHandler(old_handler);

            bool bRetry = false;
            double dfDelay = 0.0;
            for (size_t i = 0; i < m_aoAdviseReadRanges.size(); ++i)
            {
                bool bReqDone;
                {
                    // To please Coverity Scan
                    std::lock_guard<std::mutex> oLock(
                        m_aoAdviseReadRanges[i]->oMutex);
                    bReqDone = m_aoAdviseReadRanges[i]->bDone;
                }
                if (!bReqDone && !m_aoAdviseReadRanges[i]->bToRetry)
                {
                    DealWithRequest(aHandles[i]);
                }
                if (m_aoAdviseReadRanges[i]->bToRetry)
                    dfDelay = std::max(dfDelay,
                                       m_aoAdviseReadRanges[i]->dfSleepDelay);
                bRetry = bRetry || m_aoAdviseReadRanges[i]->bToRetry;
                if (aHandles[i])
                {
                    curl_multi_remove_handle(m_hCurlMultiHandleForAdviseRead,
                                             aHandles[i]);
                    VSICURLResetHeaderAndWriterFunctions(aHandles[i]);
                    curl_easy_cleanup(aHandles[i]);
                }
                CPLFree(apszRanges[i]);
                CPLFree(asWriteFuncData[i].pBuffer);
                CPLFree(asWriteFuncHeaderData[i].pBuffer);
                if (aHeaders[i])
                    curl_slist_free_all(aHeaders[i]);
            }
            if (!bRetry)
                break;
            CPLSleep(dfDelay);
        }

        NetworkStatisticsLogger::LogGET(nTotalDownloaded);
    };

    m_oThreadAdviseRead = std::thread(task, l_osURL);
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

size_t VSICurlHandle::Write(const void * /* pBuffer */, size_t /* nSize */,
                            size_t /* nMemb */)
{
    return 0;
}

/************************************************************************/
/*                             ClearErr()                               */
/************************************************************************/

void VSICurlHandle::ClearErr()

{
    bEOF = false;
    bError = false;
}

/************************************************************************/
/*                              Error()                                 */
/************************************************************************/

int VSICurlHandle::Error()

{
    return bError ? TRUE : FALSE;
}

/************************************************************************/
/*                                Eof()                                 */
/************************************************************************/

int VSICurlHandle::Eof()

{
    return bEOF ? TRUE : FALSE;
}

/************************************************************************/
/*                                 Flush()                              */
/************************************************************************/

int VSICurlHandle::Flush()
{
    return 0;
}

/************************************************************************/
/*                                  Close()                             */
/************************************************************************/

int VSICurlHandle::Close()
{
    return 0;
}

/************************************************************************/
/*                   VSICurlFilesystemHandlerBase()                         */
/************************************************************************/

VSICurlFilesystemHandlerBase::VSICurlFilesystemHandlerBase()
    : oCacheFileProp{100 * 1024}, oCacheDirList{1024, 0}
{
}

/************************************************************************/
/*                           CachedConnection                           */
/************************************************************************/

namespace
{
struct CachedConnection
{
    CURLM *hCurlMultiHandle = nullptr;
    void clear();

    ~CachedConnection()
    {
        clear();
    }
};
}  // namespace

#ifdef _WIN32
// Currently thread_local and C++ objects don't work well with DLL on Windows
static void FreeCachedConnection(void *pData)
{
    delete static_cast<
        std::map<VSICurlFilesystemHandlerBase *, CachedConnection> *>(pData);
}

// Per-thread and per-filesystem Curl connection cache.
static std::map<VSICurlFilesystemHandlerBase *, CachedConnection> &
GetConnectionCache()
{
    static std::map<VSICurlFilesystemHandlerBase *, CachedConnection>
        dummyCache;
    int bMemoryErrorOccurred = false;
    void *pData =
        CPLGetTLSEx(CTLS_VSICURL_CACHEDCONNECTION, &bMemoryErrorOccurred);
    if (bMemoryErrorOccurred)
    {
        return dummyCache;
    }
    if (pData == nullptr)
    {
        auto cachedConnection =
            new std::map<VSICurlFilesystemHandlerBase *, CachedConnection>();
        CPLSetTLSWithFreeFuncEx(CTLS_VSICURL_CACHEDCONNECTION, cachedConnection,
                                FreeCachedConnection, &bMemoryErrorOccurred);
        if (bMemoryErrorOccurred)
        {
            delete cachedConnection;
            return dummyCache;
        }
        return *cachedConnection;
    }
    return *static_cast<
        std::map<VSICurlFilesystemHandlerBase *, CachedConnection> *>(pData);
}
#else
static thread_local std::map<VSICurlFilesystemHandlerBase *, CachedConnection>
    g_tls_connectionCache;

static std::map<VSICurlFilesystemHandlerBase *, CachedConnection> &
GetConnectionCache()
{
    return g_tls_connectionCache;
}
#endif

/************************************************************************/
/*                              clear()                                 */
/************************************************************************/

void CachedConnection::clear()
{
    if (hCurlMultiHandle)
    {
        VSICURLMultiCleanup(hCurlMultiHandle);
        hCurlMultiHandle = nullptr;
    }
}

/************************************************************************/
/*                  ~VSICurlFilesystemHandlerBase()                         */
/************************************************************************/

VSICurlFilesystemHandlerBase::~VSICurlFilesystemHandlerBase()
{
    VSICurlFilesystemHandlerBase::ClearCache();
    GetConnectionCache().erase(this);

    if (hMutex != nullptr)
        CPLDestroyMutex(hMutex);
    hMutex = nullptr;
}

/************************************************************************/
/*                      AllowCachedDataFor()                            */
/************************************************************************/

bool VSICurlFilesystemHandlerBase::AllowCachedDataFor(const char *pszFilename)
{
    bool bCachedAllowed = true;
    char **papszTokens = CSLTokenizeString2(
        CPLGetConfigOption("CPL_VSIL_CURL_NON_CACHED", ""), ":", 0);
    for (int i = 0; papszTokens && papszTokens[i]; i++)
    {
        if (STARTS_WITH(pszFilename, papszTokens[i]))
        {
            bCachedAllowed = false;
            break;
        }
    }
    CSLDestroy(papszTokens);
    return bCachedAllowed;
}

/************************************************************************/
/*                     GetCurlMultiHandleFor()                          */
/************************************************************************/

CURLM *VSICurlFilesystemHandlerBase::GetCurlMultiHandleFor(
    const std::string & /*osURL*/)
{
    auto &conn = GetConnectionCache()[this];
    if (conn.hCurlMultiHandle == nullptr)
    {
        conn.hCurlMultiHandle = VSICURLMultiInit();
    }
    return conn.hCurlMultiHandle;
}

/************************************************************************/
/*                          GetRegionCache()                            */
/************************************************************************/

VSICurlFilesystemHandlerBase::RegionCacheType *
VSICurlFilesystemHandlerBase::GetRegionCache()
{
    // should be called under hMutex taken
    if (m_poRegionCacheDoNotUseDirectly == nullptr)
    {
        m_poRegionCacheDoNotUseDirectly.reset(
            new RegionCacheType(static_cast<size_t>(GetMaxRegions())));
    }
    return m_poRegionCacheDoNotUseDirectly.get();
}

/************************************************************************/
/*                          GetRegion()                                 */
/************************************************************************/

std::shared_ptr<std::string>
VSICurlFilesystemHandlerBase::GetRegion(const char *pszURL,
                                        vsi_l_offset nFileOffsetStart)
{
    CPLMutexHolder oHolder(&hMutex);

    const int knDOWNLOAD_CHUNK_SIZE = VSICURLGetDownloadChunkSize();
    nFileOffsetStart =
        (nFileOffsetStart / knDOWNLOAD_CHUNK_SIZE) * knDOWNLOAD_CHUNK_SIZE;

    std::shared_ptr<std::string> out;
    if (GetRegionCache()->tryGet(
            FilenameOffsetPair(std::string(pszURL), nFileOffsetStart), out))
    {
        return out;
    }

    return nullptr;
}

/************************************************************************/
/*                          AddRegion()                                 */
/************************************************************************/

void VSICurlFilesystemHandlerBase::AddRegion(const char *pszURL,
                                             vsi_l_offset nFileOffsetStart,
                                             size_t nSize, const char *pData)
{
    CPLMutexHolder oHolder(&hMutex);

    std::shared_ptr<std::string> value(new std::string());
    value->assign(pData, nSize);
    GetRegionCache()->insert(
        FilenameOffsetPair(std::string(pszURL), nFileOffsetStart), value);
}

/************************************************************************/
/*                         GetCachedFileProp()                          */
/************************************************************************/

bool VSICurlFilesystemHandlerBase::GetCachedFileProp(const char *pszURL,
                                                     FileProp &oFileProp)
{
    CPLMutexHolder oHolder(&hMutex);
    bool inCache;
    if (oCacheFileProp.tryGet(std::string(pszURL), inCache))
    {
        if (VSICURLGetCachedFileProp(pszURL, oFileProp))
        {
            return true;
        }
        oCacheFileProp.remove(std::string(pszURL));
    }
    return false;
}

/************************************************************************/
/*                         SetCachedFileProp()                          */
/************************************************************************/

void VSICurlFilesystemHandlerBase::SetCachedFileProp(const char *pszURL,
                                                     FileProp &oFileProp)
{
    CPLMutexHolder oHolder(&hMutex);
    oCacheFileProp.insert(std::string(pszURL), true);
    VSICURLSetCachedFileProp(pszURL, oFileProp);
}

/************************************************************************/
/*                         GetCachedDirList()                           */
/************************************************************************/

bool VSICurlFilesystemHandlerBase::GetCachedDirList(
    const char *pszURL, CachedDirList &oCachedDirList)
{
    CPLMutexHolder oHolder(&hMutex);

    return oCacheDirList.tryGet(std::string(pszURL), oCachedDirList) &&
           // Let a chance to use new auth parameters
           gnGenerationAuthParameters ==
               oCachedDirList.nGenerationAuthParameters;
}

/************************************************************************/
/*                         SetCachedDirList()                           */
/************************************************************************/

void VSICurlFilesystemHandlerBase::SetCachedDirList(
    const char *pszURL, CachedDirList &oCachedDirList)
{
    CPLMutexHolder oHolder(&hMutex);

    std::string key(pszURL);
    CachedDirList oldValue;
    if (oCacheDirList.tryGet(key, oldValue))
    {
        nCachedFilesInDirList -= oldValue.oFileList.size();
        oCacheDirList.remove(key);
    }

    while ((!oCacheDirList.empty() &&
            nCachedFilesInDirList + oCachedDirList.oFileList.size() >
                1024 * 1024) ||
           oCacheDirList.size() == oCacheDirList.getMaxAllowedSize())
    {
        std::string oldestKey;
        oCacheDirList.getOldestEntry(oldestKey, oldValue);
        nCachedFilesInDirList -= oldValue.oFileList.size();
        oCacheDirList.remove(oldestKey);
    }
    oCachedDirList.nGenerationAuthParameters = gnGenerationAuthParameters;

    nCachedFilesInDirList += oCachedDirList.oFileList.size();
    oCacheDirList.insert(key, oCachedDirList);
}

/************************************************************************/
/*                        ExistsInCacheDirList()                        */
/************************************************************************/

bool VSICurlFilesystemHandlerBase::ExistsInCacheDirList(
    const std::string &osDirname, bool *pbIsDir)
{
    CachedDirList cachedDirList;
    if (GetCachedDirList(osDirname.c_str(), cachedDirList))
    {
        if (pbIsDir)
            *pbIsDir = !cachedDirList.oFileList.empty();
        return false;
    }
    else
    {
        if (pbIsDir)
            *pbIsDir = false;
        return false;
    }
}

/************************************************************************/
/*                        InvalidateCachedData()                        */
/************************************************************************/

void VSICurlFilesystemHandlerBase::InvalidateCachedData(const char *pszURL)
{
    CPLMutexHolder oHolder(&hMutex);

    oCacheFileProp.remove(std::string(pszURL));

    // Invalidate all cached regions for this URL
    std::list<FilenameOffsetPair> keysToRemove;
    std::string osURL(pszURL);
    auto lambda =
        [&keysToRemove,
         &osURL](const lru11::KeyValuePair<FilenameOffsetPair,
                                           std::shared_ptr<std::string>> &kv)
    {
        if (kv.key.filename_ == osURL)
            keysToRemove.push_back(kv.key);
    };
    auto *poRegionCache = GetRegionCache();
    poRegionCache->cwalk(lambda);
    for (const auto &key : keysToRemove)
        poRegionCache->remove(key);
}

/************************************************************************/
/*                            ClearCache()                              */
/************************************************************************/

void VSICurlFilesystemHandlerBase::ClearCache()
{
    CPLMutexHolder oHolder(&hMutex);

    GetRegionCache()->clear();

    {
        const auto lambda = [](const lru11::KeyValuePair<std::string, bool> &kv)
        { VSICURLInvalidateCachedFileProp(kv.key.c_str()); };
        oCacheFileProp.cwalk(lambda);
        oCacheFileProp.clear();
    }

    oCacheDirList.clear();
    nCachedFilesInDirList = 0;

    GetConnectionCache()[this].clear();
}

/************************************************************************/
/*                          PartialClearCache()                         */
/************************************************************************/

void VSICurlFilesystemHandlerBase::PartialClearCache(
    const char *pszFilenamePrefix)
{
    CPLMutexHolder oHolder(&hMutex);

    std::string osURL = GetURLFromFilename(pszFilenamePrefix);
    {
        std::list<FilenameOffsetPair> keysToRemove;
        auto lambda =
            [&keysToRemove, &osURL](
                const lru11::KeyValuePair<FilenameOffsetPair,
                                          std::shared_ptr<std::string>> &kv)
        {
            if (strncmp(kv.key.filename_.c_str(), osURL.c_str(),
                        osURL.size()) == 0)
                keysToRemove.push_back(kv.key);
        };
        auto *poRegionCache = GetRegionCache();
        poRegionCache->cwalk(lambda);
        for (const auto &key : keysToRemove)
            poRegionCache->remove(key);
    }

    {
        std::list<std::string> keysToRemove;
        auto lambda = [&keysToRemove,
                       &osURL](const lru11::KeyValuePair<std::string, bool> &kv)
        {
            if (strncmp(kv.key.c_str(), osURL.c_str(), osURL.size()) == 0)
                keysToRemove.push_back(kv.key);
        };
        oCacheFileProp.cwalk(lambda);
        for (const auto &key : keysToRemove)
            oCacheFileProp.remove(key);
    }
    VSICURLInvalidateCachedFilePropPrefix(osURL.c_str());

    {
        const size_t nLen = strlen(pszFilenamePrefix);
        std::list<std::string> keysToRemove;
        auto lambda =
            [this, &keysToRemove, pszFilenamePrefix,
             nLen](const lru11::KeyValuePair<std::string, CachedDirList> &kv)
        {
            if (strncmp(kv.key.c_str(), pszFilenamePrefix, nLen) == 0)
            {
                keysToRemove.push_back(kv.key);
                nCachedFilesInDirList -= kv.value.oFileList.size();
            }
        };
        oCacheDirList.cwalk(lambda);
        for (const auto &key : keysToRemove)
            oCacheDirList.remove(key);
    }
}

/************************************************************************/
/*                          CreateFileHandle()                          */
/************************************************************************/

VSICurlHandle *
VSICurlFilesystemHandlerBase::CreateFileHandle(const char *pszFilename)
{
    return new VSICurlHandle(this, pszFilename);
}

/************************************************************************/
/*                          GetActualURL()                              */
/************************************************************************/

const char *VSICurlFilesystemHandlerBase::GetActualURL(const char *pszFilename)
{
    VSICurlHandle *poHandle = CreateFileHandle(pszFilename);
    if (poHandle == nullptr)
        return pszFilename;
    std::string osURL(poHandle->GetURL());
    delete poHandle;
    return CPLSPrintf("%s", osURL.c_str());
}

/************************************************************************/
/*                           GetOptions()                               */
/************************************************************************/

#define VSICURL_OPTIONS                                                        \
    "  <Option name='GDAL_HTTP_MAX_RETRY' type='int' "                         \
    "description='Maximum number of retries' default='0'/>"                    \
    "  <Option name='GDAL_HTTP_RETRY_DELAY' type='double' "                    \
    "description='Retry delay in seconds' default='30'/>"                      \
    "  <Option name='GDAL_HTTP_HEADER_FILE' type='string' "                    \
    "description='Filename of a file that contains HTTP headers to "           \
    "forward to the server'/>"                                                 \
    "  <Option name='CPL_VSIL_CURL_USE_HEAD' type='boolean' "                  \
    "description='Whether to use HTTP HEAD verb to retrieve "                  \
    "file information' default='YES'/>"                                        \
    "  <Option name='GDAL_HTTP_MULTIRANGE' type='string-select' "              \
    "description='Strategy to apply to run multi-range requests' "             \
    "default='PARALLEL'>"                                                      \
    "       <Value>PARALLEL</Value>"                                           \
    "       <Value>SERIAL</Value>"                                             \
    "  </Option>"                                                              \
    "  <Option name='GDAL_HTTP_MULTIPLEX' type='boolean' "                     \
    "description='Whether to enable HTTP/2 multiplexing' default='YES'/>"      \
    "  <Option name='GDAL_HTTP_MERGE_CONSECUTIVE_RANGES' type='boolean' "      \
    "description='Whether to merge consecutive ranges in multirange "          \
    "requests' default='YES'/>"                                                \
    "  <Option name='CPL_VSIL_CURL_NON_CACHED' type='string' "                 \
    "description='Colon-separated list of filenames whose content"             \
    "must not be cached across open attempts'/>"                               \
    "  <Option name='CPL_VSIL_CURL_ALLOWED_FILENAME' type='string' "           \
    "description='Single filename that is allowed to be opened'/>"             \
    "  <Option name='CPL_VSIL_CURL_ALLOWED_EXTENSIONS' type='string' "         \
    "description='Comma or space separated list of allowed file "              \
    "extensions'/>"                                                            \
    "  <Option name='GDAL_DISABLE_READDIR_ON_OPEN' type='string-select' "      \
    "description='Whether to disable establishing the list of files in "       \
    "the directory of the current filename' default='NO'>"                     \
    "       <Value>NO</Value>"                                                 \
    "       <Value>YES</Value>"                                                \
    "       <Value>EMPTY_DIR</Value>"                                          \
    "  </Option>"                                                              \
    "  <Option name='VSI_CACHE' type='boolean' "                               \
    "description='Whether to cache in memory the contents of the opened "      \
    "file as soon as they are read' default='NO'/>"                            \
    "  <Option name='CPL_VSIL_CURL_CHUNK_SIZE' type='integer' "                \
    "description='Size in bytes of the minimum amount of data read in a "      \
    "file' default='16384' min='1024' max='10485760'/>"                        \
    "  <Option name='CPL_VSIL_CURL_CACHE_SIZE' type='integer' "                \
    "description='Size in bytes of the global /vsicurl/ cache' "               \
    "default='16384000'/>"                                                     \
    "  <Option name='CPL_VSIL_CURL_IGNORE_GLACIER_STORAGE' type='boolean' "    \
    "description='Whether to skip files with Glacier storage class in "        \
    "directory listing.' default='YES'/>"                                      \
    "  <Option name='CPL_VSIL_CURL_ADVISE_READ_TOTAL_BYTES_LIMIT' "            \
    "type='integer' description='Maximum number of bytes AdviseRead() is "     \
    "allowed to fetch at once' default='104857600'/>"                          \
    "  <Option name='GDAL_HTTP_MAX_CACHED_CONNECTIONS' type='integer' "        \
    "description='Maximum amount of connections that libcurl may keep alive "  \
    "in its connection cache after use'/>"                                     \
    "  <Option name='GDAL_HTTP_MAX_TOTAL_CONNECTIONS' type='integer' "         \
    "description='Maximum number of simultaneously open connections in "       \
    "total'/>"

const char *VSICurlFilesystemHandlerBase::GetOptionsStatic()
{
    return VSICURL_OPTIONS;
}

const char *VSICurlFilesystemHandlerBase::GetOptions()
{
    static std::string osOptions(std::string("<Options>") + GetOptionsStatic() +
                                 "</Options>");
    return osOptions.c_str();
}

/************************************************************************/
/*                        IsAllowedFilename()                           */
/************************************************************************/

bool VSICurlFilesystemHandlerBase::IsAllowedFilename(const char *pszFilename)
{
    const char *pszAllowedFilename =
        CPLGetConfigOption("CPL_VSIL_CURL_ALLOWED_FILENAME", nullptr);
    if (pszAllowedFilename != nullptr)
    {
        return strcmp(pszFilename, pszAllowedFilename) == 0;
    }

    // Consider that only the files whose extension ends up with one that is
    // listed in CPL_VSIL_CURL_ALLOWED_EXTENSIONS exist on the server.  This can
    // speeds up dramatically open experience, in case the server cannot return
    // a file list.  {noext} can be used as a special token to mean file with no
    // extension.
    // For example:
    // gdalinfo --config CPL_VSIL_CURL_ALLOWED_EXTENSIONS ".tif"
    // /vsicurl/http://igskmncngs506.cr.usgs.gov/gmted/Global_tiles_GMTED/075darcsec/bln/W030/30N030W_20101117_gmted_bln075.tif
    const char *pszAllowedExtensions =
        CPLGetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", nullptr);
    if (pszAllowedExtensions)
    {
        char **papszExtensions =
            CSLTokenizeString2(pszAllowedExtensions, ", ", 0);
        const char *queryStart = strchr(pszFilename, '?');
        char *pszFilenameWithoutQuery = nullptr;
        if (queryStart != nullptr)
        {
            pszFilenameWithoutQuery = CPLStrdup(pszFilename);
            pszFilenameWithoutQuery[queryStart - pszFilename] = '\0';
            pszFilename = pszFilenameWithoutQuery;
        }
        const size_t nURLLen = strlen(pszFilename);
        bool bFound = false;
        for (int i = 0; papszExtensions[i] != nullptr; i++)
        {
            const size_t nExtensionLen = strlen(papszExtensions[i]);
            if (EQUAL(papszExtensions[i], "{noext}"))
            {
                const char *pszLastSlash = strrchr(pszFilename, '/');
                if (pszLastSlash != nullptr &&
                    strchr(pszLastSlash, '.') == nullptr)
                {
                    bFound = true;
                    break;
                }
            }
            else if (nURLLen > nExtensionLen &&
                     EQUAL(pszFilename + nURLLen - nExtensionLen,
                           papszExtensions[i]))
            {
                bFound = true;
                break;
            }
        }

        CSLDestroy(papszExtensions);
        if (pszFilenameWithoutQuery)
        {
            CPLFree(pszFilenameWithoutQuery);
        }

        return bFound;
    }
    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

VSIVirtualHandle *VSICurlFilesystemHandlerBase::Open(const char *pszFilename,
                                                     const char *pszAccess,
                                                     bool bSetError,
                                                     CSLConstList papszOptions)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()) &&
        !STARTS_WITH_CI(pszFilename, "/vsicurl?"))
        return nullptr;

    if (strchr(pszAccess, 'w') != nullptr || strchr(pszAccess, '+') != nullptr)
    {
        if (bSetError)
        {
            VSIError(VSIE_FileError,
                     "Only read-only mode is supported for /vsicurl");
        }
        return nullptr;
    }
    if (!papszOptions ||
        !CPLTestBool(CSLFetchNameValueDef(
            papszOptions, "IGNORE_FILENAME_RESTRICTIONS", "NO")))
    {
        if (!IsAllowedFilename(pszFilename))
            return nullptr;
    }

    bool bListDir = true;
    bool bEmptyDir = false;
    CPL_IGNORE_RET_VAL(VSICurlGetURLFromFilename(pszFilename, nullptr, nullptr,
                                                 nullptr, &bListDir, &bEmptyDir,
                                                 nullptr, nullptr, nullptr));

    const char *pszOptionVal = CSLFetchNameValueDef(
        papszOptions, "DISABLE_READDIR_ON_OPEN",
        VSIGetPathSpecificOption(pszFilename, "GDAL_DISABLE_READDIR_ON_OPEN",
                                 "NO"));
    const bool bSkipReadDir =
        !bListDir || bEmptyDir || EQUAL(pszOptionVal, "EMPTY_DIR") ||
        CPLTestBool(pszOptionVal) || !AllowCachedDataFor(pszFilename);

    std::string osFilename(pszFilename);
    bool bGotFileList = !bSkipReadDir;
    bool bForceExistsCheck = false;
    FileProp cachedFileProp;
    if (!(GetCachedFileProp(osFilename.c_str() + strlen(GetFSPrefix().c_str()),
                            cachedFileProp) &&
          cachedFileProp.eExists == EXIST_YES) &&
        strchr(CPLGetFilename(osFilename.c_str()), '.') != nullptr &&
        !STARTS_WITH(CPLGetExtensionSafe(osFilename.c_str()).c_str(), "zip") &&
        !bSkipReadDir)
    {
        char **papszFileList = ReadDirInternal(
            (CPLGetDirnameSafe(osFilename.c_str()) + '/').c_str(), 0,
            &bGotFileList);
        const bool bFound =
            VSICurlIsFileInList(papszFileList,
                                CPLGetFilename(osFilename.c_str())) != -1;
        if (bGotFileList && !bFound)
        {
            // Some file servers are case insensitive, so in case there is a
            // match with case difference, do a full check just in case.
            // e.g.
            // http://pds-geosciences.wustl.edu/mgs/mgs-m-mola-5-megdr-l3-v1/mgsl_300x/meg004/MEGA90N000CB.IMG
            // that is queried by
            // gdalinfo
            // /vsicurl/http://pds-geosciences.wustl.edu/mgs/mgs-m-mola-5-megdr-l3-v1/mgsl_300x/meg004/mega90n000cb.lbl
            if (CSLFindString(papszFileList,
                              CPLGetFilename(osFilename.c_str())) != -1)
            {
                bForceExistsCheck = true;
            }
            else
            {
                CSLDestroy(papszFileList);
                return nullptr;
            }
        }
        CSLDestroy(papszFileList);
    }

    VSICurlHandle *poHandle = CreateFileHandle(osFilename.c_str());
    if (poHandle == nullptr)
        return nullptr;
    if (!bGotFileList || bForceExistsCheck)
    {
        // If we didn't get a filelist, check that the file really exists.
        if (!poHandle->Exists(bSetError))
        {
            delete poHandle;
            return nullptr;
        }
    }

    if (CPLTestBool(CPLGetConfigOption("VSI_CACHE", "FALSE")))
        return VSICreateCachedFile(poHandle);
    else
        return poHandle;
}

/************************************************************************/
/*                        VSICurlParserFindEOL()                        */
/*                                                                      */
/*      Small helper function for VSICurlPaseHTMLFileList() to find     */
/*      the end of a line in the directory listing.  Either a <br>      */
/*      or newline.                                                     */
/************************************************************************/

static char *VSICurlParserFindEOL(char *pszData)

{
    while (*pszData != '\0' && *pszData != '\n' &&
           !STARTS_WITH_CI(pszData, "<br>"))
        pszData++;

    if (*pszData == '\0')
        return nullptr;

    return pszData;
}

/************************************************************************/
/*                   VSICurlParseHTMLDateTimeFileSize()                 */
/************************************************************************/

static const char *const apszMonths[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

static bool VSICurlParseHTMLDateTimeFileSize(const char *pszStr,
                                             struct tm &brokendowntime,
                                             GUIntBig &nFileSize,
                                             GIntBig &mTime)
{
    for (int iMonth = 0; iMonth < 12; iMonth++)
    {
        char szMonth[32] = {};
        szMonth[0] = '-';
        memcpy(szMonth + 1, apszMonths[iMonth], 3);
        szMonth[4] = '-';
        szMonth[5] = '\0';
        const char *pszMonthFound = strstr(pszStr, szMonth);
        if (pszMonthFound)
        {
            // Format of Apache, like in
            // http://download.osgeo.org/gdal/data/gtiff/
            // "17-May-2010 12:26"
            if (pszMonthFound - pszStr > 2 && strlen(pszMonthFound) > 15 &&
                pszMonthFound[-2 + 11] == ' ' && pszMonthFound[-2 + 14] == ':')
            {
                pszMonthFound -= 2;
                int nDay = atoi(pszMonthFound);
                int nYear = atoi(pszMonthFound + 7);
                int nHour = atoi(pszMonthFound + 12);
                int nMin = atoi(pszMonthFound + 15);
                if (nDay >= 1 && nDay <= 31 && nYear >= 1900 && nHour >= 0 &&
                    nHour <= 24 && nMin >= 0 && nMin < 60)
                {
                    brokendowntime.tm_year = nYear - 1900;
                    brokendowntime.tm_mon = iMonth;
                    brokendowntime.tm_mday = nDay;
                    brokendowntime.tm_hour = nHour;
                    brokendowntime.tm_min = nMin;
                    mTime = CPLYMDHMSToUnixTime(&brokendowntime);

                    return true;
                }
            }
            return false;
        }

        /* Microsoft IIS */
        snprintf(szMonth, sizeof(szMonth), " %s ", apszMonths[iMonth]);
        pszMonthFound = strstr(pszStr, szMonth);
        if (pszMonthFound)
        {
            int nLenMonth = static_cast<int>(strlen(apszMonths[iMonth]));
            if (pszMonthFound - pszStr > 2 && pszMonthFound[-1] != ',' &&
                pszMonthFound[-2] != ' ' &&
                static_cast<int>(strlen(pszMonthFound - 2)) >
                    2 + 1 + nLenMonth + 1 + 4 + 1 + 5 + 1 + 4)
            {
                /* Format of http://ortho.linz.govt.nz/tifs/1994_95/ */
                /* "        Friday, 21 April 2006 12:05 p.m.     48062343
                 * m35a_fy_94_95.tif" */
                pszMonthFound -= 2;
                int nDay = atoi(pszMonthFound);
                int nCurOffset = 2 + 1 + nLenMonth + 1;
                int nYear = atoi(pszMonthFound + nCurOffset);
                nCurOffset += 4 + 1;
                int nHour = atoi(pszMonthFound + nCurOffset);
                if (nHour < 10)
                    nCurOffset += 1 + 1;
                else
                    nCurOffset += 2 + 1;
                const int nMin = atoi(pszMonthFound + nCurOffset);
                nCurOffset += 2 + 1;
                if (STARTS_WITH(pszMonthFound + nCurOffset, "p.m."))
                    nHour += 12;
                else if (!STARTS_WITH(pszMonthFound + nCurOffset, "a.m."))
                    nHour = -1;
                nCurOffset += 4;

                const char *pszFilesize = pszMonthFound + nCurOffset;
                while (*pszFilesize == ' ')
                    pszFilesize++;
                if (*pszFilesize >= '1' && *pszFilesize <= '9')
                    nFileSize = CPLScanUIntBig(
                        pszFilesize, static_cast<int>(strlen(pszFilesize)));

                if (nDay >= 1 && nDay <= 31 && nYear >= 1900 && nHour >= 0 &&
                    nHour <= 24 && nMin >= 0 && nMin < 60)
                {
                    brokendowntime.tm_year = nYear - 1900;
                    brokendowntime.tm_mon = iMonth;
                    brokendowntime.tm_mday = nDay;
                    brokendowntime.tm_hour = nHour;
                    brokendowntime.tm_min = nMin;
                    mTime = CPLYMDHMSToUnixTime(&brokendowntime);

                    return true;
                }
                nFileSize = 0;
            }
            else if (pszMonthFound - pszStr > 1 && pszMonthFound[-1] == ',' &&
                     static_cast<int>(strlen(pszMonthFound)) >
                         1 + nLenMonth + 1 + 2 + 1 + 1 + 4 + 1 + 5 + 1 + 2)
            {
                // Format of
                // http://publicfiles.dep.state.fl.us/dear/BWR_GIS/2007NWFLULC/
                // "        Sunday, June 20, 2010  6:46 PM    233170905
                // NWF2007LULCForSDE.zip"
                pszMonthFound += 1;
                int nCurOffset = nLenMonth + 1;
                int nDay = atoi(pszMonthFound + nCurOffset);
                nCurOffset += 2 + 1 + 1;
                int nYear = atoi(pszMonthFound + nCurOffset);
                nCurOffset += 4 + 1;
                int nHour = atoi(pszMonthFound + nCurOffset);
                nCurOffset += 2 + 1;
                const int nMin = atoi(pszMonthFound + nCurOffset);
                nCurOffset += 2 + 1;
                if (STARTS_WITH(pszMonthFound + nCurOffset, "PM"))
                    nHour += 12;
                else if (!STARTS_WITH(pszMonthFound + nCurOffset, "AM"))
                    nHour = -1;
                nCurOffset += 2;

                const char *pszFilesize = pszMonthFound + nCurOffset;
                while (*pszFilesize == ' ')
                    pszFilesize++;
                if (*pszFilesize >= '1' && *pszFilesize <= '9')
                    nFileSize = CPLScanUIntBig(
                        pszFilesize, static_cast<int>(strlen(pszFilesize)));

                if (nDay >= 1 && nDay <= 31 && nYear >= 1900 && nHour >= 0 &&
                    nHour <= 24 && nMin >= 0 && nMin < 60)
                {
                    brokendowntime.tm_year = nYear - 1900;
                    brokendowntime.tm_mon = iMonth;
                    brokendowntime.tm_mday = nDay;
                    brokendowntime.tm_hour = nHour;
                    brokendowntime.tm_min = nMin;
                    mTime = CPLYMDHMSToUnixTime(&brokendowntime);

                    return true;
                }
                nFileSize = 0;
            }
            return false;
        }
    }

    return false;
}

/************************************************************************/
/*                          ParseHTMLFileList()                         */
/*                                                                      */
/*      Parse a file list document and return all the components.       */
/************************************************************************/

char **VSICurlFilesystemHandlerBase::ParseHTMLFileList(const char *pszFilename,
                                                       int nMaxFiles,
                                                       char *pszData,
                                                       bool *pbGotFileList)
{
    *pbGotFileList = false;

    std::string osURL(VSICurlGetURLFromFilename(pszFilename, nullptr, nullptr,
                                                nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr));
    const char *pszDir = nullptr;
    if (STARTS_WITH_CI(osURL.c_str(), "http://"))
        pszDir = strchr(osURL.c_str() + strlen("http://"), '/');
    else if (STARTS_WITH_CI(osURL.c_str(), "https://"))
        pszDir = strchr(osURL.c_str() + strlen("https://"), '/');
    else if (STARTS_WITH_CI(osURL.c_str(), "ftp://"))
        pszDir = strchr(osURL.c_str() + strlen("ftp://"), '/');
    if (pszDir == nullptr)
        pszDir = "";

    /* Apache */
    std::string osExpectedString = "<title>Index of ";
    osExpectedString += pszDir;
    osExpectedString += "</title>";
    /* shttpd */
    std::string osExpectedString2 = "<title>Index of ";
    osExpectedString2 += pszDir;
    osExpectedString2 += "/</title>";
    /* FTP */
    std::string osExpectedString3 = "FTP Listing of ";
    osExpectedString3 += pszDir;
    osExpectedString3 += "/";
    /* Apache 1.3.33 */
    std::string osExpectedString4 = "<TITLE>Index of ";
    osExpectedString4 += pszDir;
    osExpectedString4 += "</TITLE>";

    // The listing of
    // http://dds.cr.usgs.gov/srtm/SRTM_image_sample/picture%20examples/
    // has
    // "<title>Index of /srtm/SRTM_image_sample/picture examples</title>"
    // so we must try unescaped %20 also.
    // Similar with
    // http://datalib.usask.ca/gis/Data/Central_America_goodbutdoweown%3f/
    std::string osExpectedString_unescaped;
    if (strchr(pszDir, '%'))
    {
        char *pszUnescapedDir = CPLUnescapeString(pszDir, nullptr, CPLES_URL);
        osExpectedString_unescaped = "<title>Index of ";
        osExpectedString_unescaped += pszUnescapedDir;
        osExpectedString_unescaped += "</title>";
        CPLFree(pszUnescapedDir);
    }

    char *c = nullptr;
    int nCount = 0;
    int nCountTable = 0;
    CPLStringList oFileList;
    char *pszLine = pszData;
    bool bIsHTMLDirList = false;

    while ((c = VSICurlParserFindEOL(pszLine)) != nullptr)
    {
        *c = '\0';

        // To avoid false positive on pages such as
        // http://www.ngs.noaa.gov/PC_PROD/USGG2009BETA
        // This is a heuristics, but normal HTML listing of files have not more
        // than one table.
        if (strstr(pszLine, "<table"))
        {
            nCountTable++;
            if (nCountTable == 2)
            {
                *pbGotFileList = false;
                return nullptr;
            }
        }

        if (!bIsHTMLDirList &&
            (strstr(pszLine, osExpectedString.c_str()) ||
             strstr(pszLine, osExpectedString2.c_str()) ||
             strstr(pszLine, osExpectedString3.c_str()) ||
             strstr(pszLine, osExpectedString4.c_str()) ||
             (!osExpectedString_unescaped.empty() &&
              strstr(pszLine, osExpectedString_unescaped.c_str()))))
        {
            bIsHTMLDirList = true;
            *pbGotFileList = true;
        }
        // Subversion HTTP listing
        // or Microsoft-IIS/6.0 listing
        // (e.g. http://ortho.linz.govt.nz/tifs/2005_06/) */
        else if (!bIsHTMLDirList && strstr(pszLine, "<title>"))
        {
            // Detect something like:
            // <html><head><title>gdal - Revision 20739:
            // /trunk/autotest/gcore/data</title></head> */ The annoying thing
            // is that what is after ': ' is a subpart of what is after
            // http://server/
            char *pszSubDir = strstr(pszLine, ": ");
            if (pszSubDir == nullptr)
                // or <title>ortho.linz.govt.nz - /tifs/2005_06/</title>
                pszSubDir = strstr(pszLine, "- ");
            if (pszSubDir)
            {
                pszSubDir += 2;
                char *pszTmp = strstr(pszSubDir, "</title>");
                if (pszTmp)
                {
                    if (pszTmp[-1] == '/')
                        pszTmp[-1] = 0;
                    else
                        *pszTmp = 0;
                    if (strstr(pszDir, pszSubDir))
                    {
                        bIsHTMLDirList = true;
                        *pbGotFileList = true;
                    }
                }
            }
        }
        else if (bIsHTMLDirList &&
                 (strstr(pszLine, "<a href=\"") != nullptr ||
                  strstr(pszLine, "<A HREF=\"") != nullptr) &&
                 // Exclude absolute links, like to subversion home.
                 strstr(pszLine, "<a href=\"http://") == nullptr &&
                 // exclude parent directory.
                 strstr(pszLine, "Parent Directory") == nullptr)
        {
            char *beginFilename = strstr(pszLine, "<a href=\"");
            if (beginFilename == nullptr)
                beginFilename = strstr(pszLine, "<A HREF=\"");
            beginFilename += strlen("<a href=\"");
            char *endQuote = strchr(beginFilename, '"');
            if (endQuote && !STARTS_WITH(beginFilename, "?C=") &&
                !STARTS_WITH(beginFilename, "?N="))
            {
                struct tm brokendowntime;
                memset(&brokendowntime, 0, sizeof(brokendowntime));
                GUIntBig nFileSize = 0;
                GIntBig mTime = 0;

                VSICurlParseHTMLDateTimeFileSize(pszLine, brokendowntime,
                                                 nFileSize, mTime);

                *endQuote = '\0';

                // Remove trailing slash, that are returned for directories by
                // Apache.
                bool bIsDirectory = false;
                if (endQuote[-1] == '/')
                {
                    bIsDirectory = true;
                    endQuote[-1] = 0;
                }

                // shttpd links include slashes from the root directory.
                // Skip them.
                while (strchr(beginFilename, '/'))
                    beginFilename = strchr(beginFilename, '/') + 1;

                if (strcmp(beginFilename, ".") != 0 &&
                    strcmp(beginFilename, "..") != 0)
                {
                    std::string osCachedFilename =
                        CPLSPrintf("%s/%s", osURL.c_str(), beginFilename);

                    FileProp cachedFileProp;
                    GetCachedFileProp(osCachedFilename.c_str(), cachedFileProp);
                    cachedFileProp.eExists = EXIST_YES;
                    cachedFileProp.bIsDirectory = bIsDirectory;
                    cachedFileProp.mTime = static_cast<time_t>(mTime);
                    cachedFileProp.bHasComputedFileSize = nFileSize > 0;
                    cachedFileProp.fileSize = nFileSize;
                    SetCachedFileProp(osCachedFilename.c_str(), cachedFileProp);

                    oFileList.AddString(beginFilename);
                    if (ENABLE_DEBUG_VERBOSE)
                    {
                        CPLDebug(
                            GetDebugKey(),
                            "File[%d] = %s, is_dir = %d, size = " CPL_FRMT_GUIB
                            ", time = %04d/%02d/%02d %02d:%02d:%02d",
                            nCount, osCachedFilename.c_str(),
                            bIsDirectory ? 1 : 0, nFileSize,
                            brokendowntime.tm_year + 1900,
                            brokendowntime.tm_mon + 1, brokendowntime.tm_mday,
                            brokendowntime.tm_hour, brokendowntime.tm_min,
                            brokendowntime.tm_sec);
                    }
                    nCount++;

                    if (nMaxFiles > 0 && oFileList.Count() > nMaxFiles)
                        break;
                }
            }
        }
        pszLine = c + 1;
    }

    return oFileList.StealList();
}

/************************************************************************/
/*                      GetStreamingFilename()                          */
/************************************************************************/

std::string VSICurlFilesystemHandler::GetStreamingFilename(
    const std::string &osFilename) const
{
    if (STARTS_WITH(osFilename.c_str(), GetFSPrefix().c_str()))
        return "/vsicurl_streaming/" + osFilename.substr(GetFSPrefix().size());
    return osFilename;
}

/************************************************************************/
/*                         VSICurlGetToken()                            */
/************************************************************************/

static char *VSICurlGetToken(char *pszCurPtr, char **ppszNextToken)
{
    if (pszCurPtr == nullptr)
        return nullptr;

    while ((*pszCurPtr) == ' ')
        pszCurPtr++;
    if (*pszCurPtr == '\0')
        return nullptr;

    char *pszToken = pszCurPtr;
    while ((*pszCurPtr) != ' ' && (*pszCurPtr) != '\0')
        pszCurPtr++;
    if (*pszCurPtr == '\0')
    {
        *ppszNextToken = nullptr;
    }
    else
    {
        *pszCurPtr = '\0';
        pszCurPtr++;
        while ((*pszCurPtr) == ' ')
            pszCurPtr++;
        *ppszNextToken = pszCurPtr;
    }

    return pszToken;
}

/************************************************************************/
/*                    VSICurlParseFullFTPLine()                         */
/************************************************************************/

/* Parse lines like the following ones :
-rw-r--r--    1 10003    100           430 Jul 04  2008 COPYING
lrwxrwxrwx    1 ftp      ftp            28 Jun 14 14:13 MPlayer ->
mirrors/mplayerhq.hu/MPlayer -rw-r--r--    1 ftp      ftp      725614592 May 13
20:13 Fedora-15-x86_64-Live-KDE.iso drwxr-xr-x  280 1003  1003  6656 Aug 26
04:17 gnu
*/

static bool VSICurlParseFullFTPLine(char *pszLine, char *&pszFilename,
                                    bool &bSizeValid, GUIntBig &nSize,
                                    bool &bIsDirectory, GIntBig &nUnixTime)
{
    char *pszNextToken = pszLine;
    char *pszPermissions = VSICurlGetToken(pszNextToken, &pszNextToken);
    if (pszPermissions == nullptr || strlen(pszPermissions) != 10)
        return false;
    bIsDirectory = pszPermissions[0] == 'd';

    for (int i = 0; i < 3; i++)
    {
        if (VSICurlGetToken(pszNextToken, &pszNextToken) == nullptr)
            return false;
    }

    char *pszSize = VSICurlGetToken(pszNextToken, &pszNextToken);
    if (pszSize == nullptr)
        return false;

    if (pszPermissions[0] == '-')
    {
        // Regular file.
        bSizeValid = true;
        nSize = CPLScanUIntBig(pszSize, static_cast<int>(strlen(pszSize)));
    }

    struct tm brokendowntime;
    memset(&brokendowntime, 0, sizeof(brokendowntime));
    bool bBrokenDownTimeValid = true;

    char *pszMonth = VSICurlGetToken(pszNextToken, &pszNextToken);
    if (pszMonth == nullptr || strlen(pszMonth) != 3)
        return false;

    int i = 0;  // Used after for.
    for (; i < 12; i++)
    {
        if (EQUALN(pszMonth, apszMonths[i], 3))
            break;
    }
    if (i < 12)
        brokendowntime.tm_mon = i;
    else
        bBrokenDownTimeValid = false;

    char *pszDay = VSICurlGetToken(pszNextToken, &pszNextToken);
    if (pszDay == nullptr || (strlen(pszDay) != 1 && strlen(pszDay) != 2))
        return false;
    int nDay = atoi(pszDay);
    if (nDay >= 1 && nDay <= 31)
        brokendowntime.tm_mday = nDay;
    else
        bBrokenDownTimeValid = false;

    char *pszHourOrYear = VSICurlGetToken(pszNextToken, &pszNextToken);
    if (pszHourOrYear == nullptr ||
        (strlen(pszHourOrYear) != 4 && strlen(pszHourOrYear) != 5))
        return false;
    if (strlen(pszHourOrYear) == 4)
    {
        brokendowntime.tm_year = atoi(pszHourOrYear) - 1900;
    }
    else
    {
        time_t sTime;
        time(&sTime);
        struct tm currentBrokendowntime;
        CPLUnixTimeToYMDHMS(static_cast<GIntBig>(sTime),
                            &currentBrokendowntime);
        brokendowntime.tm_year = currentBrokendowntime.tm_year;
        brokendowntime.tm_hour = atoi(pszHourOrYear);
        brokendowntime.tm_min = atoi(pszHourOrYear + 3);
    }

    if (bBrokenDownTimeValid)
        nUnixTime = CPLYMDHMSToUnixTime(&brokendowntime);
    else
        nUnixTime = 0;

    if (pszNextToken == nullptr)
        return false;

    pszFilename = pszNextToken;

    char *pszCurPtr = pszFilename;
    while (*pszCurPtr != '\0')
    {
        // In case of a link, stop before the pointed part of the link.
        if (pszPermissions[0] == 'l' && STARTS_WITH(pszCurPtr, " -> "))
        {
            break;
        }
        pszCurPtr++;
    }
    *pszCurPtr = '\0';

    return true;
}

/************************************************************************/
/*                          GetURLFromFilename()                         */
/************************************************************************/

std::string VSICurlFilesystemHandlerBase::GetURLFromFilename(
    const std::string &osFilename) const
{
    return VSICurlGetURLFromFilename(osFilename.c_str(), nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr);
}

/************************************************************************/
/*                         RegisterEmptyDir()                           */
/************************************************************************/

void VSICurlFilesystemHandlerBase::RegisterEmptyDir(
    const std::string &osDirname)
{
    CachedDirList cachedDirList;
    cachedDirList.bGotFileList = true;
    cachedDirList.oFileList.AddString(".");
    SetCachedDirList(osDirname.c_str(), cachedDirList);
}

/************************************************************************/
/*                          GetFileList()                               */
/************************************************************************/

char **VSICurlFilesystemHandlerBase::GetFileList(const char *pszDirname,
                                                 int nMaxFiles,
                                                 bool *pbGotFileList)
{
    if (ENABLE_DEBUG)
        CPLDebug(GetDebugKey(), "GetFileList(%s)", pszDirname);

    *pbGotFileList = false;

    bool bListDir = true;
    bool bEmptyDir = false;
    std::string osURL(VSICurlGetURLFromFilename(pszDirname, nullptr, nullptr,
                                                nullptr, &bListDir, &bEmptyDir,
                                                nullptr, nullptr, nullptr));
    if (bEmptyDir)
    {
        *pbGotFileList = true;
        return CSLAddString(nullptr, ".");
    }
    if (!bListDir)
        return nullptr;

    // Deal with publicly visible Azure directories.
    if (STARTS_WITH(osURL.c_str(), "https://"))
    {
        const char *pszBlobCore =
            strstr(osURL.c_str(), ".blob.core.windows.net/");
        if (pszBlobCore)
        {
            FileProp cachedFileProp;
            GetCachedFileProp(osURL.c_str(), cachedFileProp);
            if (cachedFileProp.bIsAzureFolder)
            {
                const char *pszURLWithoutHTTPS =
                    osURL.c_str() + strlen("https://");
                const std::string osStorageAccount(
                    pszURLWithoutHTTPS, pszBlobCore - pszURLWithoutHTTPS);
                CPLConfigOptionSetter oSetter1("AZURE_NO_SIGN_REQUEST", "YES",
                                               false);
                CPLConfigOptionSetter oSetter2("AZURE_STORAGE_ACCOUNT",
                                               osStorageAccount.c_str(), false);
                const std::string osVSIAZ(std::string("/vsiaz/").append(
                    pszBlobCore + strlen(".blob.core.windows.net/")));
                char **papszFileList = VSIReadDirEx(osVSIAZ.c_str(), nMaxFiles);
                if (papszFileList)
                {
                    *pbGotFileList = true;
                    return papszFileList;
                }
            }
        }
    }

    // HACK (optimization in fact) for MBTiles driver.
    if (strstr(pszDirname, ".tiles.mapbox.com") != nullptr)
        return nullptr;

    if (STARTS_WITH(osURL.c_str(), "ftp://"))
    {
        WriteFuncStruct sWriteFuncData;
        sWriteFuncData.pBuffer = nullptr;

        std::string osDirname(osURL);
        osDirname += '/';

        char **papszFileList = nullptr;

        CURLM *hCurlMultiHandle = GetCurlMultiHandleFor(osDirname);
        CURL *hCurlHandle = curl_easy_init();

        for (int iTry = 0; iTry < 2; iTry++)
        {
            struct curl_slist *headers =
                VSICurlSetOptions(hCurlHandle, osDirname.c_str(), nullptr);

            // On the first pass, we want to try fetching all the possible
            // information (filename, file/directory, size). If that does not
            // work, then try again with CURLOPT_DIRLISTONLY set.
            if (iTry == 1)
            {
                unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_DIRLISTONLY, 1);
            }

            VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr,
                                       nullptr);
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA,
                                       &sWriteFuncData);
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                                       VSICurlHandleWriteFunc);

            char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER,
                                       szCurlErrBuf);

            unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER,
                                       headers);

            VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

            curl_slist_free_all(headers);

            if (sWriteFuncData.pBuffer == nullptr)
            {
                curl_easy_cleanup(hCurlHandle);
                return nullptr;
            }

            char *pszLine = sWriteFuncData.pBuffer;
            char *c = nullptr;
            int nCount = 0;

            if (STARTS_WITH_CI(pszLine, "<!DOCTYPE HTML") ||
                STARTS_WITH_CI(pszLine, "<HTML>"))
            {
                papszFileList =
                    ParseHTMLFileList(pszDirname, nMaxFiles,
                                      sWriteFuncData.pBuffer, pbGotFileList);
                break;
            }
            else if (iTry == 0)
            {
                CPLStringList oFileList;
                *pbGotFileList = true;

                while ((c = strchr(pszLine, '\n')) != nullptr)
                {
                    *c = 0;
                    if (c - pszLine > 0 && c[-1] == '\r')
                        c[-1] = 0;

                    char *pszFilename = nullptr;
                    bool bSizeValid = false;
                    GUIntBig nFileSize = 0;
                    bool bIsDirectory = false;
                    GIntBig mUnixTime = 0;
                    if (!VSICurlParseFullFTPLine(pszLine, pszFilename,
                                                 bSizeValid, nFileSize,
                                                 bIsDirectory, mUnixTime))
                        break;

                    if (strcmp(pszFilename, ".") != 0 &&
                        strcmp(pszFilename, "..") != 0)
                    {
                        std::string osCachedFilename =
                            CPLSPrintf("%s/%s", osURL.c_str(), pszFilename);

                        FileProp cachedFileProp;
                        GetCachedFileProp(osCachedFilename.c_str(),
                                          cachedFileProp);
                        cachedFileProp.eExists = EXIST_YES;
                        cachedFileProp.bIsDirectory = bIsDirectory;
                        cachedFileProp.mTime = static_cast<time_t>(mUnixTime);
                        cachedFileProp.bHasComputedFileSize = bSizeValid;
                        cachedFileProp.fileSize = nFileSize;
                        SetCachedFileProp(osCachedFilename.c_str(),
                                          cachedFileProp);

                        oFileList.AddString(pszFilename);
                        if (ENABLE_DEBUG_VERBOSE)
                        {
                            struct tm brokendowntime;
                            CPLUnixTimeToYMDHMS(mUnixTime, &brokendowntime);
                            CPLDebug(
                                GetDebugKey(),
                                "File[%d] = %s, is_dir = %d, size "
                                "= " CPL_FRMT_GUIB
                                ", time = %04d/%02d/%02d %02d:%02d:%02d",
                                nCount, pszFilename, bIsDirectory ? 1 : 0,
                                nFileSize, brokendowntime.tm_year + 1900,
                                brokendowntime.tm_mon + 1,
                                brokendowntime.tm_mday, brokendowntime.tm_hour,
                                brokendowntime.tm_min, brokendowntime.tm_sec);
                        }

                        nCount++;

                        if (nMaxFiles > 0 && oFileList.Count() > nMaxFiles)
                            break;
                    }

                    pszLine = c + 1;
                }

                if (c == nullptr)
                {
                    papszFileList = oFileList.StealList();
                    break;
                }
            }
            else
            {
                CPLStringList oFileList;
                *pbGotFileList = true;

                while ((c = strchr(pszLine, '\n')) != nullptr)
                {
                    *c = 0;
                    if (c - pszLine > 0 && c[-1] == '\r')
                        c[-1] = 0;

                    if (strcmp(pszLine, ".") != 0 && strcmp(pszLine, "..") != 0)
                    {
                        oFileList.AddString(pszLine);
                        if (ENABLE_DEBUG_VERBOSE)
                        {
                            CPLDebug(GetDebugKey(), "File[%d] = %s", nCount,
                                     pszLine);
                        }
                        nCount++;
                    }

                    pszLine = c + 1;
                }

                papszFileList = oFileList.StealList();
            }

            CPLFree(sWriteFuncData.pBuffer);
            sWriteFuncData.pBuffer = nullptr;
        }

        CPLFree(sWriteFuncData.pBuffer);
        curl_easy_cleanup(hCurlHandle);

        return papszFileList;
    }

    // Try to recognize HTML pages that list the content of a directory.
    // Currently this supports what Apache and shttpd can return.
    else if (STARTS_WITH(osURL.c_str(), "http://") ||
             STARTS_WITH(osURL.c_str(), "https://"))
    {
        std::string osDirname(std::move(osURL));
        osDirname += '/';

        CURLM *hCurlMultiHandle = GetCurlMultiHandleFor(osDirname);
        CURL *hCurlHandle = curl_easy_init();

        struct curl_slist *headers =
            VSICurlSetOptions(hCurlHandle, osDirname.c_str(), nullptr);

        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_RANGE, nullptr);

        WriteFuncStruct sWriteFuncData;
        VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA,
                                   &sWriteFuncData);
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                                   VSICurlHandleWriteFunc);

        char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER,
                                   szCurlErrBuf);

        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

        VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

        curl_slist_free_all(headers);

        NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

        if (sWriteFuncData.pBuffer == nullptr)
        {
            curl_easy_cleanup(hCurlHandle);
            return nullptr;
        }

        char **papszFileList = nullptr;
        if (STARTS_WITH_CI(sWriteFuncData.pBuffer, "<?xml") &&
            strstr(sWriteFuncData.pBuffer, "<ListBucketResult") != nullptr)
        {
            CPLStringList osFileList;
            std::string osBaseURL(pszDirname);
            osBaseURL += "/";
            bool bIsTruncated = true;
            bool ret = AnalyseS3FileList(
                osBaseURL, sWriteFuncData.pBuffer, osFileList, nMaxFiles,
                GetS3IgnoredStorageClasses(), bIsTruncated);
            // If the list is truncated, then don't report it.
            if (ret && !bIsTruncated)
            {
                if (osFileList.empty())
                {
                    // To avoid an error to be reported
                    osFileList.AddString(".");
                }
                papszFileList = osFileList.StealList();
                *pbGotFileList = true;
            }
        }
        else
        {
            papszFileList = ParseHTMLFileList(
                pszDirname, nMaxFiles, sWriteFuncData.pBuffer, pbGotFileList);
        }

        CPLFree(sWriteFuncData.pBuffer);
        curl_easy_cleanup(hCurlHandle);
        return papszFileList;
    }

    return nullptr;
}

/************************************************************************/
/*                       GetS3IgnoredStorageClasses()                   */
/************************************************************************/

std::set<std::string> VSICurlFilesystemHandlerBase::GetS3IgnoredStorageClasses()
{
    std::set<std::string> oSetIgnoredStorageClasses;
    const char *pszIgnoredStorageClasses =
        CPLGetConfigOption("CPL_VSIL_CURL_IGNORE_STORAGE_CLASSES", nullptr);
    const char *pszIgnoreGlacierStorage =
        CPLGetConfigOption("CPL_VSIL_CURL_IGNORE_GLACIER_STORAGE", nullptr);
    CPLStringList aosIgnoredStorageClasses(
        CSLTokenizeString2(pszIgnoredStorageClasses ? pszIgnoredStorageClasses
                                                    : "GLACIER,DEEP_ARCHIVE",
                           ",", 0));
    for (int i = 0; i < aosIgnoredStorageClasses.size(); ++i)
        oSetIgnoredStorageClasses.insert(aosIgnoredStorageClasses[i]);
    if (pszIgnoredStorageClasses == nullptr &&
        pszIgnoreGlacierStorage != nullptr &&
        !CPLTestBool(pszIgnoreGlacierStorage))
    {
        oSetIgnoredStorageClasses.clear();
    }
    return oSetIgnoredStorageClasses;
}

/************************************************************************/
/*                                Stat()                                */
/************************************************************************/

int VSICurlFilesystemHandlerBase::Stat(const char *pszFilename,
                                       VSIStatBufL *pStatBuf, int nFlags)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()) &&
        !STARTS_WITH_CI(pszFilename, "/vsicurl?"))
        return -1;

    memset(pStatBuf, 0, sizeof(VSIStatBufL));

    if ((nFlags & VSI_STAT_CACHE_ONLY) != 0)
    {
        cpl::FileProp oFileProp;
        if (!GetCachedFileProp(GetURLFromFilename(pszFilename).c_str(),
                               oFileProp) ||
            oFileProp.eExists != EXIST_YES)
        {
            return -1;
        }
        pStatBuf->st_mode = static_cast<unsigned short>(oFileProp.nMode);
        pStatBuf->st_mtime = oFileProp.mTime;
        pStatBuf->st_size = oFileProp.fileSize;
        return 0;
    }

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Stat");

    const std::string osFilename(pszFilename);

    if (!IsAllowedFilename(pszFilename))
        return -1;

    bool bListDir = true;
    bool bEmptyDir = false;
    std::string osURL(VSICurlGetURLFromFilename(pszFilename, nullptr, nullptr,
                                                nullptr, &bListDir, &bEmptyDir,
                                                nullptr, nullptr, nullptr));

    const char *pszOptionVal = VSIGetPathSpecificOption(
        pszFilename, "GDAL_DISABLE_READDIR_ON_OPEN", "NO");
    const bool bSkipReadDir =
        !bListDir || bEmptyDir || EQUAL(pszOptionVal, "EMPTY_DIR") ||
        CPLTestBool(pszOptionVal) || !AllowCachedDataFor(pszFilename);

    // Does it look like a FTP directory?
    if (STARTS_WITH(osURL.c_str(), "ftp://") && osFilename.back() == '/' &&
        !bSkipReadDir)
    {
        char **papszFileList = ReadDirEx(osFilename.c_str(), 0);
        if (papszFileList)
        {
            pStatBuf->st_mode = S_IFDIR;
            pStatBuf->st_size = 0;

            CSLDestroy(papszFileList);

            return 0;
        }
        return -1;
    }
    else if (strchr(CPLGetFilename(osFilename.c_str()), '.') != nullptr &&
             !STARTS_WITH_CI(CPLGetExtensionSafe(osFilename.c_str()).c_str(),
                             "zip") &&
             strstr(osFilename.c_str(), ".zip.") != nullptr &&
             strstr(osFilename.c_str(), ".ZIP.") != nullptr && !bSkipReadDir)
    {
        bool bGotFileList = false;
        char **papszFileList = ReadDirInternal(
            CPLGetDirnameSafe(osFilename.c_str()).c_str(), 0, &bGotFileList);
        const bool bFound =
            VSICurlIsFileInList(papszFileList,
                                CPLGetFilename(osFilename.c_str())) != -1;
        CSLDestroy(papszFileList);
        if (bGotFileList && !bFound)
        {
            return -1;
        }
    }

    VSICurlHandle *poHandle = CreateFileHandle(osFilename.c_str());
    if (poHandle == nullptr)
        return -1;

    if (poHandle->IsKnownFileSize() ||
        ((nFlags & VSI_STAT_SIZE_FLAG) && !poHandle->IsDirectory() &&
         CPLTestBool(CPLGetConfigOption("CPL_VSIL_CURL_SLOW_GET_SIZE", "YES"))))
    {
        pStatBuf->st_size = poHandle->GetFileSize(true);
    }

    const int nRet =
        poHandle->Exists((nFlags & VSI_STAT_SET_ERROR_FLAG) > 0) ? 0 : -1;
    pStatBuf->st_mtime = poHandle->GetMTime();
    pStatBuf->st_mode = static_cast<unsigned short>(poHandle->GetMode());
    if (pStatBuf->st_mode == 0)
        pStatBuf->st_mode = poHandle->IsDirectory() ? S_IFDIR : S_IFREG;
    delete poHandle;
    return nRet;
}

/************************************************************************/
/*                             ReadDirInternal()                        */
/************************************************************************/

char **VSICurlFilesystemHandlerBase::ReadDirInternal(const char *pszDirname,
                                                     int nMaxFiles,
                                                     bool *pbGotFileList)
{
    std::string osDirname(pszDirname);

    // Replace a/b/../c by a/c
    const auto posSlashDotDot = osDirname.find("/..");
    if (posSlashDotDot != std::string::npos && posSlashDotDot >= 1)
    {
        const auto posPrecedingSlash =
            osDirname.find_last_of('/', posSlashDotDot - 1);
        if (posPrecedingSlash != std::string::npos && posPrecedingSlash >= 1)
        {
            osDirname.erase(osDirname.begin() + posPrecedingSlash,
                            osDirname.begin() + posSlashDotDot + strlen("/.."));
        }
    }

    std::string osDirnameOri(osDirname);
    if (osDirname + "/" == GetFSPrefix())
    {
        osDirname += "/";
    }
    else if (osDirname != GetFSPrefix())
    {
        while (!osDirname.empty() && osDirname.back() == '/')
            osDirname.erase(osDirname.size() - 1);
    }

    if (osDirname.size() < GetFSPrefix().size())
    {
        if (pbGotFileList)
            *pbGotFileList = true;
        return nullptr;
    }

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("ReadDir");

    CPLMutexHolder oHolder(&hMutex);

    // If we know the file exists and is not a directory,
    // then don't try to list its content.
    FileProp cachedFileProp;
    if (GetCachedFileProp(GetURLFromFilename(osDirname.c_str()).c_str(),
                          cachedFileProp) &&
        cachedFileProp.eExists == EXIST_YES && !cachedFileProp.bIsDirectory)
    {
        if (osDirnameOri != osDirname)
        {
            if (GetCachedFileProp((GetURLFromFilename(osDirname) + "/").c_str(),
                                  cachedFileProp) &&
                cachedFileProp.eExists == EXIST_YES &&
                !cachedFileProp.bIsDirectory)
            {
                if (pbGotFileList)
                    *pbGotFileList = true;
                return nullptr;
            }
        }
        else
        {
            if (pbGotFileList)
                *pbGotFileList = true;
            return nullptr;
        }
    }

    CachedDirList cachedDirList;
    if (!GetCachedDirList(osDirname.c_str(), cachedDirList))
    {
        cachedDirList.oFileList.Assign(GetFileList(osDirname.c_str(), nMaxFiles,
                                                   &cachedDirList.bGotFileList),
                                       true);
        if (cachedDirList.bGotFileList && cachedDirList.oFileList.empty())
        {
            // To avoid an error to be reported
            cachedDirList.oFileList.AddString(".");
        }
        if (nMaxFiles <= 0 || cachedDirList.oFileList.size() < nMaxFiles)
        {
            // Only cache content if we didn't hit the limitation
            SetCachedDirList(osDirname.c_str(), cachedDirList);
        }
    }

    if (pbGotFileList)
        *pbGotFileList = cachedDirList.bGotFileList;

    return CSLDuplicate(cachedDirList.oFileList.List());
}

/************************************************************************/
/*                        InvalidateDirContent()                        */
/************************************************************************/

void VSICurlFilesystemHandlerBase::InvalidateDirContent(
    const std::string &osDirname)
{
    CPLMutexHolder oHolder(&hMutex);

    CachedDirList oCachedDirList;
    if (oCacheDirList.tryGet(osDirname, oCachedDirList))
    {
        nCachedFilesInDirList -= oCachedDirList.oFileList.size();
        oCacheDirList.remove(osDirname);
    }
}

/************************************************************************/
/*                             ReadDirEx()                              */
/************************************************************************/

char **VSICurlFilesystemHandlerBase::ReadDirEx(const char *pszDirname,
                                               int nMaxFiles)
{
    return ReadDirInternal(pszDirname, nMaxFiles, nullptr);
}

/************************************************************************/
/*                             SiblingFiles()                           */
/************************************************************************/

char **VSICurlFilesystemHandlerBase::SiblingFiles(const char *pszFilename)
{
    /* Small optimization to avoid unnecessary stat'ing from PAux or ENVI */
    /* drivers. The MBTiles driver needs no companion file. */
    if (EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "mbtiles"))
    {
        return static_cast<char **>(CPLCalloc(1, sizeof(char *)));
    }
    return nullptr;
}

/************************************************************************/
/*                          GetFileMetadata()                           */
/************************************************************************/

char **VSICurlFilesystemHandlerBase::GetFileMetadata(const char *pszFilename,
                                                     const char *pszDomain,
                                                     CSLConstList)
{
    if (pszDomain == nullptr || !EQUAL(pszDomain, "HEADERS"))
        return nullptr;
    std::unique_ptr<VSICurlHandle> poHandle(CreateFileHandle(pszFilename));
    if (poHandle == nullptr)
        return nullptr;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("GetFileMetadata");

    poHandle->GetFileSizeOrHeaders(true, true);
    return CSLDuplicate(poHandle->GetHeaders().List());
}

/************************************************************************/
/*                       VSIAppendWriteHandle()                         */
/************************************************************************/

VSIAppendWriteHandle::VSIAppendWriteHandle(VSICurlFilesystemHandlerBase *poFS,
                                           const char *pszFSPrefix,
                                           const char *pszFilename,
                                           int nChunkSize)
    : m_poFS(poFS), m_osFSPrefix(pszFSPrefix), m_osFilename(pszFilename),
      m_oRetryParameters(CPLStringList(CPLHTTPGetOptionsFromEnv(pszFilename))),
      m_nBufferSize(nChunkSize)
{
    m_pabyBuffer = static_cast<GByte *>(VSIMalloc(m_nBufferSize));
    if (m_pabyBuffer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot allocate working buffer for %s writing",
                 m_osFSPrefix.c_str());
    }
}

/************************************************************************/
/*                      ~VSIAppendWriteHandle()                         */
/************************************************************************/

VSIAppendWriteHandle::~VSIAppendWriteHandle()
{
    /* WARNING: implementation should call Close() themselves */
    /* cannot be done safely from here, since Send() can be called. */
    CPLFree(m_pabyBuffer);
}

/************************************************************************/
/*                               Seek()                                 */
/************************************************************************/

int VSIAppendWriteHandle::Seek(vsi_l_offset nOffset, int nWhence)
{
    if (!((nWhence == SEEK_SET && nOffset == m_nCurOffset) ||
          (nWhence == SEEK_CUR && nOffset == 0) ||
          (nWhence == SEEK_END && nOffset == 0)))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Seek not supported on writable %s files",
                 m_osFSPrefix.c_str());
        m_bError = true;
        return -1;
    }
    return 0;
}

/************************************************************************/
/*                               Tell()                                 */
/************************************************************************/

vsi_l_offset VSIAppendWriteHandle::Tell()
{
    return m_nCurOffset;
}

/************************************************************************/
/*                               Read()                                 */
/************************************************************************/

size_t VSIAppendWriteHandle::Read(void * /* pBuffer */, size_t /* nSize */,
                                  size_t /* nMemb */)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "Read not supported on writable %s files", m_osFSPrefix.c_str());
    m_bError = true;
    return 0;
}

/************************************************************************/
/*                         ReadCallBackBuffer()                         */
/************************************************************************/

size_t VSIAppendWriteHandle::ReadCallBackBuffer(char *buffer, size_t size,
                                                size_t nitems, void *instream)
{
    VSIAppendWriteHandle *poThis =
        static_cast<VSIAppendWriteHandle *>(instream);
    const int nSizeMax = static_cast<int>(size * nitems);
    const int nSizeToWrite = std::min(
        nSizeMax, poThis->m_nBufferOff - poThis->m_nBufferOffReadCallback);
    memcpy(buffer, poThis->m_pabyBuffer + poThis->m_nBufferOffReadCallback,
           nSizeToWrite);
    poThis->m_nBufferOffReadCallback += nSizeToWrite;
    return nSizeToWrite;
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

size_t VSIAppendWriteHandle::Write(const void *pBuffer, size_t nSize,
                                   size_t nMemb)
{
    if (m_bError)
        return 0;

    size_t nBytesToWrite = nSize * nMemb;
    if (nBytesToWrite == 0)
        return 0;

    const GByte *pabySrcBuffer = reinterpret_cast<const GByte *>(pBuffer);
    while (nBytesToWrite > 0)
    {
        if (m_nBufferOff == m_nBufferSize)
        {
            if (!Send(false))
            {
                m_bError = true;
                return 0;
            }
            m_nBufferOff = 0;
        }

        const int nToWriteInBuffer = static_cast<int>(std::min(
            static_cast<size_t>(m_nBufferSize - m_nBufferOff), nBytesToWrite));
        memcpy(m_pabyBuffer + m_nBufferOff, pabySrcBuffer, nToWriteInBuffer);
        pabySrcBuffer += nToWriteInBuffer;
        m_nBufferOff += nToWriteInBuffer;
        m_nCurOffset += nToWriteInBuffer;
        nBytesToWrite -= nToWriteInBuffer;
    }
    return nMemb;
}

/************************************************************************/
/*                                 Close()                              */
/************************************************************************/

int VSIAppendWriteHandle::Close()
{
    int nRet = 0;
    if (!m_bClosed)
    {
        m_bClosed = true;
        if (!m_bError && !Send(true))
            nRet = -1;
    }
    return nRet;
}

/************************************************************************/
/*                         CurlRequestHelper()                          */
/************************************************************************/

CurlRequestHelper::CurlRequestHelper()
{
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    VSICURLInitWriteFuncStruct(&sWriteFuncHeaderData, nullptr, nullptr,
                               nullptr);
}

/************************************************************************/
/*                        ~CurlRequestHelper()                          */
/************************************************************************/

CurlRequestHelper::~CurlRequestHelper()
{
    CPLFree(sWriteFuncData.pBuffer);
    CPLFree(sWriteFuncHeaderData.pBuffer);
}

/************************************************************************/
/*                             perform()                                */
/************************************************************************/

long CurlRequestHelper::perform(CURL *hCurlHandle, struct curl_slist *headers,
                                VSICurlFilesystemHandlerBase *poFS,
                                IVSIS3LikeHandleHelper *poS3HandleHelper)
{
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    poS3HandleHelper->ResetQueryParameters();

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERDATA,
                               &sWriteFuncHeaderData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HEADERFUNCTION,
                               VSICurlHandleWriteFunc);

    szCurlErrBuf[0] = '\0';
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    VSICURLMultiPerform(poFS->GetCurlMultiHandleFor(poS3HandleHelper->GetURL()),
                        hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);
    return response_code;
}

/************************************************************************/
/*                       NetworkStatisticsLogger                        */
/************************************************************************/

// Global variable
NetworkStatisticsLogger NetworkStatisticsLogger::gInstance{};
int NetworkStatisticsLogger::gnEnabled = -1;  // unknown state

static void ShowNetworkStats()
{
    printf("Network statistics:\n%s\n",  // ok
           NetworkStatisticsLogger::GetReportAsSerializedJSON().c_str());
}

void NetworkStatisticsLogger::ReadEnabled()
{
    const bool bShowNetworkStats =
        CPLTestBool(CPLGetConfigOption("CPL_VSIL_SHOW_NETWORK_STATS", "NO"));
    gnEnabled =
        (bShowNetworkStats || CPLTestBool(CPLGetConfigOption(
                                  "CPL_VSIL_NETWORK_STATS_ENABLED", "NO")))
            ? TRUE
            : FALSE;
    if (bShowNetworkStats)
    {
        static bool bRegistered = false;
        if (!bRegistered)
        {
            bRegistered = true;
            atexit(ShowNetworkStats);
        }
    }
}

void NetworkStatisticsLogger::EnterFileSystem(const char *pszName)
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_mapThreadIdToContextPath[CPLGetPID()].push_back(
        ContextPathItem(ContextPathType::FILESYSTEM, pszName));
}

void NetworkStatisticsLogger::LeaveFileSystem()
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_mapThreadIdToContextPath[CPLGetPID()].pop_back();
}

void NetworkStatisticsLogger::EnterFile(const char *pszName)
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_mapThreadIdToContextPath[CPLGetPID()].push_back(
        ContextPathItem(ContextPathType::FILE, pszName));
}

void NetworkStatisticsLogger::LeaveFile()
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_mapThreadIdToContextPath[CPLGetPID()].pop_back();
}

void NetworkStatisticsLogger::EnterAction(const char *pszName)
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_mapThreadIdToContextPath[CPLGetPID()].push_back(
        ContextPathItem(ContextPathType::ACTION, pszName));
}

void NetworkStatisticsLogger::LeaveAction()
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_mapThreadIdToContextPath[CPLGetPID()].pop_back();
}

std::vector<NetworkStatisticsLogger::Counters *>
NetworkStatisticsLogger::GetCountersForContext()
{
    std::vector<Counters *> v;
    const auto &contextPath = gInstance.m_mapThreadIdToContextPath[CPLGetPID()];

    Stats *curStats = &m_stats;
    v.push_back(&(curStats->counters));

    bool inFileSystem = false;
    bool inFile = false;
    bool inAction = false;
    for (const auto &item : contextPath)
    {
        if (item.eType == ContextPathType::FILESYSTEM)
        {
            if (inFileSystem)
                continue;
            inFileSystem = true;
        }
        else if (item.eType == ContextPathType::FILE)
        {
            if (inFile)
                continue;
            inFile = true;
        }
        else if (item.eType == ContextPathType::ACTION)
        {
            if (inAction)
                continue;
            inAction = true;
        }

        curStats = &(curStats->children[item]);
        v.push_back(&(curStats->counters));
    }

    return v;
}

void NetworkStatisticsLogger::LogGET(size_t nDownloadedBytes)
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    for (auto counters : gInstance.GetCountersForContext())
    {
        counters->nGET++;
        counters->nGETDownloadedBytes += nDownloadedBytes;
    }
}

void NetworkStatisticsLogger::LogPUT(size_t nUploadedBytes)
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    for (auto counters : gInstance.GetCountersForContext())
    {
        counters->nPUT++;
        counters->nPUTUploadedBytes += nUploadedBytes;
    }
}

void NetworkStatisticsLogger::LogHEAD()
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    for (auto counters : gInstance.GetCountersForContext())
    {
        counters->nHEAD++;
    }
}

void NetworkStatisticsLogger::LogPOST(size_t nUploadedBytes,
                                      size_t nDownloadedBytes)
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    for (auto counters : gInstance.GetCountersForContext())
    {
        counters->nPOST++;
        counters->nPOSTUploadedBytes += nUploadedBytes;
        counters->nPOSTDownloadedBytes += nDownloadedBytes;
    }
}

void NetworkStatisticsLogger::LogDELETE()
{
    if (!IsEnabled())
        return;
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    for (auto counters : gInstance.GetCountersForContext())
    {
        counters->nDELETE++;
    }
}

void NetworkStatisticsLogger::Reset()
{
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);
    gInstance.m_stats = Stats();
    gnEnabled = -1;
}

void NetworkStatisticsLogger::Stats::AsJSON(CPLJSONObject &oJSON) const
{
    CPLJSONObject oMethods;
    if (counters.nHEAD)
        oMethods.Add("HEAD/count", counters.nHEAD);
    if (counters.nGET)
        oMethods.Add("GET/count", counters.nGET);
    if (counters.nGETDownloadedBytes)
        oMethods.Add("GET/downloaded_bytes", counters.nGETDownloadedBytes);
    if (counters.nPUT)
        oMethods.Add("PUT/count", counters.nPUT);
    if (counters.nPUTUploadedBytes)
        oMethods.Add("PUT/uploaded_bytes", counters.nPUTUploadedBytes);
    if (counters.nPOST)
        oMethods.Add("POST/count", counters.nPOST);
    if (counters.nPOSTUploadedBytes)
        oMethods.Add("POST/uploaded_bytes", counters.nPOSTUploadedBytes);
    if (counters.nPOSTDownloadedBytes)
        oMethods.Add("POST/downloaded_bytes", counters.nPOSTDownloadedBytes);
    if (counters.nDELETE)
        oMethods.Add("DELETE/count", counters.nDELETE);
    oJSON.Add("methods", oMethods);
    CPLJSONObject oFiles;
    bool bFilesAdded = false;
    for (const auto &kv : children)
    {
        CPLJSONObject childJSON;
        kv.second.AsJSON(childJSON);
        if (kv.first.eType == ContextPathType::FILESYSTEM)
        {
            std::string osName(kv.first.osName);
            if (!osName.empty() && osName[0] == '/')
                osName = osName.substr(1);
            if (!osName.empty() && osName.back() == '/')
                osName.pop_back();
            oJSON.Add(("handlers/" + osName).c_str(), childJSON);
        }
        else if (kv.first.eType == ContextPathType::FILE)
        {
            if (!bFilesAdded)
            {
                bFilesAdded = true;
                oJSON.Add("files", oFiles);
            }
            oFiles.AddNoSplitName(kv.first.osName.c_str(), childJSON);
        }
        else if (kv.first.eType == ContextPathType::ACTION)
        {
            oJSON.Add(("actions/" + kv.first.osName).c_str(), childJSON);
        }
    }
}

std::string NetworkStatisticsLogger::GetReportAsSerializedJSON()
{
    std::lock_guard<std::mutex> oLock(gInstance.m_mutex);

    CPLJSONObject oJSON;
    gInstance.m_stats.AsJSON(oJSON);
    return oJSON.Format(CPLJSONObject::PrettyFormat::Pretty);
}

} /* end of namespace cpl */

/************************************************************************/
/*                     VSICurlParseUnixPermissions()                    */
/************************************************************************/

int VSICurlParseUnixPermissions(const char *pszPermissions)
{
    if (strlen(pszPermissions) != 9)
        return 0;
    int nMode = 0;
    if (pszPermissions[0] == 'r')
        nMode |= S_IRUSR;
    if (pszPermissions[1] == 'w')
        nMode |= S_IWUSR;
    if (pszPermissions[2] == 'x')
        nMode |= S_IXUSR;
    if (pszPermissions[3] == 'r')
        nMode |= S_IRGRP;
    if (pszPermissions[4] == 'w')
        nMode |= S_IWGRP;
    if (pszPermissions[5] == 'x')
        nMode |= S_IXGRP;
    if (pszPermissions[6] == 'r')
        nMode |= S_IROTH;
    if (pszPermissions[7] == 'w')
        nMode |= S_IWOTH;
    if (pszPermissions[8] == 'x')
        nMode |= S_IXOTH;
    return nMode;
}

/************************************************************************/
/*                  Cache of file properties.                           */
/************************************************************************/

static std::mutex oCacheFilePropMutex;
static lru11::Cache<std::string, cpl::FileProp> *poCacheFileProp = nullptr;

/************************************************************************/
/*                   VSICURLGetCachedFileProp()                         */
/************************************************************************/

bool VSICURLGetCachedFileProp(const char *pszURL, cpl::FileProp &oFileProp)
{
    std::lock_guard<std::mutex> oLock(oCacheFilePropMutex);
    return poCacheFileProp != nullptr &&
           poCacheFileProp->tryGet(std::string(pszURL), oFileProp) &&
           // Let a chance to use new auth parameters
           !(oFileProp.eExists == cpl::EXIST_NO &&
             gnGenerationAuthParameters != oFileProp.nGenerationAuthParameters);
}

/************************************************************************/
/*                   VSICURLSetCachedFileProp()                         */
/************************************************************************/

void VSICURLSetCachedFileProp(const char *pszURL, cpl::FileProp &oFileProp)
{
    std::lock_guard<std::mutex> oLock(oCacheFilePropMutex);
    if (poCacheFileProp == nullptr)
        poCacheFileProp =
            new lru11::Cache<std::string, cpl::FileProp>(100 * 1024);
    oFileProp.nGenerationAuthParameters = gnGenerationAuthParameters;
    poCacheFileProp->insert(std::string(pszURL), oFileProp);
}

/************************************************************************/
/*                   VSICURLInvalidateCachedFileProp()                  */
/************************************************************************/

void VSICURLInvalidateCachedFileProp(const char *pszURL)
{
    std::lock_guard<std::mutex> oLock(oCacheFilePropMutex);
    if (poCacheFileProp != nullptr)
        poCacheFileProp->remove(std::string(pszURL));
}

/************************************************************************/
/*               VSICURLInvalidateCachedFilePropPrefix()                */
/************************************************************************/

void VSICURLInvalidateCachedFilePropPrefix(const char *pszURL)
{
    std::lock_guard<std::mutex> oLock(oCacheFilePropMutex);
    if (poCacheFileProp != nullptr)
    {
        std::list<std::string> keysToRemove;
        const size_t nURLSize = strlen(pszURL);
        auto lambda =
            [&keysToRemove, &pszURL, nURLSize](
                const lru11::KeyValuePair<std::string, cpl::FileProp> &kv)
        {
            if (strncmp(kv.key.c_str(), pszURL, nURLSize) == 0)
                keysToRemove.push_back(kv.key);
        };
        poCacheFileProp->cwalk(lambda);
        for (const auto &key : keysToRemove)
            poCacheFileProp->remove(key);
    }
}

/************************************************************************/
/*                   VSICURLDestroyCacheFileProp()                      */
/************************************************************************/

void VSICURLDestroyCacheFileProp()
{
    std::lock_guard<std::mutex> oLock(oCacheFilePropMutex);
    delete poCacheFileProp;
    poCacheFileProp = nullptr;
}

/************************************************************************/
/*                       VSICURLMultiCleanup()                          */
/************************************************************************/

void VSICURLMultiCleanup(CURLM *hCurlMultiHandle)
{
    void *old_handler = CPLHTTPIgnoreSigPipe();
    curl_multi_cleanup(hCurlMultiHandle);
    CPLHTTPRestoreSigPipeHandler(old_handler);
}

/************************************************************************/
/*                      VSICurlInstallReadCbk()                         */
/************************************************************************/

int VSICurlInstallReadCbk(VSILFILE *fp, VSICurlReadCbkFunc pfnReadCbk,
                          void *pfnUserData, int bStopOnInterruptUntilUninstall)
{
    return reinterpret_cast<cpl::VSICurlHandle *>(fp)->InstallReadCbk(
        pfnReadCbk, pfnUserData, bStopOnInterruptUntilUninstall);
}

/************************************************************************/
/*                    VSICurlUninstallReadCbk()                         */
/************************************************************************/

int VSICurlUninstallReadCbk(VSILFILE *fp)
{
    return reinterpret_cast<cpl::VSICurlHandle *>(fp)->UninstallReadCbk();
}

/************************************************************************/
/*                       VSICurlSetOptions()                            */
/************************************************************************/

struct curl_slist *VSICurlSetOptions(CURL *hCurlHandle, const char *pszURL,
                                     const char *const *papszOptions)
{
    struct curl_slist *headers = static_cast<struct curl_slist *>(
        CPLHTTPSetOptions(hCurlHandle, pszURL, papszOptions));

    long option = CURLFTPMETHOD_SINGLECWD;
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FTP_FILEMETHOD, option);

    // ftp://ftp2.cits.rncan.gc.ca/pub/cantopo/250k_tif/
    // doesn't like EPSV command,
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FTP_USE_EPSV, 0);

    return headers;
}

/************************************************************************/
/*                    VSICurlSetContentTypeFromExt()                    */
/************************************************************************/

struct curl_slist *VSICurlSetContentTypeFromExt(struct curl_slist *poList,
                                                const char *pszPath)
{
    struct curl_slist *iter = poList;
    while (iter != nullptr)
    {
        if (STARTS_WITH_CI(iter->data, "Content-Type"))
        {
            return poList;
        }
        iter = iter->next;
    }

    static const struct
    {
        const char *ext;
        const char *mime;
    } aosExtMimePairs[] = {
        {"txt", "text/plain"}, {"json", "application/json"},
        {"tif", "image/tiff"}, {"tiff", "image/tiff"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"jp2", "image/jp2"},  {"jpx", "image/jp2"},
        {"j2k", "image/jp2"},  {"jpc", "image/jp2"},
        {"png", "image/png"},
    };

    const std::string osExt = CPLGetExtensionSafe(pszPath);
    if (!osExt.empty())
    {
        for (const auto &pair : aosExtMimePairs)
        {
            if (EQUAL(osExt.c_str(), pair.ext))
            {

                const std::string osContentType(
                    CPLSPrintf("Content-Type: %s", pair.mime));
                poList = curl_slist_append(poList, osContentType.c_str());
#ifdef DEBUG_VERBOSE
                CPLDebug("HTTP", "Setting %s, based on lookup table.",
                         osContentType.c_str());
#endif
                break;
            }
        }
    }

    return poList;
}

/************************************************************************/
/*                VSICurlSetCreationHeadersFromOptions()                */
/************************************************************************/

struct curl_slist *VSICurlSetCreationHeadersFromOptions(
    struct curl_slist *headers, CSLConstList papszOptions, const char *pszPath)
{
    bool bContentTypeFound = false;
    for (CSLConstList papszIter = papszOptions; papszIter && *papszIter;
         ++papszIter)
    {
        char *pszKey = nullptr;
        const char *pszValue = CPLParseNameValue(*papszIter, &pszKey);
        if (pszKey && pszValue)
        {
            if (EQUAL(pszKey, "Content-Type"))
            {
                bContentTypeFound = true;
            }
            headers = curl_slist_append(headers,
                                        CPLSPrintf("%s: %s", pszKey, pszValue));
        }
        CPLFree(pszKey);
    }

    // If Content-type not found in papszOptions, try to set it from the
    // filename exstension.
    if (!bContentTypeFound)
    {
        headers = VSICurlSetContentTypeFromExt(headers, pszPath);
    }

    return headers;
}

#endif  // DOXYGEN_SKIP
//! @endcond

/************************************************************************/
/*                   VSIInstallCurlFileHandler()                        */
/************************************************************************/

/*!
 \brief Install /vsicurl/ HTTP/FTP file system handler (requires libcurl)

 \verbatim embed:rst
 See :ref:`/vsicurl/ documentation <vsicurl>`
 \endverbatim

 @since GDAL 1.8.0
 */
void VSIInstallCurlFileHandler(void)
{
    VSIFilesystemHandler *poHandler = new cpl::VSICurlFilesystemHandler;
    VSIFileManager::InstallHandler("/vsicurl/", poHandler);
    VSIFileManager::InstallHandler("/vsicurl?", poHandler);
}

/************************************************************************/
/*                         VSICurlClearCache()                          */
/************************************************************************/

/**
 * \brief Clean local cache associated with /vsicurl/ (and related file systems)
 *
 * /vsicurl (and related file systems like /vsis3/, /vsigs/, /vsiaz/, /vsioss/,
 * /vsiswift/) cache a number of
 * metadata and data for faster execution in read-only scenarios. But when the
 * content on the server-side may change during the same process, those
 * mechanisms can prevent opening new files, or give an outdated version of
 * them.
 *
 * @since GDAL 2.2.1
 */

void VSICurlClearCache(void)
{
    // FIXME ? Currently we have different filesystem instances for
    // vsicurl/, /vsis3/, /vsigs/ . So each one has its own cache of regions.
    // File properties cache are now shared
    char **papszPrefix = VSIFileManager::GetPrefixes();
    for (size_t i = 0; papszPrefix && papszPrefix[i]; ++i)
    {
        auto poFSHandler = dynamic_cast<cpl::VSICurlFilesystemHandlerBase *>(
            VSIFileManager::GetHandler(papszPrefix[i]));

        if (poFSHandler)
            poFSHandler->ClearCache();
    }
    CSLDestroy(papszPrefix);

    VSICurlStreamingClearCache();
}

/************************************************************************/
/*                      VSICurlPartialClearCache()                      */
/************************************************************************/

/**
 * \brief Clean local cache associated with /vsicurl/ (and related file systems)
 * for a given filename (and its subfiles and subdirectories if it is a
 * directory)
 *
 * /vsicurl (and related file systems like /vsis3/, /vsigs/, /vsiaz/, /vsioss/,
 * /vsiswift/) cache a number of
 * metadata and data for faster execution in read-only scenarios. But when the
 * content on the server-side may change during the same process, those
 * mechanisms can prevent opening new files, or give an outdated version of
 * them.
 *
 * The filename prefix must start with the name of a known virtual file system
 * (such as "/vsicurl/", "/vsis3/")
 *
 * VSICurlPartialClearCache("/vsis3/b") will clear all cached state for any file
 * or directory starting with that prefix, so potentially "/vsis3/bucket",
 * "/vsis3/basket/" or "/vsis3/basket/object".
 *
 * @param pszFilenamePrefix Filename prefix
 * @since GDAL 2.4.0
 */

void VSICurlPartialClearCache(const char *pszFilenamePrefix)
{
    auto poFSHandler = dynamic_cast<cpl::VSICurlFilesystemHandlerBase *>(
        VSIFileManager::GetHandler(pszFilenamePrefix));

    if (poFSHandler)
        poFSHandler->PartialClearCache(pszFilenamePrefix);
}

/************************************************************************/
/*                        VSINetworkStatsReset()                        */
/************************************************************************/

/**
 * \brief Clear network related statistics.
 *
 * The effect of the CPL_VSIL_NETWORK_STATS_ENABLED configuration option
 * will also be reset. That is, that the next network access will check its
 * value again.
 *
 * @since GDAL 3.2.0
 */

void VSINetworkStatsReset(void)
{
    cpl::NetworkStatisticsLogger::Reset();
}

/************************************************************************/
/*                 VSINetworkStatsGetAsSerializedJSON()                 */
/************************************************************************/

/**
 * \brief Return network related statistics, as a JSON serialized object.
 *
 * Statistics collecting should be enabled with the
 CPL_VSIL_NETWORK_STATS_ENABLED
 * configuration option set to YES before any network activity starts
 * (for efficiency, reading it is cached on first access, until
 VSINetworkStatsReset() is called)
 *
 * Statistics can also be emitted on standard output at process termination if
 * the CPL_VSIL_SHOW_NETWORK_STATS configuration option is set to YES.
 *
 * Example of output:
 * \code{.js}
 * {
 *   "methods":{
 *     "GET":{
 *       "count":6,
 *       "downloaded_bytes":40825
 *     },
 *     "PUT":{
 *       "count":1,
 *       "uploaded_bytes":35472
 *     }
 *   },
 *   "handlers":{
 *     "vsigs":{
 *       "methods":{
 *         "GET":{
 *           "count":2,
 *           "downloaded_bytes":446
 *         },
 *         "PUT":{
 *           "count":1,
 *           "uploaded_bytes":35472
 *         }
 *       },
 *       "files":{
 *         "\/vsigs\/spatialys\/byte.tif":{
 *           "methods":{
 *             "PUT":{
 *               "count":1,
 *               "uploaded_bytes":35472
 *             }
 *           },
 *           "actions":{
 *             "Write":{
 *               "methods":{
 *                 "PUT":{
 *                   "count":1,
 *                   "uploaded_bytes":35472
 *                 }
 *               }
 *             }
 *           }
 *         }
 *       },
 *       "actions":{
 *         "Stat":{
 *           "methods":{
 *             "GET":{
 *               "count":2,
 *               "downloaded_bytes":446
 *             }
 *           },
 *           "files":{
 *             "\/vsigs\/spatialys\/byte.tif\/":{
 *               "methods":{
 *                 "GET":{
 *                   "count":1,
 *                   "downloaded_bytes":181
 *                 }
 *               }
 *             }
 *           }
 *         }
 *       }
 *     },
 *     "vsis3":{
 *          [...]
 *     }
 *   }
 * }
 * \endcode
 *
 * @param papszOptions Unused.
 * @return a JSON serialized string to free with VSIFree(), or nullptr
 * @since GDAL 3.2.0
 */

char *VSINetworkStatsGetAsSerializedJSON(CPL_UNUSED char **papszOptions)
{
    return CPLStrdup(
        cpl::NetworkStatisticsLogger::GetReportAsSerializedJSON().c_str());
}

#endif /* HAVE_CURL */

#undef ENABLE_DEBUG
