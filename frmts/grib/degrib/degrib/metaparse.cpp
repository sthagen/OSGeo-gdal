/*****************************************************************************
 * metaparse.c
 *
 * DESCRIPTION
 *    This file contains the code necessary to initialize the meta data
 * structure, and parse the meta data that comes out of the GRIB2 decoder.
 *
 * HISTORY
 *    9/2002 Arthur Taylor (MDL / RSIS): Created.
 *
 * NOTES
 * 1) Need to add support for GS3_ORTHOGRAPHIC = 90,
 *    GS3_EQUATOR_EQUIDIST = 110, GS3_AZIMUTH_RANGE = 120
 * 2) Need to add support for GS4_RADAR = 20
 *****************************************************************************
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits>
#include "clock.h"
#include "meta.h"
#include "metaname.h"
#include "myassert.h"
#include "myerror.h"
#include "scan.h"
#include "weather.h"
#include "hazard.h"
#include "tendian.h"
#include "myutil.h"

#include "cpl_string.h"

static void debug_printf(const char* fmt, ... ) CPL_PRINT_FUNC_FORMAT (1, 2);

static void debug_printf(const char* fmt, ... )
{
    va_list args;

    va_start( args, fmt );
    CPLDebug("GRIB", "%s", CPLString().vPrintf(fmt, args ).c_str() );
    va_end( args );
}

#undef printf
#define printf debug_printf


/*****************************************************************************
 * MetaInit() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To initialize a grib_metaData structure.
 *
 * ARGUMENTS
 * meta = The structure to fill. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 *****************************************************************************
 */
void MetaInit (grib_MetaData *meta)
{
   meta->element = nullptr;
   meta->comment = nullptr;
   meta->unitName = nullptr;
   meta->convert = 0;
   meta->shortFstLevel = nullptr;
   meta->longFstLevel = nullptr;
   meta->pds2.sect2.ptrType = GS2_NONE;

   meta->pds2.sect2.wx.data = nullptr;
   meta->pds2.sect2.wx.dataLen = 0;
   meta->pds2.sect2.wx.maxLen = 0;
   meta->pds2.sect2.wx.ugly = nullptr;
   meta->pds2.sect2.unknown.data = nullptr;
   meta->pds2.sect2.unknown.dataLen = 0;
   meta->pds2.sect2.hazard.data = nullptr;
   meta->pds2.sect2.hazard.dataLen = 0;
   meta->pds2.sect2.hazard.maxLen = 0;
   meta->pds2.sect2.hazard.haz = nullptr;

   meta->pds2.sect4.numInterval = 0;
   meta->pds2.sect4.Interval = nullptr;
   meta->pds2.sect4.numBands = 0;
   meta->pds2.sect4.bands = nullptr;
   return;
}

/*****************************************************************************
 * MetaSect2Free() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To free the section 2 data in the grib_metaData structure.
 *
 * ARGUMENTS
 * meta = The structure to free. (Input/Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *   2/2003 Arthur Taylor (MDL/RSIS): Created.
 *   3/2003 AAT: Cleaned up declaration of variable: WxType.
 *
 * NOTES
 *****************************************************************************
 */
void MetaSect2Free (grib_MetaData *meta)
{
   size_t i;            /* Counter for use when freeing Wx data. */

   if (meta->pds2.sect2.ptrType == GS2_WXTYPE) {
      for (i = 0; i < meta->pds2.sect2.wx.dataLen; i++) {
         free (meta->pds2.sect2.wx.data[i]);
         FreeUglyString (&(meta->pds2.sect2.wx.ugly[i]));
      }
      free (meta->pds2.sect2.wx.ugly);
      meta->pds2.sect2.wx.ugly = nullptr;
      free (meta->pds2.sect2.wx.data);
      meta->pds2.sect2.wx.data = nullptr;
      free (meta->pds2.sect2.wx.f_valid);
      meta->pds2.sect2.wx.f_valid = nullptr;
      meta->pds2.sect2.wx.dataLen = 0;
      meta->pds2.sect2.wx.maxLen = 0;
   } else if (meta->pds2.sect2.ptrType == GS2_HAZARD) {
      for (i = 0; i < meta->pds2.sect2.hazard.dataLen; i++) {
         free (meta->pds2.sect2.hazard.data[i]);
         FreeHazardString (&(meta->pds2.sect2.hazard.haz[i]));
      }
      free (meta->pds2.sect2.hazard.haz);
      meta->pds2.sect2.hazard.haz = nullptr;
      free (meta->pds2.sect2.hazard.data);
      meta->pds2.sect2.hazard.data = nullptr;
      free (meta->pds2.sect2.hazard.f_valid);
      meta->pds2.sect2.hazard.f_valid = nullptr;
      meta->pds2.sect2.hazard.dataLen = 0;
      meta->pds2.sect2.hazard.maxLen = 0;
   } else {
      free (meta->pds2.sect2.unknown.data);
      meta->pds2.sect2.unknown.data = nullptr;
      meta->pds2.sect2.unknown.dataLen = 0;
   }
   meta->pds2.sect2.ptrType = GS2_NONE;
}

/*****************************************************************************
 * MetaFree() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To free a grib_metaData structure.
 *
 * ARGUMENTS
 * meta = The structure to free. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 *****************************************************************************
 */
void MetaFree (grib_MetaData *meta)
{
   free (meta->pds2.sect4.bands);
   meta->pds2.sect4.bands = nullptr;
   meta->pds2.sect4.numBands = 0;
   free (meta->pds2.sect4.Interval);
   meta->pds2.sect4.Interval = nullptr;
   meta->pds2.sect4.numInterval = 0;
   MetaSect2Free (meta);
   free (meta->unitName);
   meta->unitName = nullptr;
   meta->convert = 0;
   free (meta->comment);
   meta->comment = nullptr;
   free (meta->element);
   meta->element = nullptr;
   free (meta->shortFstLevel);
   meta->shortFstLevel = nullptr;
   free (meta->longFstLevel);
   meta->longFstLevel = nullptr;
}

/*****************************************************************************
 * ParseTime() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *    To parse the time data from the grib2 integer array to a time_t in
 * UTC seconds from the epoch.
 *
 * ARGUMENTS
 * AnsTime = The time_t value to fill with the resulting time. (Output)
 *    year = The year to parse. (Input)
 *     mon = The month to parse. (Input)
 *     day = The day to parse. (Input)
 *    hour = The hour to parse. (Input)
 *     min = The minute to parse. (Input)
 *     sec = The second to parse. (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = gribLen is too small.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *   4/2003 AAT: Modified to use year/mon/day/hour/min/sec instead of an
 *               integer array.
 *   2/2004 AAT: Added error checks (because of corrupt GRIB1 files)
 *
 * NOTES
 * 1) Couldn't use the default time_zone variable (concern over portability
 *    issues), so we print the hours, and compare them to the hours we had
 *    intended.  Then subtract the difference from the AnsTime.
 * 2) Need error check for times outside of 1902..2037.
 *****************************************************************************
 */
int ParseTime (double *AnsTime, int year, uChar mon, uChar day, uChar hour,
               uChar min, uChar sec)
{
   /* struct tm time; *//* A temporary variable to put the time info into. */
   /* char buffer[10]; *//* Used when printing the AnsTime's Hr. */
   /* int timeZone; *//* The adjustment in Hr needed to get the right UTC * time. */

   if ((year < 1900) || (year > 2100)) {
      errSprintf ("ParseTime:: year %d is invalid\n", year);
/*      return -1; */
      year += 2000;
   }
   /* sec is allowed to be 61 for leap seconds. */
   if ((mon > 12) || (day == 0) || (day > 31) || (hour > 24) || (min > 60) ||
       (sec > 61)) {
      errSprintf ("ParseTime:: Problems with %d/%d %d:%d:%d\n", mon, day,
                  hour, min, sec);
      return -1;
   }
   Clock_ScanDate (AnsTime, year, mon, day);
   *AnsTime += hour * 3600. + min * 60. + sec;
/*   *AnsTime -= Clock_GetTimeZone() * 3600;*/

/*
   memset (&time, 0, sizeof (struct tm));
   time.tm_year = year - 1900;
   time.tm_mon = mon - 1;
   time.tm_mday = day;
   time.tm_hour = hour;
   time.tm_min = min;
   time.tm_sec = sec;
   printf ("%ld\n", mktime (&time));
   *AnsTime = mktime (&time) - (Clock_GetTimeZone () * 3600);
*/
   /* Cheap method of getting global time_zone variable. */
/*
   strftime (buffer, 10, "%H", gmtime (AnsTime));
   timeZone = atoi (buffer) - hour;
   if (timeZone < 0) {
      timeZone += 24;
   }
   *AnsTime = *AnsTime - (timeZone * 3600);
*/
   return 0;
}

/*****************************************************************************
 * ParseSect0() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To verify and parse section 0 data.
 *
 * ARGUMENTS
 *      is0 = The unpacked section 0 array. (Input)
 *      ns0 = The size of section 0. (Input)
 * grib_len = The length of the entire grib message. (Input)
 *     meta = The structure to fill. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = ns0 is too small.
 * -2 = unexpected values in is0.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 * 1) 1196575042L == ASCII representation of "GRIB"
 *****************************************************************************
 */
static int ParseSect0 (sInt4 *is0, sInt4 ns0, sInt4 grib_len,
                       grib_MetaData *meta)
{
   if (ns0 < 9) {
      return -1;
   }
   if ((is0[0] != 1196575042L) || (is0[7] != 2) || (is0[8] != grib_len)) {
      errSprintf ("ERROR IS0 has unexpected values: %ld %ld %ld\n",
                  is0[0], is0[7], is0[8]);
      errSprintf ("Should be %ld %d %ld\n", 1196575042L, 2, grib_len);
      return -2;
   }
   meta->pds2.prodType = (uChar) is0[6];
   return 0;
}

/*****************************************************************************
 * ParseSect1() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To verify and parse section 1 data.
 *
 * ARGUMENTS
 *  is1 = The unpacked section 1 array. (Input)
 *  ns1 = The size of section 1. (Input)
 * meta = The structure to fill. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = ns1 is too small.
 * -2 = unexpected values in is1.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 *****************************************************************************
 */
static int ParseSect1 (sInt4 *is1, sInt4 ns1, grib_MetaData *meta)
{
   if (ns1 < 21) {
      return -1;
   }
   if (is1[4] != 1) {
      errSprintf ("ERROR IS1 not labeled correctly. %ld\n", is1[4]);
      return -2;
   }
   meta->center = (unsigned short int) is1[5];
   meta->subcenter = (unsigned short int) is1[7];
   meta->pds2.mstrVersion = (uChar) is1[9];
   meta->pds2.lclVersion = (uChar) is1[10];
   if (((meta->pds2.mstrVersion < 1) || (meta->pds2.mstrVersion > 5)) ||
       (meta->pds2.lclVersion > 1)) {
      if (meta->pds2.mstrVersion == 0) {
         printf ("Warning: Master table version == 0, was experimental\n"
                 "I don't have a copy, and don't know where to get one\n"
                 "Use meta data at your own risk.\n");
      } else if (meta->pds2.mstrVersion != 255) {
         printf ("Warning: use meta data at your own risk.\n");
         printf ("Supported master table versions: (1,2,3,4,5) yours is %u... ",
                 meta->pds2.mstrVersion);
         printf ("Supported local table version supported (0,1) yours is %u...\n",
                 meta->pds2.lclVersion);
      }
   }
   meta->pds2.sigTime = (uChar) is1[11];
   if (ParseTime (&(meta->pds2.refTime), is1[12], is1[14], is1[15], is1[16],
                  is1[17], is1[18]) != 0) {
      preErrSprintf ("Error in call to ParseTime from ParseSect1 (GRIB2)");
      return -2;
   }
   meta->pds2.operStatus = (uChar) is1[19];
   meta->pds2.dataType = (uChar) is1[20];
   return 0;
}

/*****************************************************************************
 * ParseSect2_Wx() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *    To verify and parse section 2 data when we know the variable is of type
 * Wx (Weather).
 *
 * ARGUMENTS
 *    rdat = The float data in section 2. (Input)
 *   nrdat = Length of rdat. (Input)
 *    idat = The integer data in section 2. (Input)
 *   nidat = Length of idat. (Input)
 *      Wx = The weather structure to fill. (Output)
 * simpVer = The version of the simple weather code to use when parsing the
 *           WxString. (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = nrdat or nidat is too small.
 * -2 = unexpected values in rdat.
 *
 * HISTORY
 *   2/2003 Arthur Taylor (MDL/RSIS): Created.
 *   5/2003 AAT: Stopped messing around with the way buffer and data[i]
 *          were allocated.  It was confusing the free routine.
 *   5/2003 AAT: Added maxLen to Wx structure.
 *   6/2003 AAT: Revisited after Matt (matt at wunderground.com) informed me of
 *          memory problems.
 *          1) I had a memory leak caused by a buffer+= buffLen
 *          2) buffLen could have increased out of bounds of buffer.
 *   8/2003 AAT: Found an invalid "assertion" when dealing with non-NULL
 *          terminated weather groups.
 *
 * NOTES
 * 1) May want to rewrite so that we don't need 'meta->sect2NumGroups'
 *****************************************************************************
 */
static int ParseSect2_Wx (float *rdat, sInt4 nrdat, sInt4 *idat,
                          uInt4 nidat, sect2_WxType *Wx, int simpVer)
{
   size_t loc;          /* Where we currently are in idat. */
   size_t groupLen;     /* Length of current group in idat. */
   size_t j;            /* Counter over the length of the current group. */
   char *buffer;        /* Used to store the current "ugly" string. */
   int buffLen;         /* Length of current "ugly" string. */
   int len;             /* length of current English phrases during creation
                         * of the maxEng[] data. */
   int i;               /* assists in traversing the maxEng[] array. */

   if (nrdat < 1) {
      return -1;
   }

   if (rdat[0] != 0) {
      errSprintf ("ERROR: Expected rdat to be empty when dealing with "
                  "section 2 Weather data\n");
      return -2;
   }
   Wx->dataLen = 0;
   Wx->data = nullptr;
   Wx->maxLen = 0;
   for (i = 0; i < NUM_UGLY_WORD; i++) {
      Wx->maxEng[i] = 0;
   }

   loc = 0;
   if (nidat == 0) {
      errSprintf ("ERROR: Ran out of idat data\n");
      return -1;
   }
   groupLen = idat[loc++];

   loc++;               /* Skip the decimal scale factor data. */
   /* Note: This also assures that buffLen stays <= nidat. */
   if (loc + groupLen >= nidat) {
      errSprintf ("ERROR: Ran out of idat data\n");
      return -1;
   }

   buffLen = 0;
   buffer = (char *) malloc ((nidat + 1) * sizeof (char));
   if( !buffer ) {
       errSprintf ("Out of memory\n");
       return -1;
   }
   while (groupLen > 0) {
      for (j = 0; j < groupLen; j++) {
         buffer[buffLen] = (char) idat[loc];
         buffLen++;
         loc++;
         if (buffer[buffLen - 1] == '\0') {
            Wx->dataLen++;
            Wx->data = (char **) realloc ((void *) Wx->data,
                                          Wx->dataLen * sizeof (char *));
            /* This is done after the realloc, just to make sure we have
             * enough memory allocated.  */
            /* Assert: buffLen is 1 more than strlen(buffer). */
            Wx->data[Wx->dataLen - 1] = (char *)
                  malloc (buffLen * sizeof (char));
            strcpy (Wx->data[Wx->dataLen - 1], buffer);
            if (Wx->maxLen < buffLen) {
               Wx->maxLen = buffLen;
            }
            buffLen = 0;
         }
      }
      if (loc >= nidat) {
         groupLen = 0;
      } else {
         groupLen = idat[loc];
         loc++;
         if (groupLen != 0) {
            loc++;      /* Skip the decimal scale factor data. */
            /* Note: This also assures that buffLen stays <= nidat. */
            if (loc + groupLen >= nidat) {
               errSprintf ("ERROR: Ran out of idat data\n");
               free (buffer);
               return -1;
            }
         }
      }
   }
   if (buffLen != 0) {
      buffer[buffLen] = '\0';
      Wx->dataLen++;
      Wx->data = (char **) realloc ((void *) Wx->data,
                                    Wx->dataLen * sizeof (char *));
      /* Assert: buffLen is 1 more than strlen(buffer). -- FALSE -- */
      buffLen = static_cast<int>(strlen (buffer)) + 1;

      Wx->data[Wx->dataLen - 1] = (char *) malloc (buffLen * sizeof (char));
      if (Wx->maxLen < buffLen) {
         Wx->maxLen = buffLen;
      }
      strcpy (Wx->data[Wx->dataLen - 1], buffer);
   }
   free (buffer);
   Wx->ugly = (UglyStringType *) malloc (Wx->dataLen *
                                         sizeof (UglyStringType));
   Wx->f_valid = (uChar *) malloc (Wx->dataLen * sizeof (uChar));
   for (j = 0; j < Wx->dataLen; j++) {
      if (ParseUglyString (&(Wx->ugly[j]), Wx->data[j], simpVer) == 0) {
         Wx->f_valid[j] = 1;
      } else {
         Wx->f_valid[j] = 0;
      }
   }
   /* We want to know how many bytes we need for each English phrase column,
    * so we walk through each column calculating that value. */
   for (i = 0; i < NUM_UGLY_WORD; i++) {
      /* Assert: Already initialized Wx->maxEng[i]. */
      for (j = 0; j < Wx->dataLen; j++) {
         if (Wx->ugly[j].english[i] != nullptr) {
            len = static_cast<int>(strlen (Wx->ugly[j].english[i]));
            if (len > Wx->maxEng[i]) {
               Wx->maxEng[i] = len;
            }
         }
      }
   }
   return 0;
}

