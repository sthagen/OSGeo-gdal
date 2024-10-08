/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  The library set-up/clean-up routines.
 * Author:   Mateusz Loskot <mateusz@loskot.net>
 *
 ******************************************************************************
 * Copyright (c) 2010, Mateusz Loskot <mateusz@loskot.net>
 * Copyright (c) 2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal.h"
#include "gdalpython.h"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "ogr_api.h"

static bool bInGDALGlobalDestructor = false;
extern "C" int CPL_DLL GDALIsInGlobalDestructor();

int GDALIsInGlobalDestructor()
{
    return bInGDALGlobalDestructor;
}

void CPLFinalizeTLS();

static bool bGDALDestroyAlreadyCalled = FALSE;

/************************************************************************/
/*                           GDALDestroy()                              */
/************************************************************************/

/** Finalize GDAL/OGR library.
 *
 * This function calls GDALDestroyDriverManager() and OGRCleanupAll() and
 * finalize Thread Local Storage variables.
 *
 * Prior to GDAL 2.4.0, this function should normally be explicitly called by
 * application code if GDAL is dynamically linked (but that does not hurt),
 * since it was automatically called through
 * the unregistration mechanisms of dynamic library loading.
 *
 * Since GDAL 2.4.0, this function may be called by application code, since
 * it is no longer called automatically, on non-MSVC builds, due to ordering
 * problems with respect to automatic destruction of global C++ objects.
 *
 * Note: no GDAL/OGR code should be called after this call!
 *
 * @since GDAL 2.0
 */

void GDALDestroy(void)
{
    if (bGDALDestroyAlreadyCalled)
        return;
    bGDALDestroyAlreadyCalled = true;

    bInGDALGlobalDestructor = true;

    // logging/error handling may call GDALIsInGlobalDestructor()
    CPLDebug("GDAL", "In GDALDestroy - unloading GDAL shared library.");

    GDALDestroyDriverManager();

    OGRCleanupAll();
    GDALPythonFinalize();
    bInGDALGlobalDestructor = false;

    /* See corresponding bug reports: */
    /* https://trac.osgeo.org/gdal/ticket/6139 */
    /* https://trac.osgeo.org/gdal/ticket/6868 */
    /* Needed in case no driver manager has been instantiated. */
    CPLFreeConfig();
    CPLFinalizeTLS();
    CPLCleanupErrorMutex();
    CPLCleanupMasterMutex();
}

/************************************************************************/
/*  The library set-up/clean-up routines implemented with               */
/*  GNU C/C++ extensions.                                               */
/*  TODO: Is it Linux-only solution or Unix portable?                   */
/************************************************************************/
#ifdef __GNUC__

static void GDALInitialize() __attribute__((constructor));

/************************************************************************/
/* Called when GDAL is loaded by loader or by dlopen(),                 */
/* and before dlopen() returns.                                         */
/************************************************************************/

static void GDALInitialize()
{
    // nothing to do
    // CPLDebug("GDAL", "Library loaded");
#ifdef DEBUG
    const char *pszLocale = CPLGetConfigOption("GDAL_LOCALE", nullptr);
    if (pszLocale)
        CPLsetlocale(LC_ALL, pszLocale);
#endif
}

#endif  // __GNUC__

/************************************************************************/
/*  The library set-up/clean-up routine implemented as DllMain entry    */
/*  point specific for Windows.                                         */
/************************************************************************/
#ifdef _MSC_VER
#ifndef CPL_DISABLE_DLL

#include <windows.h>

extern "C" int WINAPI DllMain(HINSTANCE /* hInstance */, DWORD dwReason,
                              LPVOID /* lpReserved */)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        // nothing to do
    }
    else if (dwReason == DLL_THREAD_ATTACH)
    {
        // nothing to do
    }
    else if (dwReason == DLL_THREAD_DETACH)
    {
        ::CPLCleanupTLS();
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        GDALDestroy();
    }

    return 1;  // ignored for all reasons but DLL_PROCESS_ATTACH
}

#endif  // CPL_DISABLE_DLL
#endif  // _MSC_VER
