/******************************************************************************
 *
 * Purpose:  Various public (documented) utility functions.
 *
 ******************************************************************************
 * Copyright (c) 2009
 * PCI Geomatics, 90 Allstate Parkway, Markham, Ontario, Canada.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#include "pcidsk_config.h"
#include "pcidsk_types.h"
#include "pcidsk_exception.h"
#include "core/pcidsk_utils.h"
#include <cstdlib>
#include <cstring>

using namespace PCIDSK;

/************************************************************************/
/*                            DataTypeSize()                            */
/************************************************************************/

/**
 * Return size of data type.
 *
 * Note that type CHN_BIT exists to represent one bit backed data from
 * bitmap segments, but because the return of this functions is measured
 * in bytes, the size of a CHN_BIT pixel cannot be properly returned (one
 * eighth of a byte), so "1" is returned instead.
 *
 * @param chan_type the channel type enumeration value.
 *
 * @return the size of the passed data type in bytes, or zero for unknown
 * values.
 */

int PCIDSK::DataTypeSize( eChanType chan_type )

{
    switch( chan_type )
    {
      case CHN_8U:
        return 1;
      case CHN_16S:
        return 2;
      case CHN_16U:
        return 2;
      case CHN_32S:
        return 4;
      case CHN_32U:
        return 4;
      case CHN_32R:
        return 4;
      case CHN_64S:
        return 8;
      case CHN_64U:
        return 8;
      case CHN_64R:
        return 8;
      case CHN_C16U:
        return 4;
      case CHN_C16S:
        return 4;
      case CHN_C32U:
        return 8;
      case CHN_C32S:
        return 8;
      case CHN_C32R:
        return 8;
      case CHN_BIT:
        return 1; // not really accurate!
      default:
        return 0;
    }
}

/************************************************************************/
/*                            DataTypeName()                            */
/************************************************************************/

/**
 * Return name for the data type.
 *
 * The returned values are suitable for display to people, and matches
 * the portion of the name after the underscore (i.e. "8U" for CHN_8U.
 *
 * @param chan_type the channel type enumeration value to be translated.
 *
 * @return a string representing the data type.
 */

const char * PCIDSK::DataTypeName( eChanType chan_type )

{
    switch( chan_type )
    {
      case CHN_8U:
        return "8U";
      case CHN_16S:
        return "16S";
      case CHN_16U:
        return "16U";
      case CHN_32S:
        return "32S";
      case CHN_32U:
        return "32U";
      case CHN_32R:
        return "32R";
      case CHN_64S:
        return "64S";
      case CHN_64U:
        return "64U";
      case CHN_64R:
        return "64R";
      case CHN_C16U:
        return "C16U";
      case CHN_C16S:
        return "C16S";
      case CHN_C32U:
        return "C32U";
      case CHN_C32S:
        return "C32S";
      case CHN_C32R:
        return "C32R";
      case CHN_BIT:
        return "BIT";
      default:
        return "UNK";
    }
}

/************************************************************************/
/*                      GetDataTypeFromName()                           */
/************************************************************************/

/**
 * @brief Return the segment type code based on the contents of type_name
 *
 * @param pszDataType the type name, as a string
 *
 * @return the channel type code
 */
eChanType PCIDSK::GetDataTypeFromName(const char * pszDataType)
{
    if (strstr(pszDataType, "8U") != nullptr)
        return CHN_8U;
    if (strstr(pszDataType, "C16U") != nullptr)
        return CHN_C16U;
    if (strstr(pszDataType, "C16S") != nullptr)
        return CHN_C16S;
    if (strstr(pszDataType, "C32U") != nullptr)
        return CHN_C32U;
    if (strstr(pszDataType, "C32S") != nullptr)
        return CHN_C32S;
    if (strstr(pszDataType, "C32R") != nullptr)
        return CHN_C32R;
    if (strstr(pszDataType, "16U") != nullptr)
        return CHN_16U;
    if (strstr(pszDataType, "16S") != nullptr)
        return CHN_16S;
    if (strstr(pszDataType, "32U") != nullptr)
        return CHN_32U;
    if (strstr(pszDataType, "32S") != nullptr)
        return CHN_32S;
    if (strstr(pszDataType, "32R") != nullptr)
        return CHN_32R;
    if (strstr(pszDataType, "64U") != nullptr)
        return CHN_64U;
    if (strstr(pszDataType, "64S") != nullptr)
        return CHN_64S;
    if (strstr(pszDataType, "64R") != nullptr)
        return CHN_64R;
    if (strstr(pszDataType, "BIT") != nullptr)
        return CHN_BIT;

    return CHN_UNKNOWN;
}

/************************************************************************/
/*                       IsDataTypeComplex()                           */
/************************************************************************/

/**
 * @brief Return whether or not the data type is complex
 *
 * @param type the type
 *
 * @return true if the data type is complex, false otherwise
 */
bool PCIDSK::IsDataTypeComplex(eChanType type)
{
    switch(type)
    {
    case CHN_C32R:
    case CHN_C32U:
    case CHN_C32S:
    case CHN_C16U:
    case CHN_C16S:
        return true;
    default:
        return false;
    }
}

/************************************************************************/
/*                          SegmentTypeName()                           */
/************************************************************************/

/**
 * Return name for segment type.
 *
 * Returns a short name for the segment type code passed in.  This is normally
 * the portion of the enumeration name that comes after the underscore - i.e.
 * "BIT" for SEG_BIT.
 *
 * @param type the segment type code.
 *
 * @return the string for the segment type.
 */

const char * PCIDSK::SegmentTypeName( int type )
{
    switch( type )
    {
      case SEG_BIT:
        return "BIT";
      case SEG_VEC:
        return "VEC";
      case SEG_SIG:
        return "SIG";
      case SEG_TEX:
        return "TEX";
      case SEG_GEO:
        return "GEO";
      case SEG_ORB:
        return "ORB";
      case SEG_LUT:
        return "LUT";
      case SEG_PCT:
        return "PCT";
      case SEG_BLUT:
        return "BLUT";
      case SEG_BPCT:
        return "BPCT";
      case SEG_BIN:
        return "BIN";
      case SEG_ARR:
        return "ARR";
      case SEG_SYS:
        return "SYS";
      case SEG_GCPOLD:
        return "GCPOLD";
      case SEG_GCP2:
        return "GCP2";
      default:
        return "UNKNOWN";
    }
}