static int ParseSect2_Hazard (float *rdat, sInt4 nrdat, sInt4 *idat,
                          uInt4 nidat, sect2_HazardType *Hazard, int simpWWA)
{
   size_t loc;          /* Where we currently are in idat. */
   size_t groupLen;     /* Length of current group in idat. */
   size_t j;            /* Counter over the length of the current group. */
   int len;             /* length of current english phrases during creation
                         * of the maxEng[] data. */
   int i;               /* assists in traversing the maxEng[] array. */
   char *buffer;        /* Used to store the current Hazard string. */
   int buffLen;         /* Length of current Hazard string. */
/*
   int k;
*/

   if (nrdat < 1) {
      return -1;
   }

   if (rdat[0] != 0) {
      errSprintf ("ERROR: Expected rdat to be empty when dealing with "
                  "section 2 Weather data\n");
      return -2;
   }
   Hazard->dataLen = 0;
   Hazard->data = nullptr;
   Hazard->maxLen = 0;
   for (j = 0; j < NUM_HAZARD_WORD; j++) {
      Hazard->maxEng[j] = 0;
   }

   loc = 0;
   if (nidat == 0) {
      errSprintf ("ERROR: Ran out of idat data\n");
      return -1;
   }
   groupLen = idat[loc++];

   loc++;               /* Skip the decimal scale factor data. */
   /* Note: This also assures that buffLen stays <= nidat. */
   if (loc + groupLen >= nidat) {
      errSprintf ("ERROR: Ran out of idat data\n");
      return -1;
   }

   buffLen = 0;
   buffer = (char *) malloc ((nidat + 1) * sizeof (char));
   if (!buffer) {
       errSprintf ("Out of memory\n");
       return -1;
   }
   while (groupLen > 0) {
      for (j = 0; j < groupLen; j++) {
         buffer[buffLen] = (char) idat[loc];
         buffLen++;
         loc++;
         if (buffer[buffLen - 1] == '\0') {
            Hazard->dataLen++;
            Hazard->data = (char **) realloc ((void *) Hazard->data,
                                          Hazard->dataLen * sizeof (char *));
            /* This is done after the realloc, just to make sure we have
             * enough memory allocated.  */
            /* Assert: buffLen is 1 more than strlen(buffer). */
            Hazard->data[Hazard->dataLen - 1] = (char *)
                  malloc (buffLen * sizeof (char));
            strcpy (Hazard->data[Hazard->dataLen - 1], buffer);
            if (Hazard->maxLen < buffLen) {
               Hazard->maxLen = buffLen;
            }
            buffLen = 0;
         }
      }
      if (loc >= nidat) {
         groupLen = 0;
      } else {
         groupLen = idat[loc];
         loc++;
         if (groupLen != 0) {
            loc++;      /* Skip the decimal scale factor data. */
            /* Note: This also assures that buffLen stays <= nidat. */
            if (loc + groupLen >= nidat) {
               errSprintf ("ERROR: Ran out of idat data\n");
               free (buffer);
               return -1;
            }
         }
      }
   }
   if (buffLen != 0) {
      buffer[buffLen] = '\0';
      Hazard->dataLen++;
      Hazard->data = (char **) realloc ((void *) Hazard->data,
                                    Hazard->dataLen * sizeof (char *));
      /* Assert: buffLen is 1 more than strlen(buffer). -- FALSE -- */
      buffLen = static_cast<int>(strlen (buffer)) + 1;

      Hazard->data[Hazard->dataLen - 1] = (char *) malloc (buffLen * sizeof (char));
      if (Hazard->maxLen < buffLen) {
         Hazard->maxLen = buffLen;
      }
      strcpy (Hazard->data[Hazard->dataLen - 1], buffer);
   }
   free (buffer);
   Hazard->haz = (HazardStringType *) malloc (Hazard->dataLen *
                                         sizeof (HazardStringType));
   Hazard->f_valid = (uChar *) malloc (Hazard->dataLen * sizeof (uChar));
   for (j = 0; j < Hazard->dataLen; j++) {
      ParseHazardString (&(Hazard->haz[j]), Hazard->data[j], simpWWA);
      Hazard->f_valid[j] = 1;
/*
      printf ("%d : %d : %s", j, Hazard->haz[j].numValid, Hazard->data[j]);
      for (k = 0; k < Hazard->haz[j].numValid; k++) {
         printf (": %s", Hazard->haz[j].english[k]);
      }
      printf ("\n");
*/
   }
   /* We want to know how many bytes we need for each english phrase column,
    * so we walk through each column calculating that value. */
   for (i = 0; i < NUM_HAZARD_WORD; i++) {
      /* Assert: Already initialized Hazard->maxEng[i]. */
      for (j = 0; j < Hazard->dataLen; j++) {
         if (Hazard->haz[j].english[i] != nullptr) {
            len = static_cast<int>(strlen (Hazard->haz[j].english[i]));
            if (len > Hazard->maxEng[i]) {
               Hazard->maxEng[i] = len;
            }
         }
      }
   }
   return 0;
}

/*****************************************************************************
 * ParseSect2_Unknown() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *    To verify and parse section 2 data when we don't know anything more
 * about the data.
 *
 * ARGUMENTS
 *  rdat = The float data in section 2. (Input)
 * nrdat = Length of rdat. (Input)
 *  idat = The integer data in section 2. (Input)
 * nidat = Length of idat. (Input)
 *  meta = The structure to fill. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = nrdat or nidat is too small.
 * -2 = unexpected values in rdat.
 *
 * HISTORY
 *   2/2003 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 *   In the extremely improbable case that there is both idat data and rdat
 *   data, we process the rdat data first.
 *****************************************************************************
 */
static int ParseSect2_Unknown (float *rdat, sInt4 nrdat, sInt4 *idat,
                               sInt4 nidat, grib_MetaData *meta)
{
   /* Used for easier access to answer. */
   int loc;             /* Where we currently are in idat. */
   int ansLoc;          /* Where we are in the answer data structure. */
   sInt4 groupLen;      /* Length of current group in idat. */
   int j;               /* Counter over the length of the current group. */

   meta->pds2.sect2.unknown.dataLen = 0;
   meta->pds2.sect2.unknown.data = nullptr;
   ansLoc = 0;

   /* Work with rdat data. */
   loc = 0;
   if (nrdat <= loc) {
      errSprintf ("ERROR: Ran out of rdat data\n");
      return -1;
   }
   groupLen = (sInt4) rdat[loc++];
   loc++;               /* Skip the decimal scale factor data. */
   if (nrdat <= loc + groupLen) {
      errSprintf ("ERROR: Ran out of rdat data\n");
      return -1;
   }
   while (groupLen > 0) {
      meta->pds2.sect2.unknown.dataLen += groupLen;
      meta->pds2.sect2.unknown.data = (double *)
            realloc ((void *) meta->pds2.sect2.unknown.data,
                     meta->pds2.sect2.unknown.dataLen * sizeof (double));
      for (j = 0; j < groupLen; j++) {
         meta->pds2.sect2.unknown.data[ansLoc++] = rdat[loc++];
      }
      if (nrdat <= loc) {
         groupLen = 0;
      } else {
         groupLen = (sInt4) rdat[loc++];
         if (groupLen != 0) {
            loc++;      /* Skip the decimal scale factor data. */
            if (nrdat <= loc + groupLen) {
               errSprintf ("ERROR: Ran out of rdat data\n");
               return -1;
            }
         }
      }
   }

   /* Work with idat data. */
   loc = 0;
   if (nidat <= loc) {
      errSprintf ("ERROR: Ran out of idat data\n");
      return -1;
   }
   groupLen = idat[loc++];
   loc++;               /* Skip the decimal scale factor data. */
   if (nidat <= loc + groupLen) {
      errSprintf ("ERROR: Ran out of idat data\n");
      return -1;
   }
   while (groupLen > 0) {
      meta->pds2.sect2.unknown.dataLen += groupLen;
      meta->pds2.sect2.unknown.data = (double *)
            realloc ((void *) meta->pds2.sect2.unknown.data,
                     meta->pds2.sect2.unknown.dataLen * sizeof (double));
      for (j = 0; j < groupLen; j++) {
         meta->pds2.sect2.unknown.data[ansLoc++] = idat[loc++];
      }
      if (nidat <= loc) {
         groupLen = 0;
      } else {
         groupLen = idat[loc++];
         if (groupLen != 0) {
            loc++;      /* Skip the decimal scale factor data. */
            if (nidat <= loc + groupLen) {
               errSprintf ("ERROR: Ran out of idat data\n");
               return -1;
            }
         }
      }
   }
   return 0;
}

/*****************************************************************************
 * ParseSect3() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To verify and parse section 3 data.
 *
 * ARGUMENTS
 *  is3 = The unpacked section 3 array. (Input)
 *  ns3 = The size of section 3. (Input)
 * meta = The structure to fill. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = ns3 is too small.
 * -2 = unexpected values in is3.
 * -3 = un-supported map Projection.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *   9/2003 AAT: Adjusted Radius Earth case 1,6 to be based on:
 *          Y * 10^D = R
 *          Where Y = original value, D is scale factor, R is scale value.
 *   1/2004 AAT: Adjusted Radius Earth case 6 to always be 6371.229 km
 *
 * NOTES
 * Need to add support for GS3_ORTHOGRAPHIC = 90,
 *   GS3_EQUATOR_EQUIDIST = 110, GS3_AZIMUTH_RANGE = 120
 *****************************************************************************
 */
