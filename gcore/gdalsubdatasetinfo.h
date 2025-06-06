/***************************************************************************
  gdal_subdatasetinfo.h - GDALSubdatasetInfo

 ---------------------
 begin                : 21.7.2023
 copyright            : (C) 2023 by Alessndro Pasotti
 email                : elpaso@itopen.it
 ***************************************************************************
 *                                                                         *
 * SPDX-License-Identifier: MIT
 *                                                                         *
 ***************************************************************************/
#ifndef GDALSUBDATASETINFO_H
#define GDALSUBDATASETINFO_H

#include "cpl_port.h"
#include <string>

/**
 * The GDALSubdatasetInfo abstract class provides methods to extract and
 * manipulate subdataset information from a file name that contains subdataset
 * information.
 *
 * Drivers offering this functionality must override the parseFileName() method.
 */
struct CPL_DLL GDALSubdatasetInfo
{

  public:
    /**
     * @brief Construct a GDALSubdatasetInfo object from a subdataset file descriptor.
     * @param fileName          The subdataset file name descriptor.
     */
    GDALSubdatasetInfo(const std::string &fileName);

    virtual ~GDALSubdatasetInfo();

    /**
 * @brief Returns the unquoted and unescaped path component of the complete file descriptor
 *        stripping any subdataset, prefix and additional information.
 * @return                      The path to the file
 * @since                       GDAL 3.8
 */
    std::string GetPathComponent() const;

    /**
 * @brief Replaces the path component of the complete file descriptor
 *        by keeping the subdataset and any other component unaltered.
 *        The returned string must be freed with CPLFree()
 * @param newPathName           New path name with no subdataset information.
 * @note                        This method does not check if the subdataset actually exists.
 * @return                      The original file name with the old path component replaced by newPathName.
 * @since                       GDAL 3.8
 */
    std::string ModifyPathComponent(const std::string &newPathName) const;

    /**
 * @brief Returns the subdataset component of the file name.
 *
 * @return                      The subdataset name
 * @since                       GDAL 3.8
 */
    std::string GetSubdatasetComponent() const;

    //! @cond Doxygen_Suppress
  protected:
    /**
     * This method is called once to parse the fileName and populate the member variables.
     * It must be reimplemented by concrete derived classes.
     */
    virtual void parseFileName() = 0;

    /**
     * Adds double quotes to paths and escape double quotes inside the path.
     */
    static std::string quote(const std::string &path);

    /**
     * Removes double quotes and unescape double quotes.
     */
    static std::string unquote(const std::string &path);

    //! The original unparsed complete file name passed to the constructor (e.g. GPKG:/path/to/file.gpkg:layer_name)
    std::string m_fileName;
    //! The unmodified path component of the file name (e.g. "\"C:\\path\\to\\file.gpkg\"", "/path/to/file.gpkg")
    std::string m_pathComponent;
    //! The unquoted and unescaped path component of the file name (e.g. "C:\\path\\to\\file.gpkg", "/path/to/file.gpkg")
    std::string m_cleanedPathComponent;
    //! The subdataset component (e.g. layer_name)
    std::string m_subdatasetComponent;
    //! The driver prefix component (e.g. GPKG)
    std::string m_driverPrefixComponent;
    //! If the path is enclosed in double quotes.
    bool m_isQuoted = false;

  private:
    mutable bool m_initialized = false;

    void init() const;

    //! @endcond
};

#endif  // GDALSUBDATASETINFO_H