static int ParseSect3 (sInt4 *is3, sInt4 ns3, grib_MetaData *meta)
{
   double unit;         /* Used to convert from stored value to degrees
                         * lat/lon. See GRIB2 Regulation 92.1.6 */
   sInt4 angle;         /* For Lat/Lon, 92.1.6 may not hold, in which case,
                         * angle != 0, and unit = angle/subdivision. */
   sInt4 subdivision;   /* see angle explanation. */
   int ret = 0;

   if (ns3 < 14) {
      return -1;
   }
   if (is3[4] != 3) {
      errSprintf ("ERROR IS3 not labeled correctly. %ld\n", is3[4]);
      return -2;
   }
   if (is3[5] != 0) {
      errSprintf ("Can not handle 'Source of Grid Definition' = %ld\n",
                  is3[5]);
      errSprintf ("Can only handle grids defined in Code table 3.1\n");
      // return -3;
   }
   meta->gds.numPts = is3[6];
   if ((is3[10] != 0) || (is3[11] != 0)) {
      errSprintf ("Un-supported Map Projection.\n  All Supported "
                  "projections have 0 bytes following the template.\n");
      // return -3;
   }
   meta->gds.projType = (uChar) is3[12];

   // Do not refuse to convert the GRIB file if only the projection is unknown.

   /*
   if ((is3[12] != GS3_LATLON) && (is3[12] != GS3_MERCATOR) &&
       (is3[12] != GS3_POLAR) && (is3[12] != GS3_LAMBERT)) {
      errSprintf ("Un-supported Map Projection %ld\n", is3[12]);
      return -3;
   }
	 */

   /*
    * Handle variables common to the supported templates.
    */
   if (ns3 < 38) {
      return -1;
   }
   /* Assert: is3[14] is the shape of the earth. */
   meta->gds.hdatum = 0;
   switch (is3[14]) {
      case 0:
         meta->gds.f_sphere = 1;
         meta->gds.majEarth = 6367.47;
         meta->gds.minEarth = 6367.47;
         break;
      case 6:
         meta->gds.f_sphere = 1;
         meta->gds.majEarth = 6371.229;
         meta->gds.minEarth = 6371.229;
         break;
      case 1:
         meta->gds.f_sphere = 1;
         /* Following assumes scale factor and scale value refer to
          * scientific notation. */
         /* Incorrect Assumption (9/8/2003): scale factor / value are based
          * on: Y * 10^D = R, where Y = original value, D = scale factor, ___
          * R = scale value. */

         // File of https://github.com/OSGeo/gdal/issues/7811
         // has is3[16] == -1 and is3[15] = 255
         if (is3[16] > 0 && is3[15] != 255 &&
             (is3[16] != GRIB2MISSING_s4) && (is3[15] != GRIB2MISSING_s1)) {
            /* Assumes data is given in m (not km). */
            double denom = pow (10.0, is3[15]) * 1000.;
            if( denom == 0 )
            {
                errSprintf ("Invalid radius.\n");
                ret = -2;
            }
            else
            {
                meta->gds.majEarth = is3[16] / denom;
                meta->gds.minEarth = meta->gds.majEarth;
            }
         } else {
            errSprintf ("Missing info on radius of Earth.\n");
            ret = -2;
         }
         /* Check if our m assumption was valid. If it was not, they give us
          * 6371 km, which we convert to 6.371 < 6.4 */
         if (ret == 0 && meta->gds.majEarth < 6.4) {
            meta->gds.majEarth = meta->gds.majEarth * 1000.;
            meta->gds.minEarth = meta->gds.minEarth * 1000.;
         }
         break;
      case 2:
         meta->gds.f_sphere = 0;
         meta->gds.majEarth = 6378.160;
         meta->gds.minEarth = 6356.775;
         break;
      case 4: // GRS80
         meta->gds.f_sphere = 0;
         meta->gds.majEarth = 6378.137;
         meta->gds.minEarth = meta->gds.majEarth * (1 - 1 / 298.257222101);
         break;
      case 5: // WGS84
         meta->gds.f_sphere = 0;
         meta->gds.majEarth = 6378.137;
         meta->gds.minEarth = meta->gds.majEarth * (1 - 1 / 298.257223563);
         break;
      case 3:
         meta->gds.f_sphere = 0;
         /* Following assumes scale factor and scale value refer to
          * scientific notation. */
         /* Incorrect Assumption (9/8/2003): scale factor / value are based
          * on: Y * 10^D = R, where Y = original value, D = scale factor, ___
          * R = scale value. */
         if ((is3[21] != GRIB2MISSING_s4) && (is3[20] != GRIB2MISSING_s1) &&
             (is3[26] != GRIB2MISSING_s4) && (is3[25] != GRIB2MISSING_s1)) {
            /* Assumes data is given in km (not m). */
            double denomMaj = pow (10.0, is3[20]);
            double denomMin = pow (10.0, is3[25]);
            if( denomMaj == 0.0 || denomMin == 0.0 )
            {
                errSprintf ("Invalid major / minor axis.\n");
                ret = -2;
            }
            else
            {
                meta->gds.majEarth = is3[21] / denomMaj;
                meta->gds.minEarth = is3[26] / denomMin;
            }
         } else {
            errSprintf ("Missing info on major / minor axis of Earth.\n");
            ret = -2;
         }
         /* Check if our km assumption was valid. If it was not, they give us
          * 6371000 m, which is > 6400. */
         if (meta->gds.majEarth > 6400) {
            meta->gds.majEarth = meta->gds.majEarth / 1000.;
         }
         if (meta->gds.minEarth > 6400) {
            meta->gds.minEarth = meta->gds.minEarth / 1000.;
         }
         break;
      case 7:
         meta->gds.f_sphere = 0;
         /* Following assumes scale factor and scale value refer to
          * scientific notation. */
         /* Incorrect Assumption (9/8/2003): scale factor / value are based
          * on: Y * 10^D = R, where Y = original value, D = scale factor, ___
          * R = scale value. */
         if ((is3[21] != GRIB2MISSING_s4) && (is3[20] != GRIB2MISSING_s1) &&
             (is3[26] != GRIB2MISSING_s4) && (is3[25] != GRIB2MISSING_s1)) {
            /* Assumes data is given in m (not km). */
            double denomMaj = pow (10.0, is3[20]) * 1000.;
            double denomMin = pow (10.0, is3[25]) * 1000.;
            if( denomMaj == 0.0 || denomMin == 0.0 )
            {
                errSprintf ("Invalid major / minor axis.\n");
                ret = -2;
            }
            else
            {
                meta->gds.majEarth = is3[21] / denomMaj;
                meta->gds.minEarth = is3[26] / denomMin;
            }
         } else {
            errSprintf ("Missing info on major / minor axis of Earth.\n");
            ret = -2;
         }
         /* Check if our m assumption was valid. If it was not, they give us
          * 6371 km, which we convert to 6.371 < 6.4 */
         if (meta->gds.majEarth < 6.4) {
            meta->gds.majEarth = meta->gds.majEarth * 1000.;
         }
         if (meta->gds.minEarth < 6.4) {
            meta->gds.minEarth = meta->gds.minEarth * 1000.;
         }
         break;
      case 8:
         meta->gds.f_sphere = 1;
         meta->gds.majEarth = 6371.2;
         meta->gds.minEarth = 6371.2;
         meta->gds.hdatum = 1;
         break;
      default:
         errSprintf ("Undefined shape of earth? %ld\n", is3[14]);
         return -2;
   }
   /* Validate the radEarth is reasonable. */
   if ((meta->gds.majEarth > 6400) || (meta->gds.majEarth < 6300) ||
       (meta->gds.minEarth > 6400) || (meta->gds.minEarth < 6300)) {
      errSprintf ("Bad shape of earth? %f %f\n", meta->gds.majEarth,
                  meta->gds.minEarth);
      meta->gds.majEarth = -1;
      meta->gds.minEarth = -1;
      ret = -2;
   }
   meta->gds.Nx = is3[30];
   meta->gds.Ny = is3[34];
   if ((meta->gds.Nx != 0 && meta->gds.Ny > UINT_MAX / meta->gds.Nx) ||
       meta->gds.Nx * meta->gds.Ny != meta->gds.numPts) {
      errSprintf ("Nx * Ny != number of points?\n");
      return -2;
   }

   /* Initialize variables prior to parsing the specific templates. */
   unit = 1e-6;
   meta->gds.center = 0;
   meta->gds.scaleLat1 = meta->gds.scaleLat2 = 0;
   meta->gds.southLat = meta->gds.southLon = 0;
   meta->gds.lat2 = meta->gds.lon2 = 0;
   switch (is3[12]) {
      case GS3_LATLON: /* 0: Regular lat/lon grid. */
      case GS3_ROTATED_LATLON: // 1: Rotated lat/lon grid
      case GS3_GAUSSIAN_LATLON:  /* 40: Gaussian lat/lon grid. */
         if (ns3 < 72) {
            return -1;
         }
         angle = is3[38];
         subdivision = is3[42];
         if (angle != 0) {
            if (subdivision == 0) {
               errSprintf ("subdivision of 0? Could not determine unit"
                           " for latlon grid\n");
               return -2;
            }
            unit = angle / (double) (subdivision);
         }
         if ((is3[46] == GRIB2MISSING_s4) || (is3[50] == GRIB2MISSING_s4) ||
             (is3[55] == GRIB2MISSING_s4) || (is3[59] == GRIB2MISSING_s4) ||
             (is3[63] == GRIB2MISSING_s4) || (is3[67] == GRIB2MISSING_s4)) {
            errSprintf ("Lat/Lon grid is not defined completely.\n");
            return -2;
         }
         meta->gds.lat1 = is3[46] * unit;
         meta->gds.lon1 = is3[50] * unit;
         meta->gds.resFlag = (uChar) is3[54];
         meta->gds.lat2 = is3[55] * unit;
         meta->gds.lon2 = is3[59] * unit;
         meta->gds.Dx = is3[63] * unit; /* degrees. */
         if (is3[12] == GS3_GAUSSIAN_LATLON) {
            int np = is3[67]; /* parallels between a pole and the equator */
            if( np == 0 )
            {
                errSprintf ("Gaussian Lat/Lon grid is not defined completely.\n");
                return -2;
            }
            meta->gds.Dy = 90.0 / np;
         } else
            meta->gds.Dy = is3[67] * unit; /* degrees. */
         meta->gds.scan = (uChar) is3[71];
         meta->gds.meshLat = 0;
         meta->gds.orientLon = 0;
         if( is3[12] == GS3_ROTATED_LATLON ) {
             if( ns3 < 84 ) {
                 return -1;
             }
             meta->gds.f_typeLatLon = 3;
             meta->gds.southLat = is3[73-1] * unit;
             meta->gds.southLon = is3[77-1] * unit;
             meta->gds.angleRotate = is3[81-1] * unit;
         }
         /* Resolve resolution flag(bit 3,4).  Copy Dx,Dy as appropriate. */
         if ((meta->gds.resFlag & GRIB2BIT_3) &&
             (!(meta->gds.resFlag & GRIB2BIT_4))) {
            meta->gds.Dy = meta->gds.Dx;
         } else if ((!(meta->gds.resFlag & GRIB2BIT_3)) &&
                    (meta->gds.resFlag & GRIB2BIT_4)) {
            meta->gds.Dx = meta->gds.Dy;
         }
         break;
      case GS3_MERCATOR: /* 10: Mercator grid. */
         if (ns3 < 72) {
            return -1;
         }
         if ((is3[38] == GRIB2MISSING_s4) || (is3[42] == GRIB2MISSING_s4) ||
             (is3[47] == GRIB2MISSING_s4) || (is3[51] == GRIB2MISSING_s4) ||
             (is3[55] == GRIB2MISSING_s4) || (is3[60] == GRIB2MISSING_s4)) {
            errSprintf ("Mercator grid is not defined completely.\n");
            return -2;
         }
         meta->gds.lat1 = is3[38] * unit;
         meta->gds.lon1 = is3[42] * unit;
         meta->gds.resFlag = (uChar) is3[46];
         meta->gds.meshLat = is3[47] * unit;
         meta->gds.lat2 = is3[51] * unit;
         meta->gds.lon2 = is3[55] * unit;
         meta->gds.scan = (uChar) is3[59];
         meta->gds.orientLon = is3[60] * unit;
         meta->gds.Dx = is3[64] / 1000.; /* mm -> m */
         meta->gds.Dy = is3[68] / 1000.; /* mm -> m */
         /* Resolve resolution flag(bit 3,4).  Copy Dx,Dy as appropriate. */
         if ((meta->gds.resFlag & GRIB2BIT_3) &&
             (!(meta->gds.resFlag & GRIB2BIT_4))) {
            if (is3[64] == GRIB2MISSING_s4) {
               errSprintf ("Mercator grid is not defined completely.\n");
               return -2;
            }
            meta->gds.Dy = meta->gds.Dx;
         } else if ((!(meta->gds.resFlag & GRIB2BIT_3)) &&
                    (meta->gds.resFlag & GRIB2BIT_4)) {
            if (is3[68] == GRIB2MISSING_s4) {
               errSprintf ("Mercator grid is not defined completely.\n");
               return -2;
            }
            meta->gds.Dx = meta->gds.Dy;
         }
         break;
      case GS3_TRANSVERSE_MERCATOR: /* 12: Transverse mercator */
         if (ns3 < 84) {
            return -1;
         }
         meta->gds.latitude_of_origin = is3[38] * unit;
         meta->gds.central_meridian = is3[42] * unit;
         meta->gds.resFlag = (uChar) is3[46];
         {
             float fTemp;
             GUInt32 nTemp = is3[47] < 0 ? (-is3[47]) | 0x80000000U : is3[47];
             memcpy(&fTemp, &nTemp, 4);
             meta->gds.scaleLat1 = fTemp;
         }
         meta->gds.x0 = is3[51] / 100.0;
         meta->gds.y0 = is3[55] / 100.0;
         meta->gds.scan = (uChar) is3[59];
         meta->gds.Dx = is3[60] / 100.0;
         meta->gds.Dy = is3[64] / 100.0;
         meta->gds.x1 = is3[68] / 100.0;
         meta->gds.y1 = is3[72] / 100.0;
         meta->gds.x2 = is3[76] / 100.0;
         meta->gds.y2 = is3[80] / 100.0;
         break;

      case GS3_POLAR:  /* 20: Polar Stereographic grid. */
         if (ns3 < 65) {
            return -1;
         }
         if ((is3[38] == GRIB2MISSING_s4) || (is3[42] == GRIB2MISSING_s4) ||
             (is3[47] == GRIB2MISSING_s4) || (is3[51] == GRIB2MISSING_s4)) {
            errSprintf ("Polar Stereographic grid is not defined "
                        "completely.\n");
            return -2;
         }
         meta->gds.lat1 = is3[38] * unit;
         meta->gds.lon1 = is3[42] * unit;
         meta->gds.resFlag = (uChar) is3[46];
         /* Note (1) resFlag (bit 3,4) not applicable. */
         meta->gds.meshLat = is3[47] * unit;
         meta->gds.orientLon = is3[51] * unit;
         meta->gds.Dx = is3[55] / 1000.; /* mm -> m */
         meta->gds.Dy = is3[59] / 1000.; /* mm -> m */
         meta->gds.center = (uChar) is3[63];
         if (meta->gds.center & GRIB2BIT_1) {
            /* South polar stereographic. */
            meta->gds.scaleLat1 = meta->gds.scaleLat2 = -90;
         } else {
            /* North polar stereographic. */
            meta->gds.scaleLat1 = meta->gds.scaleLat2 = 90;
         }
         if (meta->gds.center & GRIB2BIT_2) {
            errSprintf ("Note (4) specifies no 'bi-polar stereograhic"
                        " projections'.\n");
            return -2;
         }
         meta->gds.scan = (uChar) is3[64];
         break;
      case GS3_LAMBERT: /* 30: Lambert Conformal grid. */
      case GS3_ALBERS_EQUAL_AREA: /* 31: Albers equal area */
         if (ns3 < 81) {
            return -1;
         }
         if ((is3[38] == GRIB2MISSING_s4) || (is3[42] == GRIB2MISSING_s4) ||
             (is3[47] == GRIB2MISSING_s4) || (is3[51] == GRIB2MISSING_s4) ||
             (is3[65] == GRIB2MISSING_s4) || (is3[69] == GRIB2MISSING_s4)) {
            if( is3[12] == GS3_LAMBERT )
            {
                errSprintf ("Lambert Conformal grid is not defined "
                            "completely.\n");
            }
            else
            {
                errSprintf ("Albers Equal Area grid is not defined "
                            "completely.\n");
            }
            return -2;
         }
         meta->gds.lat1 = is3[38] * unit;
         meta->gds.lon1 = is3[42] * unit;
         meta->gds.resFlag = (uChar) is3[46];
         /* Note (3) resFlag (bit 3,4) not applicable. */
         meta->gds.meshLat = is3[47] * unit;
         meta->gds.orientLon = is3[51] * unit;
         meta->gds.Dx = is3[55] / 1000.; /* mm -> m */
         meta->gds.Dy = is3[59] / 1000.; /* mm -> m */
         meta->gds.center = (uChar) is3[63];
         meta->gds.scan = (uChar) is3[64];
         meta->gds.scaleLat1 = is3[65] * unit;
         meta->gds.scaleLat2 = is3[69] * unit;
         if( (is3[73] == GRIB2MISSING_s4) || (is3[77] == GRIB2MISSING_s4) )
         {
             meta->gds.southLat = 0.0;
             meta->gds.southLon = 0.0;
         }
         else
         {
            meta->gds.southLat = is3[73] * unit;
            meta->gds.southLon = is3[77] * unit;
         }
         break;
    case GS3_ORTHOGRAPHIC: /* 90: Orthographic grid. */
				 // Misusing gdsType elements (gdsType needs extension)
         meta->gds.lat1 = is3[38];
         meta->gds.lon1 = is3[42];
         meta->gds.resFlag = (uChar) is3[46];
         meta->gds.Dx = is3[47];
         meta->gds.Dy = is3[51];

         meta->gds.lon2 = is3[55] / 1000.; /* xp - X-coordinateSub-satellite, mm -> m */
         meta->gds.lat2 = is3[59] / 1000.; /* yp - Y-coordinateSub-satellite, mm -> m */
         meta->gds.scan = (uChar) is3[63];
				 meta->gds.orientLon = is3[64]; /* angle */
				 meta->gds.stretchFactor = is3[68] * 1000000.; /* altitude */

         meta->gds.southLon = is3[72]; /* x0 - X-coordinateOrigin */
         meta->gds.southLat = is3[76]; /* y0 - Y-coordinateOrigin */
         break;
    case GS3_LAMBERT_AZIMUTHAL: /* 140: Lambert Azimuthal Equal Area Projection */
         meta->gds.lat1 = is3[38] * unit;
         meta->gds.lon1 = is3[42] * unit;
         meta->gds.meshLat = is3[46] * unit;
         meta->gds.orientLon = is3[50] * unit;
         meta->gds.resFlag = (uChar) is3[54];
         meta->gds.Dx = is3[55] / 1000.; /* mm -> m */
         meta->gds.Dy = is3[59] / 1000.; /* mm -> m */
         meta->gds.scan = (uChar) is3[63];
         break;
      default:
         errSprintf ("Un-supported Map Projection. %ld\n", is3[12]);
				 // Don't abandon the conversion only because of an unknown projection
				 break;
         //return -3;
   }
   if (meta->gds.scan != GRIB2BIT_2) {
#ifdef DEBUG
      printf ("Scan mode is expected to be 0100 (i.e. %d) not %u\n",
              GRIB2BIT_2, meta->gds.scan);
      printf ("The merged GRIB2 Library should return it in 0100\n");
      printf ("The merged library swaps both NCEP and MDL data to scan "
              "mode 0100\n");
#endif
/*
      errSprintf ("Scan mode is expected to be 0100 (i.e. %d) not %d",
                  GRIB2BIT_2, meta->gds.scan);
      return -2;
*/
   }
   return ret;
}

/*****************************************************************************
 * ParseSect4Time2secV1() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *    Attempt to parse time data in units provided by GRIB1 table 4, to
 * seconds.
 *
 * ARGUMENTS
 * time = The delta time to convert. (Input)
 * unit = The unit to convert. (Input)
 *  ans = The converted answer. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int
 *  0 = OK
 * -1 = could not determine.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *   1/2005 AAT: Fixed unit2sec[] table to have element 10 be 10800 (3 hours)
 *          instead of 0.
 *
 * NOTES
 * http://www.nco.ncep.noaa.gov/pmb/docs/on388/table4.html
 *****************************************************************************
 */
int ParseSect4Time2secV1 (sInt4 time, int unit, double *ans)
{
   /* Following is a lookup table for unit conversion (see code table 4.4). */
   static const sInt4 unit2sec[] = {
      60, 3600, 86400L, 0, 0,
      0, 0, 0, 0, 0,
      10800, 21600L, 43200L
   };
   if ((unit >= 0) && (unit < 13)) {
      if (unit2sec[unit] != 0) {
         *ans = (double) (time) * unit2sec[unit];
         return 0;
      }
   } else if (unit == 254) {
      *ans = (double) (time);
      return 0;
   }
   *ans = 0;
   return -1;
}

/*****************************************************************************
 * ParseSect4Time2sec() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *    Attempt to parse time data in units provided by GRIB2 table 4.4, to
 * seconds.
 *
 * ARGUMENTS
 * refTime = To add "years / centuries / decades and normals", we need a
 *           reference time.
 *    delt = The delta time to convert. (Input)
 *    unit = The unit to convert. (Input)
 *     ans = The converted answer. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int
 *  0 = OK
 * -1 = could not determine.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *   1/2005 AAT: Fixed unit2sec[] table to have element 10 be 10800 (3 hours)
 *          instead of 0.
 *
 * NOTES
 *****************************************************************************
 */
int ParseSect4Time2sec (double refTime, sInt4 delt, int unit, double *ans)
{
   /* Following is a lookup table for unit conversion (see code table 4.4). */
   static const sInt4 unit2sec[] = {
      60, 3600, 86400L, 0, 0,
      0, 0, 0, 0, 0,
      10800, 21600L, 43200L, 1
   };
   if ((unit >= 0) && (unit < 14)) {
      if (unit2sec[unit] != 0) {
         *ans = (double) (delt) * unit2sec[unit];
         return 0;
      } else {
         /* The procedure returns number of seconds to adjust by, rather
          * than the new time, which is why we subtract refTime */
         switch (unit) {
            case 3: /* month */
               *ans = Clock_AddMonthYear (refTime, delt, 0) - refTime;
               return 0;
            case 4: /* year */
               *ans = Clock_AddMonthYear (refTime, 0, delt) - refTime;
               return 0;
            case 5: /* decade */
               if( delt < INT_MIN / 10 || delt > INT_MAX / 10 )
                   return -1;
               *ans = Clock_AddMonthYear (refTime, 0, delt * 10) - refTime;
               return 0;
            case 6: /* normal (30 year) */
               if( delt < INT_MIN / 30 || delt > INT_MAX / 30 )
                   return -1;
               *ans = Clock_AddMonthYear (refTime, 0, delt * 30) - refTime;
               return 0;
            case 7: /* century (100 year) */
               if( delt < INT_MIN / 100 || delt > INT_MAX / 100 )
                   return -1;
               *ans = Clock_AddMonthYear (refTime, 0, delt * 100) - refTime;
               return 0;
         }
      }
   }
   *ans = 0;
   return -1;
}

/*****************************************************************************
 * sbit_2Comp_fourByte() -- Arthur Taylor / MDL
 *
 * PURPOSE
 *    The NCEP g2clib-1.0.2 library stored the lower limits and upper limits
 * of probabilities using unsigned ints, whereas version 1.0.4 used signed
 * ints.  The reason for the change is because some thresholds were negative.
 *    To encode a negative value using an unsigned int, 1.0.2 used "2's
 * complement + 1".  To encode a negative value using signed an int, 1.0.4
 * used a "sign bit".  Example -2 => FFFFFFFE (1.0.2) => 80000002 (1.0.4).
 * The problem (for backward compatibility sake) is to be able to read both
 * encodings and get -2.  If one only read the new encoding method, then
 * archived data would not be handled.
 *    The algorithm is: If the number is positive or missing, leave it alone.
 * If the number is negative, look at the 2's complement method, and the sign
 * bit method, and use the method which results in a smaller absolute value.
 *
 * ARGUMENTS
 * data = The number read by NCEP's library. (Input)
 *
 * RETURNS: sInt4
 *    The value of treating the number as read by either method
 *
 * HISTORY
 * 10/2007 Arthur Taylor (MDL): Created.
 *
 * NOTES
 * 1) This algorithm will impact the possible range of values, by reducing it
 *    from -2^31..(2^31-1) to -2^30..(2^31-1).
 * 2) The NCEP change also impacted large positive values.  One originally
 *    could encode 0..2^32-1.  Some confusion could arrise if the value was
 *    originally encoded by 1.0.2 was in the range of 2^31..2^32-1.
 ****************************************************************************/
sInt4 sbit_2Comp_fourByte(sInt4 data)
{
   sInt4 x;             /* The pos. 2's complement interpretation of data */
   sInt4 y;             /* The pos. sign bit interpretation of data */

   if ((data == GRIB2MISSING_s4) || (data >= 0)) {
      return data;
   }
   if( data == INT_MIN ) // doesn't make sense since it is negative 0 in sign bit logic
      return 0;
   x = ~data + 1;
   y = data & 0x7fffffff;
   if (x < y) {
      return -1 * x;
   } else {
      return -1 * y;
   }
}

/*****************************************************************************
 * sbit_2Comp_oneByte() -- Arthur Taylor / MDL
 *
 * PURPOSE
 *    The NCEP g2clib-1.0.2 library stored the lower limits and upper limits
 * of probabilities using unsigned ints, whereas version 1.0.4 used signed
 * ints.  The reason for the change is because some thresholds were negative.
 *    To encode a negative value using an unsigned int, 1.0.2 used "2's
 * complement + 1".  To encode a negative value using signed an int, 1.0.4
 * used a "sign bit".  Example -2 => 11111110 (1.0.2) => 10000010 (1.0.4).
 * The problem (for backward compatibility sake) is to be able to read both
 * encodings and get -2.  If one only read the new encoding method, then
 * archived data would not be handled.
 *    The algorithm is: If the number is positive or missing, leave it alone.
 * If the number is negative, look at the 2's complement method, and the sign
 * bit method, and use the method which results in a smaller absolute value.
 *
 * ARGUMENTS
 * data = The number read by NCEP's library. (Input)
 *
 * RETURNS: sChar
 *    The value of treating the number as read by either method
 *
 * HISTORY
 * 10/2007 Arthur Taylor (MDL): Created.
 *
 * NOTES
 * 1) This algorithm will impact the possible range of values, by reducing it
 *    from -128..127 to -64...127.
 * 2) The NCEP change also impacted large positive values.  One originally
 *    could encode 0..255.  Some confusion could arrise if the value was
 *    originally encoded by 1.0.2 was in the range of 128..255.
 ****************************************************************************/
sChar sbit_2Comp_oneByte(sChar data)
{
   sChar x;             /* The pos. 2's complement interpretation of data */
   sChar y;             /* The pos. sign bit interpretation of data */

   if ((data == GRIB2MISSING_s1) || (data >= 0)) {
      return data;
   }
   x = ~data + 1;
   y = data & 0x7f;
   if (x < y) {
      return -1 * x;
   } else {
      return -1 * y;
   }
}

/*****************************************************************************
 * ParseSect4() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To verify and parse section 4 data.
 *
 * ARGUMENTS
 *  is4 = The unpacked section 4 array. (Input)
 *  ns4 = The size of section 4. (Input)
 * meta = The structure to fill. (Output)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = ns4 is too small.
 * -2 = unexpected values in is4.
 * -4 = un-supported Sect 4 template.
 * -5 = unsupported forecast time unit.
 * -6 = Ran out of memory.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *   3/2003 AAT: Added support for GS4_SATELLITE.
 *   3/2003 AAT: Adjusted allocing of sect4.Interval (should be safer now).
 *   9/2003 AAT: Adjusted interpretation of scale factor / value.
 *   5/2004 AAT: Added some memory checks.
 *   3/2005 AAT: Added a cast to (uChar) when comparing to GRIB2MISSING_1
 *   3/2005 AAT: Added GS4_PROBABIL_PNT.
 *
 * NOTES
 * Need to add support for GS4_RADAR = 20
 *****************************************************************************
 */
static int ParseSect4 (sInt4 *is4, sInt4 ns4, grib_MetaData *meta)
{
   int i;               /* Counter for time intervals in template 4.8, 4.9
                         * (typically 1) or counter for satellite band in
                         * template 4.30. */
   void *temp_ptr;      /* A temporary pointer when reallocating memory. */
   char *msg;           /* A pointer to the current error message. */

   if (ns4 < 9) {
      return -1;
   }
   if (is4[4] != 4) {
#ifdef DEBUG
      printf ("ERROR IS4 not labeled correctly. %d\n", is4[4]);
#endif
      errSprintf ("ERROR IS4 not labeled correctly. %d\n", is4[4]);
      return -2;
   }

   if ((is4[7] != GS4_ANALYSIS) && (is4[7] != GS4_ENSEMBLE) &&
       (is4[7] != GS4_DERIVED) && (is4[7] != GS4_PROBABIL_PNT) &&
       (is4[7] != GS4_PERCENT_PNT) && (is4[7] != GS4_ERROR) &&
       (is4[7] != GS4_STATISTIC) && (is4[7] != GS4_PROBABIL_TIME) &&
       (is4[7] != GS4_PERCENT_TIME) && (is4[7] != GS4_ENSEMBLE_STAT) &&
       (is4[7] != GS4_SATELLITE) && (is4[7] != GS4_SATELLITE_SYNTHETIC) &&
       (is4[7] != GS4_DERIVED_INTERVAL) && (is4[7] != GS4_STATISTIC_SPATIAL_AREA) &&
       (is4[7] != GS4_ANALYSIS_CHEMICAL) && (is4[7] != GS4_OPTICAL_PROPERTIES_AEROSOL)) {
#ifdef DEBUG
      //printf ("Un-supported Template. %d\n", is4[7]);
#endif
      errSprintf ("Un-supported Template. %d\n", is4[7]);
      return -4;
   }
   meta->pds2.sect4.templat = (unsigned short int) is4[7];

   /*
    * Handle variables common to the supported templates.
    */
   if (ns4 < 34) {
      return -1;
   }
   meta->pds2.sect4.cat = (uChar) is4[9];
   meta->pds2.sect4.subcat = (uChar) is4[10];
   int nOffset = 0;
   if( is4[7] == GS4_ANALYSIS_CHEMICAL ) {
        nOffset = 16 - 14;
   }
   else if( is4[7] == GS4_OPTICAL_PROPERTIES_AEROSOL ) {
        nOffset = 38 - 14;
   }
   if (ns4 < 34 + nOffset) {
      return -1;
   }
   meta->pds2.sect4.genProcess = (uChar) is4[11 + nOffset];

   /* Initialize variables prior to parsing the specific templates. */
   meta->pds2.sect4.typeEnsemble = 0;
   meta->pds2.sect4.perturbNum = 0;
   meta->pds2.sect4.numberFcsts = 0;
   meta->pds2.sect4.derivedFcst = (uChar)-1;
   meta->pds2.sect4.validTime = meta->pds2.refTime;

   if (meta->pds2.sect4.templat == GS4_SATELLITE) {
      meta->pds2.sect4.genID = (uChar) is4[12];
      meta->pds2.sect4.numBands = (uChar) is4[13];
      meta->pds2.sect4.bands =
            (sect4_BandType *) realloc ((void *) meta->pds2.sect4.bands,
                                        meta->pds2.sect4.numBands *
                                        sizeof (sect4_BandType));
      for (i = 0; i < meta->pds2.sect4.numBands; i++) {
         if (ns4 < 20 + 10 * i + 1) {
             return -1;
         }
         meta->pds2.sect4.bands[i].series =
               (unsigned short int) is4[14 + 10 * i];
         meta->pds2.sect4.bands[i].numbers =
               (unsigned short int) is4[16 + 10 * i];
         meta->pds2.sect4.bands[i].instType = (uChar) is4[18 + 10 * i];
         meta->pds2.sect4.bands[i].centWaveNum.factor =
               (uChar) is4[19 + 10 * i];
         meta->pds2.sect4.bands[i].centWaveNum.value = is4[20 + 10 * i];
      }

      meta->pds2.sect4.fstSurfType = GRIB2MISSING_u1;
      meta->pds2.sect4.fstSurfScale = GRIB2MISSING_s1;
      meta->pds2.sect4.fstSurfValue = 0;
      meta->pds2.sect4.sndSurfType = GRIB2MISSING_u1;
      meta->pds2.sect4.sndSurfScale = GRIB2MISSING_s1;
      meta->pds2.sect4.sndSurfValue = 0;

      return 0;
   }
   if (meta->pds2.sect4.templat == GS4_SATELLITE_SYNTHETIC) {
      meta->pds2.sect4.genID = (uChar) is4[12];
      meta->pds2.sect4.numBands = (uChar) is4[22];
      meta->pds2.sect4.bands =
            (sect4_BandType *) realloc ((void *) meta->pds2.sect4.bands,
                                        meta->pds2.sect4.numBands *
                                        sizeof (sect4_BandType));
      for (i = 0; i < meta->pds2.sect4.numBands; i++) {
         if (ns4 < 30 + 11 * i + 1) {
             return -1;
         }
         meta->pds2.sect4.bands[i].series =
               (unsigned short int) is4[23 + 11 * i];
         meta->pds2.sect4.bands[i].numbers =
               (unsigned short int) is4[25 + 11 * i];
         meta->pds2.sect4.bands[i].instType = (uChar) is4[27 + 11 * i];
         meta->pds2.sect4.bands[i].centWaveNum.factor =
               (uChar) is4[29 + 11 * i];
         meta->pds2.sect4.bands[i].centWaveNum.value = is4[30 + 11 * i];
      }

      meta->pds2.sect4.fstSurfType = GRIB2MISSING_u1;
      meta->pds2.sect4.fstSurfScale = GRIB2MISSING_s1;
      meta->pds2.sect4.fstSurfValue = 0;
      meta->pds2.sect4.sndSurfType = GRIB2MISSING_u1;
      meta->pds2.sect4.sndSurfScale = GRIB2MISSING_s1;
      meta->pds2.sect4.sndSurfValue = 0;

      return 0;
   }

   meta->pds2.sect4.bgGenID = (uChar) is4[12 + nOffset];
   meta->pds2.sect4.genID = (uChar) is4[13 + nOffset];
   if ((is4[14 + nOffset] == GRIB2MISSING_u2) || (is4[16 + nOffset] == GRIB2MISSING_u1)) {
      meta->pds2.sect4.f_validCutOff = 0;
      meta->pds2.sect4.cutOff = 0;
   } else {
      meta->pds2.sect4.f_validCutOff = 1;
      meta->pds2.sect4.cutOff = is4[14 + nOffset] * 3600 + is4[16 + nOffset] * 60;
   }
/*   if (is4[18] == GRIB2MISSING_s4) */
   if (is4[18] < -1 * (0x3fffffff)) {
      //printf ("  Warning - Forecast time %ld is 'too' negative.\n", is4[18]);
      //printf ("  Assuming incorrect decoding of 2s complement.");
      is4[18] = -1 * (int)(((unsigned)is4[18])^(0x80000000));
      //printf ("  Using %ld\n", is4[18]);
   }

   meta->pds2.sect4.foreUnit = is4[17 + nOffset];
   if (ParseSect4Time2sec (meta->pds2.refTime, is4[18 + nOffset], is4[17 + nOffset],
                           &(meta->pds2.sect4.foreSec)) != 0) {
      errSprintf ("Unable to convert this TimeUnit: %ld\n", is4[17 + nOffset]);
      return -5;
   }

   meta->pds2.sect4.validTime = (time_t) (meta->pds2.refTime +
                                          meta->pds2.sect4.foreSec);

   /*
    * Following is based on what was needed to get correct Radius of Earth in
    * section 3.  (Hopefully they are consistent).
    */
   meta->pds2.sect4.fstSurfType = (uChar) is4[22 + nOffset];
   if ((is4[24 + nOffset] == GRIB2MISSING_s4) || (is4[23 + nOffset] == GRIB2MISSING_s1) ||
       (meta->pds2.sect4.fstSurfType == GRIB2MISSING_u1)) {
      meta->pds2.sect4.fstSurfScale = GRIB2MISSING_s1;
      meta->pds2.sect4.fstSurfValue = 0;
   } else {
      meta->pds2.sect4.fstSurfScale = is4[23 + nOffset];
      meta->pds2.sect4.fstSurfValue = is4[24 + nOffset] / pow (10.0, is4[23 + nOffset]);
   }
   meta->pds2.sect4.sndSurfType = (uChar) is4[28 + nOffset];
   if ((is4[30 + nOffset] == GRIB2MISSING_s4) || (is4[29 + nOffset] == GRIB2MISSING_s1) ||
       (meta->pds2.sect4.sndSurfType == GRIB2MISSING_u1)) {
      meta->pds2.sect4.sndSurfScale = GRIB2MISSING_s1;
      meta->pds2.sect4.sndSurfValue = 0;
   } else {
      meta->pds2.sect4.sndSurfScale = is4[29 + nOffset];
      meta->pds2.sect4.sndSurfValue = is4[30 + nOffset] / pow (10.0, is4[29 + nOffset]);
   }
   switch (meta->pds2.sect4.templat) {
      case GS4_ANALYSIS: /* 4.0 */
      case GS4_ERROR: /* 4.7 */
         break;
      case GS4_ENSEMBLE: /* 4.1 */
         if (ns4 < 37) {
            return -1;
         }
         meta->pds2.sect4.typeEnsemble = (uChar) is4[34];
         meta->pds2.sect4.perturbNum = (uChar) is4[35];
         meta->pds2.sect4.numberFcsts = (uChar) is4[36];
         break;
      case GS4_ENSEMBLE_STAT: /* 4.11 */
         if (ns4 < 46) {
            return -1;
         }
         meta->pds2.sect4.typeEnsemble = (uChar) is4[34];
         meta->pds2.sect4.perturbNum = (uChar) is4[35];
         meta->pds2.sect4.numberFcsts = (uChar) is4[36];
         if (ParseTime (&(meta->pds2.sect4.validTime), is4[37], is4[39],
                        is4[40], is4[41], is4[42], is4[43]) != 0) {
            msg = errSprintf (nullptr);
            meta->pds2.sect4.numInterval = (uChar) is4[44];
            if (meta->pds2.sect4.numInterval != 1) {
               errSprintf ("ERROR: in call to ParseTime from ParseSect4\n%s",
                           msg);
               errSprintf ("Most likely they didn't complete bytes 38-44 of "
                           "Template 4.11\n");
               free (msg);
               meta->pds2.sect4.numInterval = 0;
               return -1;
            }
            printf ("Warning: in call to ParseTime from ParseSect4\n%s", msg);
            free (msg);
            meta->pds2.sect4.validTime = (time_t) (meta->pds2.refTime +
                                                   meta->pds2.sect4.foreSec);
            printf ("Most likely they didn't complete bytes 38-44 of "
                    "Template 4.11\n");
         } else {
            meta->pds2.sect4.numInterval = (uChar) is4[44];
         }

         /* Added this check because some MOS grids didn't finish the
          * template. */
         if (meta->pds2.sect4.numInterval != 0) {
            temp_ptr = realloc ((void *) meta->pds2.sect4.Interval,
                                meta->pds2.sect4.numInterval *
                                sizeof (sect4_IntervalType));
            if (temp_ptr == nullptr) {
               printf ("Ran out of memory.\n");
               return -6;
            }
            meta->pds2.sect4.Interval = (sect4_IntervalType *) temp_ptr;
            meta->pds2.sect4.numMissing = is4[45];
            if (ns4 < 57 + (meta->pds2.sect4.numInterval-1)*12+1) {
                return -1;
            }
            for (i = 0; i < meta->pds2.sect4.numInterval; i++) {
               meta->pds2.sect4.Interval[i].processID =
                     (uChar) is4[49 + i * 12];
               meta->pds2.sect4.Interval[i].incrType =
                     (uChar) is4[50 + i * 12];
               meta->pds2.sect4.Interval[i].timeRangeUnit =
                     (uChar) is4[51 + i * 12];
               meta->pds2.sect4.Interval[i].lenTime = is4[52 + i * 12];
               meta->pds2.sect4.Interval[i].incrUnit =
                     (uChar) is4[56 + i * 12];
               meta->pds2.sect4.Interval[i].timeIncr =
                     (uChar) is4[57 + i * 12];
            }
         } else {
#ifdef DEBUG
            printf ("Caution: Template 4.11 had no Intervals.\n");
#endif
            meta->pds2.sect4.numMissing = is4[45];
         }
         break;
      case GS4_DERIVED: /* 4.2 */
         if (ns4 < 36) {
            return -1;
         }
         meta->pds2.sect4.derivedFcst = (uChar) is4[34];
         meta->pds2.sect4.numberFcsts = (uChar) is4[35];
         break;
      case GS4_DERIVED_CLUSTER_RECTANGULAR_AREA: /* 4.3 */
         if (ns4 < 68) {
            return -1;
         }
         meta->pds2.sect4.derivedFcst = (uChar) is4[34];
         meta->pds2.sect4.numberFcsts = (uChar) is4[35];
         break;
      case GS4_DERIVED_CLUSTER_CIRCULAR_AREA: /* 4.4 */
         if (ns4 < 64) {
            return -1;
         }
         meta->pds2.sect4.derivedFcst = (uChar) is4[34];
         meta->pds2.sect4.numberFcsts = (uChar) is4[35];
         break;
      case GS4_DERIVED_INTERVAL: /* 4.12 */
         if (ns4 < 45) {
            return -1;
         }
         meta->pds2.sect4.derivedFcst = (uChar) is4[34];
         meta->pds2.sect4.numberFcsts = (uChar) is4[35];

         if (ParseTime (&(meta->pds2.sect4.validTime), is4[36], is4[38],
                        is4[39], is4[40], is4[41], is4[42]) != 0) {
            msg = errSprintf (nullptr);
            meta->pds2.sect4.numInterval = (uChar) is4[43];
            if (meta->pds2.sect4.numInterval != 1) {
               errSprintf ("ERROR: in call to ParseTime from ParseSect4\n%s",
                           msg);
               errSprintf ("Most likely they didn't complete bytes 37-43 of "
                           "Template 4.12\n");
               free (msg);
               meta->pds2.sect4.numInterval = 0;
               return -1;
            }
            printf ("Warning: in call to ParseTime from ParseSect4\n%s", msg);
            free (msg);
            meta->pds2.sect4.validTime = (time_t) (meta->pds2.refTime +
                                                   meta->pds2.sect4.foreSec);
            printf ("Most likely they didn't complete bytes 37-43 of "
                    "Template 4.12\n");
         } else {
            meta->pds2.sect4.numInterval = (uChar) is4[43];
         }

         /* Added this check because some MOS grids didn't finish the
          * template. */
         if (meta->pds2.sect4.numInterval != 0) {
            temp_ptr = realloc ((void *) meta->pds2.sect4.Interval,
                                meta->pds2.sect4.numInterval *
                                sizeof (sect4_IntervalType));
            if (temp_ptr == nullptr) {
               printf ("Ran out of memory.\n");
               return -6;
            }
            meta->pds2.sect4.Interval = (sect4_IntervalType *) temp_ptr;
            meta->pds2.sect4.numMissing = is4[44];
            if (ns4 < 56 + (meta->pds2.sect4.numInterval-1)*12+1) {
                return -1;
            }
            for (i = 0; i < meta->pds2.sect4.numInterval; i++) {
               meta->pds2.sect4.Interval[i].processID =
                     (uChar) is4[48 + i * 12];
               meta->pds2.sect4.Interval[i].incrType =
                     (uChar) is4[49 + i * 12];
               meta->pds2.sect4.Interval[i].timeRangeUnit =
                     (uChar) is4[50 + i * 12];
               meta->pds2.sect4.Interval[i].lenTime = is4[51 + i * 12];
               meta->pds2.sect4.Interval[i].incrUnit =
                     (uChar) is4[55 + i * 12];
               meta->pds2.sect4.Interval[i].timeIncr =
                     (uChar) is4[56 + i * 12];
            }
         } else {
#ifdef DEBUG
            printf ("Caution: Template 4.12 had no Intervals.\n");
#endif
            meta->pds2.sect4.numMissing = is4[44];
         }
         break;
      case GS4_DERIVED_INTERVAL_CLUSTER_RECTANGULAR_AREA: /* 4.13 */
      case GS4_DERIVED_INTERVAL_CLUSTER_CIRCULAR_AREA: /* 4.14 */
         if (ns4 < 36) {
            return -1;
         }
         meta->pds2.sect4.derivedFcst = (uChar) is4[34];
         meta->pds2.sect4.numberFcsts = (uChar) is4[35];
         break;
      case GS4_STATISTIC: /* 4.8 */
         if (ns4 < 43) {
            return -1;
         }
         if (ParseTime (&(meta->pds2.sect4.validTime), is4[34], is4[36],
                        is4[37], is4[38], is4[39], is4[40]) != 0) {
            msg = errSprintf (nullptr);
            meta->pds2.sect4.numInterval = (uChar) is4[41];
            if (meta->pds2.sect4.numInterval != 1) {
               errSprintf ("ERROR: in call to ParseTime from ParseSect4\n%s",
                           msg);
               errSprintf ("Most likely they didn't complete bytes 35-41 of "
                           "Template 4.8\n");
               free (msg);
               meta->pds2.sect4.numInterval = 0;
               return -1;
            }
            printf ("Warning: in call to ParseTime from ParseSect4\n%s", msg);
            free (msg);
            meta->pds2.sect4.validTime = (time_t) (meta->pds2.refTime +
                                                   meta->pds2.sect4.foreSec);
            printf ("Most likely they didn't complete bytes 35-41 of "
                    "Template 4.8\n");
         } else {
            meta->pds2.sect4.numInterval = (uChar) is4[41];
         }

         /* Added this check because some MOS grids didn't finish the
          * template. */
         if (meta->pds2.sect4.numInterval != 0) {
            temp_ptr = realloc ((void *) meta->pds2.sect4.Interval,
                                meta->pds2.sect4.numInterval *
                                sizeof (sect4_IntervalType));
            if (temp_ptr == nullptr) {
               printf ("Ran out of memory.\n");
               return -6;
            }
            meta->pds2.sect4.Interval = (sect4_IntervalType *) temp_ptr;
            meta->pds2.sect4.numMissing = is4[42];
            if (ns4 < 54 + (meta->pds2.sect4.numInterval-1)*12+1) {
                return -1;
            }
            for (i = 0; i < meta->pds2.sect4.numInterval; i++) {
               meta->pds2.sect4.Interval[i].processID =
                     (uChar) is4[46 + i * 12];
               meta->pds2.sect4.Interval[i].incrType =
                     (uChar) is4[47 + i * 12];
               meta->pds2.sect4.Interval[i].timeRangeUnit =
                     (uChar) is4[48 + i * 12];
               meta->pds2.sect4.Interval[i].lenTime = is4[49 + i * 12];
               meta->pds2.sect4.Interval[i].incrUnit =
                     (uChar) is4[53 + i * 12];
               meta->pds2.sect4.Interval[i].timeIncr =
                     (uChar) is4[54 + i * 12];
            }
         } else {
#ifdef DEBUG
            printf ("Caution: Template 4.8 had no Intervals.\n");
#endif
            meta->pds2.sect4.numMissing = is4[42];
         }
         break;
      case GS4_PERCENT_PNT: /* 4.6 */
         if( ns4 < 35) {
            return -1;
         }
         meta->pds2.sect4.percentile = is4[34];
         break;
      case GS4_PERCENT_TIME: /* 4.10 */
         if (ns4 < 44) {
            return -1;
         }
         meta->pds2.sect4.percentile = is4[34];
         if (ParseTime (&(meta->pds2.sect4.validTime), is4[35], is4[37],
                        is4[38], is4[39], is4[40], is4[41]) != 0) {
            msg = errSprintf (nullptr);
            meta->pds2.sect4.numInterval = (uChar) is4[42];
            if (meta->pds2.sect4.numInterval != 1) {
               errSprintf ("ERROR: in call to ParseTime from ParseSect4\n%s",
                           msg);
               errSprintf ("Most likely they didn't complete bytes 35-41 of "
                           "Template 4.10\n");
               free (msg);
               meta->pds2.sect4.numInterval = 0;
               return -1;
            }
            printf ("Warning: in call to ParseTime from ParseSect4\n%s", msg);
            free (msg);
            meta->pds2.sect4.validTime = (time_t) (meta->pds2.refTime +
                                                   meta->pds2.sect4.foreSec);
            printf ("Most likely they didn't complete bytes 35-41 of "
                    "Template 4.10\n");
         } else {
            meta->pds2.sect4.numInterval = (uChar) is4[42];
         }

         /* Added this check because some MOS grids didn't finish the
          * template. */
         if (meta->pds2.sect4.numInterval != 0) {
            temp_ptr = realloc ((void *) meta->pds2.sect4.Interval,
                                meta->pds2.sect4.numInterval *
                                sizeof (sect4_IntervalType));
            if (temp_ptr == nullptr) {
               printf ("Ran out of memory.\n");
               return -6;
            }
            meta->pds2.sect4.Interval = (sect4_IntervalType *) temp_ptr;
            meta->pds2.sect4.numMissing = is4[43];
            if (ns4 < 55 + (meta->pds2.sect4.numInterval-1)*12+1) {
                return -1;
            }
            for (i = 0; i < meta->pds2.sect4.numInterval; i++) {
               meta->pds2.sect4.Interval[i].processID =
                     (uChar) is4[47 + i * 12];
               meta->pds2.sect4.Interval[i].incrType =
                     (uChar) is4[48 + i * 12];
               meta->pds2.sect4.Interval[i].timeRangeUnit =
                     (uChar) is4[49 + i * 12];
               meta->pds2.sect4.Interval[i].lenTime = is4[50 + i * 12];
               meta->pds2.sect4.Interval[i].incrUnit =
                     (uChar) is4[54 + i * 12];
               meta->pds2.sect4.Interval[i].timeIncr =
                     (uChar) is4[55 + i * 12];
            }
         } else {
#ifdef DEBUG
            printf ("Caution: Template 4.10 had no Intervals.\n");
#endif
            meta->pds2.sect4.numMissing = is4[43];
         }
         break;
      case GS4_PROBABIL_PNT: /* 4.5 */
         if (ns4 < 44) {
            return -1;
         }
         meta->pds2.sect4.foreProbNum = (uChar) is4[34];
         meta->pds2.sect4.numForeProbs = (uChar) is4[35];
         meta->pds2.sect4.probType = (uChar) is4[36];
         meta->pds2.sect4.lowerLimit.factor =
               sbit_2Comp_oneByte((sChar) is4[37]);
         meta->pds2.sect4.lowerLimit.value = sbit_2Comp_fourByte(is4[38]);
         meta->pds2.sect4.upperLimit.factor =
               sbit_2Comp_oneByte((sChar) is4[42]);
         meta->pds2.sect4.upperLimit.value = sbit_2Comp_fourByte(is4[43]);
         break;
      case GS4_PROBABIL_TIME: /* 4.9 */
         if (ns4 < 56) {
            return -1;
         }
         meta->pds2.sect4.foreProbNum = (uChar) is4[34];
         meta->pds2.sect4.numForeProbs = (uChar) is4[35];
         meta->pds2.sect4.probType = (uChar) is4[36];
         meta->pds2.sect4.lowerLimit.factor =
               sbit_2Comp_oneByte((sChar) is4[37]);
         meta->pds2.sect4.lowerLimit.value = sbit_2Comp_fourByte(is4[38]);
         meta->pds2.sect4.upperLimit.factor =
               sbit_2Comp_oneByte((sChar) is4[42]);
         meta->pds2.sect4.upperLimit.value = sbit_2Comp_fourByte(is4[43]);
         if (ParseTime (&(meta->pds2.sect4.validTime), is4[47], is4[49],
                        is4[50], is4[51], is4[52], is4[53]) != 0) {
            msg = errSprintf (nullptr);
            meta->pds2.sect4.numInterval = (uChar) is4[54];
            if (meta->pds2.sect4.numInterval != 1) {
               errSprintf ("ERROR: in call to ParseTime from ParseSect4\n%s",
                           msg);
               errSprintf ("Most likely they didn't complete bytes 48-54 of "
                           "Template 4.9\n");
               free (msg);
               meta->pds2.sect4.numInterval = 0;
               return -1;
            }
            printf ("Warning: in call to ParseTime from ParseSect4\n%s", msg);
            free (msg);
            meta->pds2.sect4.validTime = (time_t) (meta->pds2.refTime +
                                                   meta->pds2.sect4.foreSec);
            printf ("Most likely they didn't complete bytes 48-54 of "
                    "Template 4.9\n");
         } else {
            meta->pds2.sect4.numInterval = (uChar) is4[54];
         }
         temp_ptr = realloc ((void *) meta->pds2.sect4.Interval,
                             meta->pds2.sect4.numInterval *
                             sizeof (sect4_IntervalType));
         if (temp_ptr == nullptr) {
            printf ("Ran out of memory.\n");
            return -6;
         }
         meta->pds2.sect4.Interval = (sect4_IntervalType *) temp_ptr;
         meta->pds2.sect4.numMissing = is4[55];
         if (ns4 < 67 + (meta->pds2.sect4.numInterval-1)*12+1) {
            return -1;
         }
         for (i = 0; i < meta->pds2.sect4.numInterval; i++) {
            meta->pds2.sect4.Interval[i].processID = (uChar) is4[59 + i * 12];
            meta->pds2.sect4.Interval[i].incrType = (uChar) is4[60 + i * 12];
            meta->pds2.sect4.Interval[i].timeRangeUnit =
                  (uChar) is4[61 + i * 12];
            meta->pds2.sect4.Interval[i].lenTime = is4[62 + i * 12];
            meta->pds2.sect4.Interval[i].incrUnit = (uChar) is4[66 + i * 12];
            meta->pds2.sect4.Interval[i].timeIncr = (uChar) is4[67 + i * 12];
         }
         break;
      case GS4_STATISTIC_SPATIAL_AREA: /* 4.15 */
            // TODO. Need to fetch
            // 35 Statistical process used within the spatial area defined by octet 36 (see Code Table 4.10)
            // 36 Type of spatial processing used to arrive at given data value from source data (see Code Table 4.15)
            // 37 Number of data points used in spatial processing defined in octet 36
            break;
      case GS4_ANALYSIS_CHEMICAL: /* 4.40 */
            // TODO
            break;
      case GS4_OPTICAL_PROPERTIES_AEROSOL: /* 4.48 */
            // TODO
            break;
      default:
         errSprintf ("Un-supported Template. %ld\n", is4[7]);
         return -4;
   }

   /* Do only that check at the end so that other meta fields are properly set */
   /* otherwise we might do erroneous unit conversion as in */
   /* https://github.com/OSGeo/gdal/issues/3158 */
   if (is4[5] != 0) {
#ifdef DEBUG
      printf ("Un-supported template.\n  All Supported template "
              "have 0 coordinate vertical values after template.");
#endif
      errSprintf ("Un-supported template.\n  All Supported template "
                  "have 0 coordinate vertical values after template.");
      return -4;
   }

   return 0;
}

/*****************************************************************************
 * ParseSect5() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To verify and parse section 5 data.
 *
 * ARGUMENTS
 *    is5 = The unpacked section 5 array. (Input)
 *    ns5 = The size of section 5. (Input)
 *   meta = The structure to fill. (Output)
 * xmissp = The primary missing value. (Input)
 * xmisss = The secondary missing value. (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *  0 = OK
 * -1 = ns5 is too small.
 * -2 = unexpected values in is5.
 * -6 = unsupported packing.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 *****************************************************************************
 */
static int ParseSect5 (sInt4 *is5, sInt4 ns5, grib_MetaData *meta,
                       float xmissp, float xmisss)
{
   if (ns5 < 22) {
      return -1;
   }
   if (is5[4] != 5) {
      errSprintf ("ERROR IS5 not labeled correctly. %ld\n", is5[5]);
      return -2;
   }
   if ((is5[9] != GS5_SIMPLE) && (is5[9] != GS5_CMPLX) &&
       (is5[9] != GS5_CMPLXSEC) && (is5[9] != GS5_IEEE) &&
       (is5[9] != GS5_SPECTRAL) &&
       (is5[9] != GS5_HARMONIC) && (is5[9] != GS5_JPEG2000) &&
       (is5[9] != GS5_PNG) && (is5[9] != GS5_JPEG2000_ORG) &&
       (is5[9] != GS5_PNG_ORG)) {
      errSprintf ("Un-supported Packing? %ld\n", is5[9]);
      return -6;
   }
   meta->gridAttrib.packType = (sInt4) is5[9];
   meta->gridAttrib.f_maxmin = 0;
   meta->gridAttrib.missPri = xmissp;
   meta->gridAttrib.missSec = xmisss;
   if ( (is5[9] == GS5_IEEE) || (is5[9] == GS5_SPECTRAL) || (is5[9] == GS5_HARMONIC)) {
      meta->gridAttrib.fieldType = 0;
      meta->gridAttrib.f_miss = 0;
      return 0;
   }
   if (is5[20] > 1) {
      errSprintf ("Invalid field type. %ld\n", is5[20]);
      return -2;
   }
   MEMCPY_BIG (&meta->gridAttrib.refVal, &(is5[11]), 4);
   meta->gridAttrib.ESF = is5[15];
   meta->gridAttrib.DSF = is5[17];
   meta->gridAttrib.fieldType = (uChar) is5[20];
   if ((is5[9] == GS5_SIMPLE) ||
       (is5[9] == GS5_JPEG2000) || (is5[9] == GS5_JPEG2000_ORG) ||
       (is5[9] == GS5_PNG) || (is5[9] == GS5_PNG_ORG)) {
      meta->gridAttrib.f_miss = 0;
      return 0;
   }

   myAssert( (is5[9] == GS5_CMPLX) || (is5[9] == GS5_CMPLXSEC) );

   if (ns5 < 23) {
       return -1;
   }
   if (is5[22] > 2) {
       errSprintf ("Invalid missing management type, f_miss = %ld\n",
                   is5[22]);
       return -2;
   }
   meta->gridAttrib.f_miss = (uChar) is5[22];

   return 0;
}

/*****************************************************************************
 * MetaParse() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To parse all the meta data from a grib2 message.
 *
 * ARGUMENTS
 *   meta = The structure to fill. (Output)
 *    is0 = The unpacked section 0 array. (Input)
 *    ns0 = The size of section 0. (Input)
 *    is1 = The unpacked section 1 array. (Input)
 *    ns1 = The size of section 1. (Input)
 *    is2 = The unpacked section 2 array. (Input)
 *    ns2 = The size of section 2. (Input)
 *   rdat = The float data in section 2. (Input)
 *  nrdat = Length of rdat. (Input)
 *   idat = The integer data in section 2. (Input)
 *  nidat = Length of idat. (Input)
 *    is3 = The unpacked section 3 array. (Input)
 *    ns3 = The size of section 3. (Input)
 *    is4 = The unpacked section 4 array. (Input)
 *    ns4 = The size of section 4. (Input)
 *    is5 = The unpacked section 5 array. (Input)
 *    ns5 = The size of section 5. (Input)
 * grib_len = The length of the entire grib message. (Input)
 * xmissp = The primary missing value. (Input)
 * xmisss = The secondary missing value. (Input)
 * simpVer = The version of the simple weather code to use when parsing the
 *           WxString (if applicable). (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: int (could use errSprintf())
 *   0 = OK
 *  -1 = A dimension is too small.
 *  -2 = unexpected values in a grib section.
 *  -3 = un-supported map Projection.
 *  -4 = un-supported Sect 4 template.
 *  -5 = unsupported forecast time unit.
 *  -6 = unsupported sect 5 packing.
 * -10 = Something the driver can't handle yet.
 *       (prodType != 0, f_sphere != 1, etc)
 * -11 = Weather grid without a lookup table.
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *
 * NOTES
 *****************************************************************************
 */
int MetaParse (grib_MetaData *meta, sInt4 *is0, sInt4 ns0,
               sInt4 *is1, sInt4 ns1, sInt4 *is2, sInt4 ns2,
               float *rdat, sInt4 nrdat, sInt4 *idat, sInt4 nidat,
               sInt4 *is3, sInt4 ns3, sInt4 *is4, sInt4 ns4,
               sInt4 *is5, sInt4 ns5, sInt4 grib_len,
               float xmissp, float xmisss, int simpVer, CPL_UNUSED int simpWWA)
{
   int ierr;            /* The error code of a called routine */
   /* char *element; *//* Holds the name of the current variable. */
   /* char *comment; *//* Holds more comments about current variable. */
   /* char *unitName; *//* Holds the name of the unit [K] [%] .. etc */
   uChar probType;      /* The probability type */
   double lowerProb;    /* The lower limit on probability forecast if
                         * template 4.5 or 4.9 */
   double upperProb;    /* The upper limit on probability forecast if
                         * template 4.5 or 4.9 */
   sInt4 lenTime;       /* Length of time for element (see 4.8 and 4.9) */
   uChar timeRangeUnit = 1;
   uChar incrType;
   uChar statProcessID; /* Statistical process id or 255 for missing */
   uChar fstSurfType;   /* Type of the first fixed surface. */
   sInt4 value;         /* The scaled value from GRIB2 file. */
   sChar scale;         /* Surface scale as opposed to probility factor. */
   double fstSurfValue; /* Value of first fixed surface. */
   sChar f_fstValue;    /* flag if FstValue is valid. */
   uChar sndSurfType;   /* Type of the second fixed surface. */
   double sndSurfValue; /* Value of second fixed surface. */
   sChar f_sndValue;    /* flag if SndValue is valid. */

   if ((ierr = ParseSect0 (is0, ns0, grib_len, meta)) != 0) {
      preErrSprintf ("Parse error Section 0\n");
      //return ierr;
   }
   if ((ierr = ParseSect1 (is1, ns1, meta)) != 0) {
		 preErrSprintf ("Parse error Section 1\n");
      //return ierr;
   }
   if (ns2 < 7) {
      errSprintf ("ns2 was too small in MetaParse\n");
      //return -1;
   }
   meta->pds2.f_sect2 = (uChar) (is2[0] != 0);
   if (meta->pds2.f_sect2) {
      meta->pds2.sect2NumGroups = is2[7 - 1];
   } else {
      meta->pds2.sect2NumGroups = 0;
   }
   if ((ierr = ParseSect3 (is3, ns3, meta)) != 0) {
      preErrSprintf ("Parse error Section 3\n");
      //return ierr;
   }
   if (IsData_NDFD (meta->center, meta->subcenter)) {
      meta->gds.hdatum = 1;
   }
   if (meta->gds.f_sphere != 1) {
      errSprintf ("Driver Filter: Can only handle spheres.\n");
      //return -10;
   }
   if ((ierr = ParseSect4 (is4, ns4, meta)) != 0) {
      preErrSprintf ("Parse error Section 4\n");
      //return ierr;
   }
   if ((ierr = ParseSect5 (is5, ns5, meta, xmissp, xmisss)) != 0) {
      preErrSprintf ("Parse error Section 5\n");
      //return ierr;
   }
   /* Compute ElementName. */
   if (meta->element) {
      free (meta->element);
      meta->element = nullptr;
   }
   if (meta->unitName) {
      free (meta->unitName);
      meta->unitName = nullptr;
   }
   if (meta->comment) {
      free (meta->comment);
      meta->comment = nullptr;
   }

   if ((meta->pds2.sect4.templat == GS4_PROBABIL_TIME) ||
       (meta->pds2.sect4.templat == GS4_PROBABIL_PNT)) {
      probType = meta->pds2.sect4.probType;
      lowerProb = meta->pds2.sect4.lowerLimit.value *
            pow (10.0, -1 * meta->pds2.sect4.lowerLimit.factor);
      upperProb = meta->pds2.sect4.upperLimit.value *
            pow (10.0, -1 * meta->pds2.sect4.upperLimit.factor);
   } else {
      probType = 0;
      lowerProb = 0;
      upperProb = 0;
   }
   if (meta->pds2.sect4.numInterval > 0) {
      /* Try to convert lenTime to hourly. */
      timeRangeUnit = meta->pds2.sect4.Interval[0].timeRangeUnit;
      if (meta->pds2.sect4.Interval[0].timeRangeUnit == 255) {
         lenTime = (sInt4) ((meta->pds2.sect4.validTime -
                             meta->pds2.sect4.foreSec -
                             meta->pds2.refTime) / 3600);
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 0) {
         lenTime = (sInt4) (meta->pds2.sect4.Interval[0].lenTime / 60.);
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 1) {
         lenTime = meta->pds2.sect4.Interval[0].lenTime;
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 2) {
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 24;
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 10) {
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 3;
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 11) {
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 6;
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 12) {
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 12;
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 13) {
         lenTime = (sInt4) (meta->pds2.sect4.Interval[0].lenTime / 3600.);
         timeRangeUnit = 1;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 3) {  /* month */
         lenTime = meta->pds2.sect4.Interval[0].lenTime;
         timeRangeUnit = 3;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 4) {  /* year */
         lenTime = meta->pds2.sect4.Interval[0].lenTime;
         timeRangeUnit = 4;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 5) {  /* decade */
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 10;
         timeRangeUnit = 4;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 6) {  /* normal */
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 30;
         timeRangeUnit = 4;
      } else if (meta->pds2.sect4.Interval[0].timeRangeUnit == 7) {  /* century */
         lenTime = meta->pds2.sect4.Interval[0].lenTime * 100;
         timeRangeUnit = 4;
      } else {
         lenTime = 0;
         printf ("Can't handle this timeRangeUnit\n");
         myAssert (meta->pds2.sect4.Interval[0].timeRangeUnit == 1);
      }
/*
      } else {
         lenTime = 255;
      }
      if (lenTime == 255) {
         lenTime = (meta->pds2.sect4.validTime - meta->pds2.sect4.foreSec -
                    meta->pds2.refTime) / 3600;
      }
*/
      if (lenTime == GRIB2MISSING_s4) {
         lenTime = 0;
      }
      incrType = meta->pds2.sect4.Interval[0].incrType;
      statProcessID = meta->pds2.sect4.Interval[0].processID;
   } else {
      lenTime = 0;
      timeRangeUnit = 1;
      incrType = 255;
      statProcessID = 255;
   }

   if ((meta->pds2.sect4.templat == GS4_RADAR) || (meta->pds2.sect4.templat == GS4_SATELLITE)
       || (meta->pds2.sect4.templat == 254) || (meta->pds2.sect4.templat == 1000) || (meta->pds2.sect4.templat == 1001)
       || (meta->pds2.sect4.templat == 1002)) {
      fstSurfValue = 0;
      f_fstValue = 0;
      fstSurfType = 0;
      sndSurfValue = 0;
      f_sndValue = 0;
   } else {
      fstSurfType = meta->pds2.sect4.fstSurfType;
      scale = meta->pds2.sect4.fstSurfScale;
      if (meta->pds2.sect4.fstSurfValue >= std::numeric_limits<sInt4>::max() ||
          meta->pds2.sect4.fstSurfValue <= std::numeric_limits<sInt4>::min()) {
         // Out of range, so just call it missing.
         preErrSprintf ("fstSurfValue out of range\n");
         value = GRIB2MISSING_s4;
      } else {
        value = static_cast<sInt4>(meta->pds2.sect4.fstSurfValue);
      }
      if ((value == GRIB2MISSING_s4) || (scale == GRIB2MISSING_s1) ||
          (fstSurfType == GRIB2MISSING_u1)) {
         fstSurfValue = 0;
         f_fstValue = 1;
      } else {
         fstSurfValue = value * pow (10.0, -1 * scale);
         f_fstValue = 1;
      }
      sndSurfType = meta->pds2.sect4.sndSurfType;
      scale = meta->pds2.sect4.sndSurfScale;
      if (meta->pds2.sect4.sndSurfValue < std::numeric_limits<int>::max() &&
          meta->pds2.sect4.sndSurfValue > std::numeric_limits<int>::min()) {
         value = static_cast<int>(meta->pds2.sect4.sndSurfValue);
      } else {
         // sndSurfValue is out of range, so just call it missing.
         // TODO(schwehr): Consider using a tmp double if the scale will
         // make the resulting sndSurfValue be within range.
         preErrSprintf ("sndSurfValue out of range\n");
         value = GRIB2MISSING_s4;
      }
      if ((value == GRIB2MISSING_s4) || (scale == GRIB2MISSING_s1) ||
          (sndSurfType == GRIB2MISSING_u1)) {
         sndSurfValue = 0;
         f_sndValue = 0;
      } else {
         sndSurfValue = value * pow (10.0, -1 * scale);
         f_sndValue = 1;
      }
   }

   ParseElemName (meta->pds2.mstrVersion, meta->center, meta->subcenter, meta->pds2.prodType,
                  meta->pds2.sect4.templat, meta->pds2.sect4.cat,
                  meta->pds2.sect4.subcat, lenTime, timeRangeUnit, statProcessID,
                  incrType, meta->pds2.sect4.genID, probType, lowerProb, upperProb,
                  meta->pds2.sect4.derivedFcst,
                  &(meta->element), &(meta->comment), &(meta->unitName),
                  &(meta->convert), meta->pds2.sect4.percentile,
                  meta->pds2.sect4.genProcess,
                  f_fstValue, fstSurfValue, f_sndValue, sndSurfValue);
#ifdef DEBUG
/*
   printf ("Element: %s\nunitName: %s\ncomment: %s\n", meta->element,
           meta->comment, meta->unitName);
*/
#endif

   if (! f_fstValue) {
      reallocSprintf (&(meta->shortFstLevel), "0 undefined");
      reallocSprintf (&(meta->longFstLevel), "0.000[-] undefined ()");
   } else {
      ParseLevelName (meta->center, meta->subcenter, fstSurfType,
                      fstSurfValue, f_sndValue, sndSurfValue,
                      &(meta->shortFstLevel), &(meta->longFstLevel));
   }

   /* Continue parsing section 2 data. */
   if (meta->pds2.f_sect2) {
      MetaSect2Free (meta);
      if (strcmp (meta->element, "Wx") == 0) {
         meta->pds2.sect2.ptrType = GS2_WXTYPE;
         if ((ierr = ParseSect2_Wx (rdat, nrdat, idat, nidat,
                                    &(meta->pds2.sect2.wx), simpVer)) != 0) {
            preErrSprintf ("Parse error Section 2 : Weather Data\n");
            return ierr;
         }
      } else if (strcmp (meta->element, "WWA") == 0) {
         meta->pds2.sect2.ptrType = GS2_HAZARD;
         if ((ierr = ParseSect2_Hazard (rdat, nrdat, idat, nidat,
                                    &(meta->pds2.sect2.hazard), simpWWA)) != 0) {
            preErrSprintf ("Parse error Section 2 : Hazard Data\n");
            return ierr;
         }
      } else {
         meta->pds2.sect2.ptrType = GS2_UNKNOWN;
         if ((ierr = ParseSect2_Unknown (rdat, nrdat, idat, nidat, meta))
             != 0) {
            preErrSprintf ("Parse error Section 2 : Unknown Data type\n");
            //return ierr;
         }
      }
   } else {
      if (strcmp (meta->element, "Wx") == 0) {
         errSprintf ("Weather grid does not have look up table?");
         //return -11;
      }
      if (strcmp (meta->element, "WWA") == 0) {
         errSprintf ("Hazard grid does not have look up table?");
         //return -11;
      }
   }
   return 0;
}

/*****************************************************************************
 * ParseGridNoMiss() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   A helper function for ParseGrid.  In this particular case it is dealing
 * with a field that has NO missing value type.
 *   Walks through either a float or an integer grid, computing the min/max
 * values in the grid, and converts the units. It uses gridAttrib info for the
 * missing values and it updates gridAttrib with the observed min/max values.
 *
 * ARGUMENTS
 *    attrib = Grid Attribute structure already filled in (Input/Output)
 * grib_Data = The place to store the grid data. (Output)
 *    Nx, Ny = The dimensions of the grid (Input)
 *      iain = Place to find data if it is an Integer (or float). (Input)
 *     unitM = M in unit conversion equation y(new) = m x(orig) + b (Input)
 *     unitB = B in unit conversion equation y(new) = m x(orig) + b (Input)
 *    f_txtType = true if we have a valid wx/hazard type. (Input)
 *  txt_dataLen = Length of text table
 *  txt_f_valid = whether that entry is used/valid. (Input)
 *    startX = The start of the X values. (Input)
 *    startY = The start of the Y values. (Input)
 *     subNx = The Nx dimension of the subgrid (Input)
 *     subNy = The Ny dimension of the subgrid (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *  12/2002 Arthur Taylor (MDL/RSIS): Created to optimize part of ParseGrid.
 *   5/2003 AAT: Added ability to see if wxType occurs.  If so sets table
 *          valid to 2, otherwise leaves it at 1.  If table valid is 0 then
 *          sets value to missing value (if applicable).
 *   2/2004 AAT: Added the subgrid capability.
 *
 * NOTES
 * 1) Don't have to check if value became missing value, because we can check
 *    if missing falls in the range of the min/max converted units.  If
 *    missing does fall in that range we need to move missing.
 *    (See f_readjust in ParseGrid)
 *****************************************************************************
 */
static void ParseGridNoMiss (gridAttribType *attrib, double *grib_Data,
                             sInt4 Nx, sInt4 Ny, sInt4 *iain,
                             double unitM, double unitB,
                             uChar f_txtType, uInt4 txt_dataLen,
                             uChar *txt_f_valid, int startX, int startY,
                             int subNx, int subNy)
{
   sInt4 x, y;          /* Where we are in the grid. */
   double value;        /* The data in the new units. */
   uChar f_maxmin = 0;  /* Flag if max/min is valid yet. */
   uInt4 index;         /* Current index into Wx table. */
   sInt4 *itemp = nullptr;
   float *ftemp = nullptr;

   /* Resolve possibility that the data is an integer or a float and find
    * max/min values. (see note 1) */
   for (y = 0; y < subNy; y++) {
      if (((startY + y - 1) < 0) || ((startY + y - 1) >= Ny)) {
         for (x = 0; x < subNx; x++) {
            *grib_Data++ = 9999;
         }
      } else {
         if (attrib->fieldType) {
            itemp = iain + (startY + y - 1) * Nx + (startX - 1);
         } else {
            ftemp = reinterpret_cast<float *>(iain) + (startY + y - 1) * Nx + (startX - 1);
         }
         for (x = 0; x < subNx; x++) {
            if (((startX + x - 1) < 0) || ((startX + x - 1) >= Nx)) {
               *grib_Data++ = 9999;
            } else {
               /* Convert the units. */
               if (attrib->fieldType) {
                  if (unitM == -10) {
                     value = pow (10.0, (*itemp++));
                  } else {
                     value = unitM * (*itemp++) + unitB;
                  }
               } else {
                  if (unitM == -10) {
                     value = pow (10.0, (double) (*ftemp++));
                  } else {
                     value = unitM * (*ftemp++) + unitB;
                  }
               }
               if (f_txtType) {
                  index = (uInt4) value;
                  if (index < txt_dataLen) {
                     if (txt_f_valid[index] == 1) {
                        txt_f_valid[index] = 2;
                     } else if (txt_f_valid[index] == 0) {
                        /* Table is not valid here so set value to missing? */
                        /* No missing value, so use index = WxType->dataLen? */
                        /* No... set f_valid to 3 so we know we used this
                         * invalid element, then handle it in degrib2.c ::
                         * ReadGrib2Record() where we set it back to 0. */
                        txt_f_valid[index] = 3;
                     }
                  }
               }
               if (f_maxmin) {
                  if (value < attrib->min) {
                     attrib->min = value;
                  } else if (value > attrib->max) {
                     attrib->max = value;
                  }
               } else {
                  attrib->min = attrib->max = value;
                  f_maxmin = 1;
               }
               *grib_Data++ = value;
            }
         }
      }
   }
   attrib->f_maxmin = f_maxmin;
}

/*****************************************************************************
 * ParseGridPrimMiss() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   A helper function for ParseGrid.  In this particular case it is dealing
 * with a field that has primary missing value type.
 *   Walks through either a float or an integer grid, computing the min/max
 * values in the grid, and converts the units. It uses gridAttrib info for the
 * missing values and it updates gridAttrib with the observed min/max values.
 *
 * ARGUMENTS
 *    attrib = sect 5 structure already filled in by ParseSect5 (In/Output)
 * grib_Data = The place to store the grid data. (Output)
 *    Nx, Ny = The dimensions of the grid (Input)
 *      iain = Place to find data if it is an Integer (or float). (Input)
 *     unitM = M in unit conversion equation y(new) = m x(orig) + b (Input)
 *     unitB = B in unit conversion equation y(new) = m x(orig) + b (Input)
 *    f_txtType = true if we have a valid wx/hazard type. (Input)
 *  txt_dataLen = Length of text table
 *  txt_f_valid = whether that entry is used/valid. (Input)
 *    startX = The start of the X values. (Input)
 *    startY = The start of the Y values. (Input)
 *     subNx = The Nx dimension of the subgrid (Input)
 *     subNy = The Ny dimension of the subgrid (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *  12/2002 Arthur Taylor (MDL/RSIS): Created to optimize part of ParseGrid.
 *   5/2003 AAT: Added ability to see if wxType occurs.  If so sets table
 *          valid to 2, otherwise leaves it at 1.  If table valid is 0 then
 *          sets value to missing value (if applicable).
 *   2/2004 AAT: Added the subgrid capability.
 *
 * NOTES
 * 1) Don't have to check if value became missing value, because we can check
 *    if missing falls in the range of the min/max converted units.  If
 *    missing does fall in that range we need to move missing.
 *    (See f_readjust in ParseGrid)
 *****************************************************************************
 */
static void ParseGridPrimMiss (gridAttribType *attrib, double *grib_Data,
                               sInt4 Nx, sInt4 Ny, sInt4 *iain,
                               double unitM, double unitB, sInt4 *missCnt,
                               uChar f_txtType, uInt4 txt_dataLen,
                               uChar *txt_f_valid,
                               int startX, int startY, int subNx, int subNy)
{
   sInt4 x, y;          /* Where we are in the grid. */
   double value;        /* The data in the new units. */
   uChar f_maxmin = 0;  /* Flag if max/min is valid yet. */
   uInt4 index;         /* Current index into Wx table. */
   sInt4 *itemp = nullptr;
   float *ftemp = nullptr;
/*   float *ain = (float *) iain;*/

   /* Resolve possibility that the data is an integer or a float and find
    * max/min values. (see note 1) */
   for (y = 0; y < subNy; y++) {
      if (((startY + y - 1) < 0) || ((startY + y - 1) >= Ny)) {
         for (x = 0; x < subNx; x++) {
            *grib_Data++ = attrib->missPri;
            (*missCnt)++;
         }
      } else {
         if (attrib->fieldType) {
            itemp = iain + (startY + y - 1) * Nx + (startX - 1);
         } else {
            ftemp = reinterpret_cast<float *>(iain) + (startY + y - 1) * Nx + (startX - 1);
         }
         for (x = 0; x < subNx; x++) {
            if (((startX + x - 1) < 0) || ((startX + x - 1) >= Nx)) {
               *grib_Data++ = attrib->missPri;
               (*missCnt)++;
            } else {
               if (attrib->fieldType) {
                  value = (*itemp++);
               } else {
                  value = (*ftemp++);
               }

               /* Make sure value is not a missing value when converting
                * units, and while computing max/min. */
               if (value == attrib->missPri) {
                  (*missCnt)++;
               } else {
                  /* Convert the units. */
                  if (unitM == -10) {
                     value = pow (10.0, value);
                  } else {
                     value = unitM * value + unitB;
                  }
                  if (f_txtType) {
                     index = (uInt4) value;
                     if (index < txt_dataLen) {
                        if (txt_f_valid[index]) {
                           txt_f_valid[index] = 2;
                        } else {
                           /* Table is not valid here so set value to missPri
                            */
                           value = attrib->missPri;
                           (*missCnt)++;
                        }
                     }
                  }
                  if ((!f_txtType) || (value != attrib->missPri)) {
                     if (f_maxmin) {
                        if (value < attrib->min) {
                           attrib->min = value;
                        } else if (value > attrib->max) {
                           attrib->max = value;
                        }
                     } else {
                        attrib->min = attrib->max = value;
                        f_maxmin = 1;
                     }
                  }
               }
               *grib_Data++ = value;
            }
         }
      }
   }
   attrib->f_maxmin = f_maxmin;
}

/*****************************************************************************
 * ParseGridSecMiss() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   A helper function for ParseGrid.  In this particular case it is dealing
 * with a field that has NO missing value type.
 *   Walks through either a float or an integer grid, computing the min/max
 * values in the grid, and converts the units. It uses gridAttrib info for the
 * missing values and it updates gridAttrib with the observed min/max values.
 *
 * ARGUMENTS
 *    attrib = sect 5 structure already filled in by ParseSect5 (In/Output)
 * grib_Data = The place to store the grid data. (Output)
 *    Nx, Ny = The dimensions of the grid (Input)
 *      iain = Place to find data if it is an Integer (or float). (Input)
 *     unitM = M in unit conversion equation y(new) = m x(orig) + b (Input)
 *     unitB = B in unit conversion equation y(new) = m x(orig) + b (Input)
 *    f_txtType = true if we have a valid wx/hazard type. (Input)
 *  txt_dataLen = Length of text table
 *  txt_f_valid = whether that entry is used/valid. (Input)
 *    startX = The start of the X values. (Input)
 *    startY = The start of the Y values. (Input)
 *     subNx = The Nx dimension of the subgrid (Input)
 *     subNy = The Ny dimension of the subgrid (Input)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *  12/2002 Arthur Taylor (MDL/RSIS): Created to optimize part of ParseGrid.
 *   5/2003 AAT: Added ability to see if wxType occurs.  If so sets table
 *          valid to 2, otherwise leaves it at 1.  If table valid is 0 then
 *          sets value to missing value (if applicable).
 *   2/2004 AAT: Added the subgrid capability.
 *
 * NOTES
 * 1) Don't have to check if value became missing value, because we can check
 *    if missing falls in the range of the min/max converted units.  If
 *    missing does fall in that range we need to move missing.
 *    (See f_readjust in ParseGrid)
 *****************************************************************************
 */
static void ParseGridSecMiss (gridAttribType *attrib, double *grib_Data,
                              sInt4 Nx, sInt4 Ny, sInt4 *iain,
                              double unitM, double unitB, sInt4 *missCnt,
                              uChar f_txtType, uInt4 txt_dataLen,
                              uChar *txt_f_valid,
                              int startX, int startY, int subNx, int subNy)
{
   sInt4 x, y;          /* Where we are in the grid. */
   double value;        /* The data in the new units. */
   uChar f_maxmin = 0;  /* Flag if max/min is valid yet. */
   uInt4 index;         /* Current index into Wx table. */
   sInt4 *itemp = nullptr;
   float *ftemp = nullptr;
/*   float *ain = (float *) iain;*/

   /* Resolve possibility that the data is an integer or a float and find
    * max/min values. (see note 1) */
   for (y = 0; y < subNy; y++) {
      if (((startY + y - 1) < 0) || ((startY + y - 1) >= Ny)) {
         for (x = 0; x < subNx; x++) {
            *grib_Data++ = attrib->missPri;
            (*missCnt)++;
         }
      } else {
         if (attrib->fieldType) {
            itemp = iain + (startY + y - 1) * Nx + (startX - 1);
         } else {
            ftemp = reinterpret_cast<float *>(iain) + (startY + y - 1) * Nx + (startX - 1);
         }
         for (x = 0; x < subNx; x++) {
            if (((startX + x - 1) < 0) || ((startX + x - 1) >= Nx)) {
               *grib_Data++ = attrib->missPri;
               (*missCnt)++;
            } else {
               if (attrib->fieldType) {
                  value = (*itemp++);
               } else {
                  value = (*ftemp++);
               }

               /* Make sure value is not a missing value when converting
                * units, and while computing max/min. */
               if ((value == attrib->missPri) || (value == attrib->missSec)) {
                  (*missCnt)++;
               } else {
                  /* Convert the units. */
                  if (unitM == -10) {
                     value = pow (10.0, value);
                  } else {
                     value = unitM * value + unitB;
                  }
                  if (f_txtType) {
                     index = (uInt4) value;
                     if (index < txt_dataLen) {
                        if (txt_f_valid[index]) {
                           txt_f_valid[index] = 2;
                        } else {
                           /* Table is not valid here so set value to missPri
                            */
                           value = attrib->missPri;
                           (*missCnt)++;
                        }
                     }
                  }
                  if ((!f_txtType) || (value != attrib->missPri)) {
                     if (f_maxmin) {
                        if (value < attrib->min) {
                           attrib->min = value;
                        } else if (value > attrib->max) {
                           attrib->max = value;
                        }
                     } else {
                        attrib->min = attrib->max = value;
                        f_maxmin = 1;
                     }
                  }
               }
               *grib_Data++ = value;
            }
         }
      }
   }
   attrib->f_maxmin = f_maxmin;
}

/*****************************************************************************
 * ParseGrid() --
 *
 * Arthur Taylor / MDL
 *
 * PURPOSE
 *   To walk through the 2 possible grids (and possible bitmap) created by
 * UNPK_GRIB2, and combine the info into 1 grid, at the same time computing
 * the min/max values in the grid.  It uses gridAttrib info for the missing values
 * and it then updates the gridAttrib structure for the min/max values that it
 * found.
 *   It also uses scan, and ScanIndex2XY, to parse the data and organize the
 * Grib_Data so that 0,0 is the lower left part of the grid, it then traverses
 * the row and then moved up to the next row starting on the left.
 *
 * ARGUMENTS
 *       attrib = sect 5 structure already filled in by ParseSect5 (In/Output)
 *    Grib_Data = The place to store the grid data. (Output)
 * grib_DataLen = The current size of Grib_Data (can increase) (Input/Output)
 *       Nx, Ny = The dimensions of the grid (Input)
 *         scan = How to walk through the original grid. (Input)
 *         iain = Place to find data if it is an Integer (or float). (Input)
 *      ibitmap = Flag stating the data has a bitmap for missing values (In)
 *           ib = Where to find the bitmap if we have one (Input)
 *        unitM = M in unit conversion equation y(new) = m x(orig) + b (Input)
 *        unitB = B in unit conversion equation y(new) = m x(orig) + b (Input)
 *    f_txtType = true if we have a valid wx/hazard type. (Input)
 *  txt_dataLen = Length of text table
 *  txt_f_valid = whether that entry is used/valid. (Input)
 *    f_subGrid = True if we have a subgrid, false if not. (Input)
 * startX stopX = The bounds of the subgrid in X. (0,-1) means full grid (In)
 * startY stopY = The bounds of the subgrid in Y. (0,-1) means full grid (In)
 *
 * FILES/DATABASES: None
 *
 * RETURNS: void
 *
 * HISTORY
 *   9/2002 Arthur Taylor (MDL/RSIS): Created.
 *  11/2002 AAT: Added unit conversion to metaparse.c
 *  12/2002 AAT: Optimized first loop to make it assume scan 0100 (64)
 *         (valid 99.9%), but still have slow loop for generic case.
 *   5/2003 AAT: Added ability to see if wxType occurs.  If so sets table
 *          valid to 2, otherwise leaves it at 1.  If table valid is 0 then
 *          sets value to missing value (if applicable).
 *   7/2003 AAT: added check if f_maxmin before checking if missing was in
 *          range of max, min for "readjust" check.
 *   2/2004 AAT: Added startX / startY / stopX / stopY
 *   5/2004 AAT: Found out that I used the opposite definition for bitmap
 *          0 = missing, 1 = valid.
 *
 * NOTES
 *****************************************************************************
 */
void ParseGrid (VSILFILE *fp, gridAttribType *attrib, double **Grib_Data,
                uInt4 *grib_DataLen, uInt4 Nx, uInt4 Ny, int scan,
                sInt4 nd2x3, sInt4 *iain, sInt4 ibitmap, sInt4 *ib, double unitM,
                double unitB, uChar f_txtType, uInt4 txt_dataLen,
                uChar *txt_f_valid,
                CPL_UNUSED uChar f_subGrid,
                int startX, int startY, int stopX, int stopY)
{
   double xmissp;       /* computed missing value needed for ibitmap = 1,
                         * Also used if unit conversion causes confusion
                         * over_ missing values. */
   double xmisss;       /* Used if unit conversion causes confusion over
                         * missing values. */
   uChar f_readjust;    /* True if unit conversion caused confusion over
                         * missing values. */
   uInt4 scanIndex;     /* Where we are in the original grid. */
   sInt4 x, y;          /* Where we are in a grid of scan value 0100 */
   uInt4 newIndex;      /* x,y in a 1 dimensional array. */
   double value;        /* The data in the new units. */
   /* A pointer to Grib_Data for ease of manipulation. */
   double *grib_Data = nullptr;
   sInt4 missCnt = 0;   /* Number of detected missing values. */
   uInt4 index;         /* Current index into Wx table. */
   float *ain = reinterpret_cast<float *>(iain);
   uInt4 subNx;         /* The Nx dimension of the subgrid. */
   uInt4 subNy;         /* The Ny dimension of the subgrid. */

   subNx = stopX - startX + 1;
   subNy = stopY - startY + 1;

   myAssert (((!f_subGrid) && (subNx == Nx)) || (f_subGrid));
   myAssert (((!f_subGrid) && (subNy == Ny)) || (f_subGrid));

   if( subNy == 0 || subNx > UINT_MAX / subNy )
   {
       errSprintf ("Too large raster");
       *grib_DataLen = 0;
       *Grib_Data = nullptr;
       return;
   }

   const uInt4 subNxNy = subNx * subNy;
   if (subNxNy > *grib_DataLen) {

      if( subNxNy > 100 * 1024 * 1024 )
      {
          vsi_l_offset curPos = VSIFTellL(fp);
          VSIFSeekL(fp, 0, SEEK_END);
          vsi_l_offset fileSize = VSIFTellL(fp);
          VSIFSeekL(fp, curPos, SEEK_SET);
          // allow a compression ratio of 1:1000
          if( subNxNy / 1000 > fileSize )
          {
            errSprintf ("ERROR: File too short\n");
            *grib_DataLen = 0;
            *Grib_Data = nullptr;
            return;
          }
      }

      double* newData = nullptr;
      const size_t nBufferSize = subNxNy * sizeof (double);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
      if( nBufferSize > static_cast<size_t>(INT_MAX) )
      {
          errSprintf ("Memory allocation failed due to being bigger than 2 GB in fuzzing mode");
      }
      else
#endif
      if( nBufferSize / sizeof(double) == subNxNy)
      {
        newData = (double *) realloc ((void *) (*Grib_Data), nBufferSize);
      }
      if( newData == nullptr )
      {
          errSprintf ("Memory allocation failed");
          free(*Grib_Data);
          *Grib_Data = nullptr;
          *grib_DataLen = 0;
          return;
      }
      *grib_DataLen = subNxNy;
      *Grib_Data = newData;
   }
   grib_Data = *Grib_Data;

   /* Resolve possibility that the data is an integer or a float, find
    * max/min values, and do unit conversion. (see note 1) */
   if (scan == 64) {
      if (attrib->f_miss == 0) {
         ParseGridNoMiss (attrib, grib_Data, Nx, Ny, iain, unitM, unitB,
                          f_txtType, txt_dataLen, txt_f_valid, startX, startY, subNx, subNy);
      } else if (attrib->f_miss == 1) {
         ParseGridPrimMiss (attrib, grib_Data, Nx, Ny, iain, unitM, unitB,
                            &missCnt, f_txtType, txt_dataLen, txt_f_valid, startX, startY,
                            subNx, subNy);
      } else if (attrib->f_miss == 2) {
         ParseGridSecMiss (attrib, grib_Data, Nx, Ny, iain, unitM, unitB,
                           &missCnt, f_txtType, txt_dataLen, txt_f_valid, startX, startY, subNx,
                           subNy);
      }
   } else {
      /* Internally we use scan = 0100.  Scan is usually 0100 from the
       * unpacker library, but if scan is not, the following code converts
       * it.  We optimized the previous (scan 0100) case by calling a
       * dedicated procedure.  Here we don't since for scan != 0100, we
       * would_ need a different unpacker library, which is extremely
       * unlikely. */
      for (scanIndex = 0; scanIndex < (uInt4)nd2x3 && scanIndex < Nx * Ny; scanIndex++) {
         if (attrib->fieldType) {
            value = iain[scanIndex];
         } else {
            value = ain[scanIndex];
         }
         /* Make sure value is not a missing value when converting units, and
          * while computing max/min. */
         if ((attrib->f_miss == 0) ||
             ((attrib->f_miss == 1) && (value != attrib->missPri)) ||
             ((attrib->f_miss == 2) && (value != attrib->missPri) &&
              (value != attrib->missSec))) {
            /* Convert the units. */
            if (unitM == -10) {
               value = pow (10.0, value);
            } else {
               value = unitM * value + unitB;
            }
            /* Don't have to check if value became missing value, because we
             * can check if missing falls in the range of min/max.  If
             * missing does fall in that range we need to move missing. See
             * f_readjust */
            if (f_txtType) {
               index = (uInt4) value;
               if (index < txt_dataLen) {
                  if (txt_f_valid[index] == 1) {
                     txt_f_valid[index] = 2;
                  } else if (txt_f_valid[index] == 0) {
                     /* Table is not valid here so set value to missPri */
                     if (attrib->f_miss != 0) {
                        value = attrib->missPri;
                        missCnt++;
                     } else {
                        /* No missing value, so use index = WxType->dataLen */
                        /* No... set f_valid to 3 so we know we used this
                         * invalid element, then handle it in degrib2.c ::
                         * ReadGrib2Record() where we set it back to 0. */
                        txt_f_valid[index] = 3;
                     }
                  }
               }
            }
            if ((!f_txtType) ||
                ((attrib->f_miss == 0) || (value != attrib->missPri))) {
               if (attrib->f_maxmin) {
                  if (value < attrib->min) {
                     attrib->min = value;
                  } else if (value > attrib->max) {
                     attrib->max = value;
                  }
               } else {
                  attrib->min = attrib->max = value;
                  attrib->f_maxmin = 1;
               }
            }
         } else {
            missCnt++;
         }
         ScanIndex2XY (scanIndex, &x, &y, scan, Nx, Ny);
         /* ScanIndex returns value as if scan was 0100 */
         newIndex = (uInt4)(x - 1) + (uInt4)(y - 1) * Nx;
         grib_Data[newIndex] = value;
      }
   }

   /* Deal with possibility that unit conversion ended up with valid numbers
    * being interpreted as missing. */
   f_readjust = 0;
   xmissp = attrib->missPri;
   xmisss = attrib->missSec;
   if (attrib->f_maxmin) {
      if ((attrib->f_miss == 1) || (attrib->f_miss == 2)) {
         if ((attrib->missPri >= attrib->min) &&
             (attrib->missPri <= attrib->max)) {
            xmissp = attrib->max + 1;
            f_readjust = 1;
         }
         if (attrib->f_miss == 2) {
            if ((attrib->missSec >= attrib->min) &&
                (attrib->missSec <= attrib->max)) {
               xmisss = attrib->max + 2;
               f_readjust = 1;
            }
         }
      }
   }

   /* Walk through the grid, resetting the missing values, as determined by
    * the original grid. */
   if (f_readjust) {
      for (scanIndex = 0; scanIndex < (uInt4)nd2x3 && scanIndex < Nx * Ny; scanIndex++) {
         ScanIndex2XY (scanIndex, &x, &y, scan, Nx, Ny);
         /* ScanIndex returns value as if scan was 0100 */
         newIndex = (uInt4)(x - 1) + (uInt4)(y - 1) * Nx;
         if (attrib->fieldType) {
            value = iain[scanIndex];
         } else {
            value = ain[scanIndex];
         }
         if (value == attrib->missPri) {
            grib_Data[newIndex] = xmissp;
         } else if ((attrib->f_miss == 2) && (value == attrib->missSec)) {
            grib_Data[newIndex] = xmisss;
         }
      }
      attrib->missPri = xmissp;
      if (attrib->f_miss == 2) {
         attrib->missSec = xmisss;
      }
   }

   /* Resolve bitmap (if there is one) in the data. */
   if (ibitmap) {
      attrib->f_maxmin = 0;
      if ((attrib->f_miss != 1) && (attrib->f_miss != 2)) {
         missCnt = 0;
         /* Figure out a missing value. */
         xmissp = 9999;
#ifdef unreachable
         if (attrib->f_maxmin) {
            if ((xmissp <= attrib->max) && (xmissp >= attrib->min)) {
               xmissp = attrib->max + 1;
            }
         }
#endif
         /* embed the missing value. */
         for (scanIndex = 0; scanIndex < (uInt4)nd2x3 && scanIndex < Nx * Ny; scanIndex++) {
            ScanIndex2XY (scanIndex, &x, &y, scan, Nx, Ny);
            /* ScanIndex returns value as if scan was 0100 */
            newIndex = (uInt4)(x - 1) + (uInt4)(y - 1) * Nx;
            /* Corrected this on 5/10/2004 */
            if (ib[scanIndex] != 1) {
               grib_Data[newIndex] = xmissp;
               missCnt++;
            } else {
               if (!attrib->f_maxmin) {
                  attrib->f_maxmin = 1;
                  attrib->max = attrib->min = grib_Data[newIndex];
               } else {
                  if (attrib->max < grib_Data[newIndex])
                     attrib->max = grib_Data[newIndex];
                  if (attrib->min > grib_Data[newIndex])
                     attrib->min = grib_Data[newIndex];
               }
            }
         }
         attrib->f_miss = 1;
         attrib->missPri = xmissp;
      }
      if (!attrib->f_maxmin) {
         attrib->f_maxmin = 1;
         attrib->max = attrib->min = xmissp;
      }
   }
   attrib->numMiss = missCnt;
}

#ifdef unused_by_GDAL
typedef struct {
   double value;
   int cnt;
} freqType;

static int freqCompare (const void *A, const void *B)
{
   const freqType *a = (freqType *) A;
   const freqType *b = (freqType *) B;

   if (a->value < b->value)
      return -1;
   if (a->value > b->value)
      return 1;
   return 0;
}

void FreqPrint (char **ans, double *Data, sInt4 DataLen, sInt4 Nx,
                sInt4 Ny, sChar decimal, char *comment)
{
   int x, y, i;
   double *ptr = NULL;
   double value;
   freqType *freq = NULL;
   int numFreq = 0;
   char format[20];

   myAssert (*ans == NULL);

   if ((Nx < 0) || (Ny < 0) || (Nx * Ny > DataLen)) {
      return;
   }

   ptr = Data;
   for (y = 0; y < Ny; y++) {
      for (x = 0; x < Nx; x++) {
         /* 2/28/2006 Introduced value to round before putting the data in
          * the Freq table. */
         value = myRound (*ptr, decimal);
         for (i = 0; i < numFreq; i++) {
            if (value == freq[i].value) {
               freq[i].cnt++;
               break;
            }
         }
         if (i == numFreq) {
            numFreq++;
            freq = (freqType *) realloc (freq, numFreq * sizeof (freqType));
            freq[i].value = value;
            freq[i].cnt = 1;
         }
         ptr++;
      }
   }

   if( freq )
     qsort (freq, numFreq, sizeof (freq[0]), freqCompare);

   mallocSprintf (ans, "%s | count\n", comment);
   snprintf (format, sizeof(format), "%%.%df | %%d\n", decimal);
   for (i = 0; i < numFreq; i++) {
      reallocSprintf (ans, format, myRound (freq[i].value, decimal),
                      freq[i].cnt);
   }
   free (freq);
}
#endif
