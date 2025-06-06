/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Class for representing a whole feature, and layer schemas.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999,  Les Technologies SoftMap Inc.
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_FEATURE_H_INCLUDED
#define OGR_FEATURE_H_INCLUDED

#include "cpl_atomic_ops.h"
#include "gdal_fwd.h"
#include "ogr_featurestyle.h"
#include "ogr_geometry.h"
#include "ogr_geomcoordinateprecision.h"

#include <cstddef>

#include <exception>
#include <memory>
#include <string>
#include <vector>

/**
 * \file ogr_feature.h
 *
 * Simple feature classes.
 */

class OGRStyleTable;

/************************************************************************/
/*                             OGRFieldDefn                             */
/************************************************************************/

/**
 * Definition of an attribute of an OGRFeatureDefn. A field is described by :
 * <ul>
 * <li>a name. See SetName() / GetNameRef()</li>
 * <li>an alternative name (optional): alternative descriptive name for the
 * field (sometimes referred to as an "alias"). See SetAlternativeName() /
 * GetAlternativeNameRef()</li> <li>a type: OFTString, OFTInteger, OFTReal, ...
 * See SetType() / GetType()</li> <li>a subtype (optional): OFSTBoolean, ... See
 * SetSubType() / GetSubType()</li> <li>a width (optional): maximal number of
 * characters. See SetWidth() / GetWidth()</li> <li>a precision (optional):
 * number of digits after decimal point. See SetPrecision() /
 * GetPrecision()</li> <li>a NOT NULL constraint (optional). See SetNullable() /
 * IsNullable()</li> <li>a UNIQUE constraint (optional). See SetUnique() /
 * IsUnique()</li> <li>a default value (optional).  See SetDefault() /
 * GetDefault()</li> <li>a boolean to indicate whether it should be ignored when
 * retrieving features.  See SetIgnored() / IsIgnored()</li> <li>a field domain
 * name (optional). See SetDomainName() / Get DomainName()</li>
 * </ul>
 *
 * Note that once a OGRFieldDefn has been added to a layer definition with
 * OGRLayer::AddFieldDefn(), its setter methods should not be called on the
 * object returned with OGRLayer::GetLayerDefn()->GetFieldDefn(). Instead,
 * OGRLayer::AlterFieldDefn() should be called on a new instance of
 * OGRFieldDefn, for drivers that support AlterFieldDefn().
 */

class CPL_DLL OGRFieldDefn
{
  private:
    char *pszName;
    char *pszAlternativeName;
    OGRFieldType eType;
    OGRJustification eJustify;
    int nWidth;  // Zero is variable.
    int nPrecision;
    char *pszDefault;

    int bIgnore;
    OGRFieldSubType eSubType;

    int bNullable;
    int bUnique;

    // Used by drivers (GPKG) to track generated fields
    bool m_bGenerated = false;

    std::string m_osDomainName{};  // field domain name. Might be empty

    std::string m_osComment{};  // field comment. Might be empty

    int m_nTZFlag = OGR_TZFLAG_UNKNOWN;
    bool m_bSealed = false;

  public:
    OGRFieldDefn(const char *, OGRFieldType);
    explicit OGRFieldDefn(const OGRFieldDefn *);
    ~OGRFieldDefn();

    // Copy constructor
    OGRFieldDefn(const OGRFieldDefn &oOther);

    // Copy assignment operator
    OGRFieldDefn &operator=(const OGRFieldDefn &oOther);

    void SetName(const char *);

    const char *GetNameRef() const
    {
        return pszName;
    }

    void SetAlternativeName(const char *);

    const char *GetAlternativeNameRef() const
    {
        return pszAlternativeName;
    }

    OGRFieldType GetType() const
    {
        return eType;
    }

    void SetType(OGRFieldType eTypeIn);
    static const char *GetFieldTypeName(OGRFieldType);
    static OGRFieldType GetFieldTypeByName(const char *);

    OGRFieldSubType GetSubType() const
    {
        return eSubType;
    }

    void SetSubType(OGRFieldSubType eSubTypeIn);
    static const char *GetFieldSubTypeName(OGRFieldSubType);
    static OGRFieldSubType GetFieldSubTypeByName(const char *);

    OGRJustification GetJustify() const
    {
        return eJustify;
    }

    void SetJustify(OGRJustification eJustifyIn)
    {
        eJustify = eJustifyIn;
    }

    int GetWidth() const
    {
        return nWidth;
    }

    void SetWidth(int nWidthIn);

    int GetPrecision() const
    {
        return nPrecision;
    }

    void SetPrecision(int nPrecisionIn);

    int GetTZFlag() const
    {
        return m_nTZFlag;
    }

    void SetTZFlag(int nTZFlag);

    void Set(const char *, OGRFieldType, int = 0, int = 0,
             OGRJustification = OJUndefined);

    void SetDefault(const char *);
    const char *GetDefault() const;
    int IsDefaultDriverSpecific() const;

    int IsIgnored() const
    {
        return bIgnore;
    }

    void SetIgnored(int bIgnoreIn)
    {
        bIgnore = bIgnoreIn;
    }

    int IsNullable() const
    {
        return bNullable;
    }

    void SetNullable(int bNullableIn);

    int IsUnique() const
    {
        return bUnique;
    }

    /**
     * @brief Return whether the field is a generated field.
     *
     * At time of writing, only the GeoPackage and PG drivers fill that information. Consequently,
     * only a returned value equal to TRUE can be fully trusted.
     * @return TRUE if the field is a generated field, FALSE otherwise.
     * @since GDAL 3.11
     */
    bool IsGenerated() const
    {
        return m_bGenerated;
    }

    /**
     * @brief SetGenerated set the field generated status.
     * @param bGeneratedIn TRUE if the field is a generated field, FALSE otherwise.
     * @since GDAL 3.11
     */
    void SetGenerated(bool bGeneratedIn)
    {
        m_bGenerated = bGeneratedIn;
    }

    void SetUnique(int bUniqueIn);

    const std::string &GetDomainName() const
    {
        return m_osDomainName;
    }

    void SetDomainName(const std::string &osDomainName);

    const std::string &GetComment() const
    {
        return m_osComment;
    }

    void SetComment(const std::string &osComment);

    int IsSame(const OGRFieldDefn *) const;

    /** Convert a OGRFieldDefn* to a OGRFieldDefnH.
     * @since GDAL 2.3
     */
    static inline OGRFieldDefnH ToHandle(OGRFieldDefn *poFieldDefn)
    {
        return reinterpret_cast<OGRFieldDefnH>(poFieldDefn);
    }

    /** Convert a OGRFieldDefnH to a OGRFieldDefn*.
     * @since GDAL 2.3
     */
    static inline OGRFieldDefn *FromHandle(OGRFieldDefnH hFieldDefn)
    {
        return reinterpret_cast<OGRFieldDefn *>(hFieldDefn);
    }

    void Seal();

    void Unseal();

    /*! @cond Doxygen_Suppress */
    struct CPL_DLL TemporaryUnsealer
    {
      private:
        OGRFieldDefn *m_poFieldDefn = nullptr;
        CPL_DISALLOW_COPY_ASSIGN(TemporaryUnsealer)
      public:
        explicit TemporaryUnsealer(OGRFieldDefn *poFieldDefn)
            : m_poFieldDefn(poFieldDefn)
        {
            m_poFieldDefn->Unseal();
        }

        TemporaryUnsealer(TemporaryUnsealer &&) = default;
        TemporaryUnsealer &operator=(TemporaryUnsealer &&) = default;

        ~TemporaryUnsealer()
        {
            m_poFieldDefn->Seal();
        }

        OGRFieldDefn *operator->()
        {
            return m_poFieldDefn;
        }
    };

    /*! @endcond */

    TemporaryUnsealer GetTemporaryUnsealer();
};

#ifdef GDAL_COMPILATION
/** Return an object that temporary unseals the OGRFieldDefn.
 *
 * The returned object calls Unseal() initially, and when it is destroyed
 * it calls Seal().
 *
 * This method should only be called by driver implementations.
 *
 * Usage: whileUnsealing(poFieldDefn)->some_method();
 *
 * @since GDAL 3.9
 */
inline OGRFieldDefn::TemporaryUnsealer whileUnsealing(OGRFieldDefn *object)
{
    return object->GetTemporaryUnsealer();
}
#endif

/************************************************************************/
/*                          OGRGeomFieldDefn                            */
/************************************************************************/

/**
 * Definition of a geometry field of an OGRFeatureDefn. A geometry field is
 * described by :
 * <ul>
 * <li>a name. See SetName() / GetNameRef()</li>
 * <li>a type: wkbPoint, wkbLineString, ... See SetType() / GetType()</li>
 * <li>a spatial reference system (optional). See SetSpatialRef() /
 * GetSpatialRef()</li> <li>a NOT NULL constraint (optional). See SetNullable()
 * / IsNullable()</li> <li>a boolean to indicate whether it should be ignored
 * when retrieving features.  See SetIgnored() / IsIgnored()</li>
 * </ul>
 *
 * Note that once a OGRGeomFieldDefn has been added to a layer definition with
 * OGRLayer::AddGeomFieldDefn(), its setter methods should not be called on the
 * object returned with OGRLayer::GetLayerDefn()->GetGeomFieldDefn(). Instead,
 * OGRLayer::AlterGeomFieldDefn() should be called on a new instance of
 * OGRFieldDefn, for drivers that support AlterFieldDefn().
 *
 * @since OGR 1.11
 */

class CPL_DLL OGRGeomFieldDefn
{
  protected:
    //! @cond Doxygen_Suppress
    char *pszName = nullptr;
    OGRwkbGeometryType eGeomType =
        wkbUnknown; /* all values possible except wkbNone */
    mutable const OGRSpatialReference *poSRS = nullptr;

    int bIgnore = false;
    mutable int bNullable = true;
    bool m_bSealed = false;
    OGRGeomCoordinatePrecision m_oCoordPrecision{};

    void Initialize(const char *, OGRwkbGeometryType);
    //! @endcond

  public:
    OGRGeomFieldDefn(const char *pszNameIn, OGRwkbGeometryType eGeomTypeIn);
    explicit OGRGeomFieldDefn(const OGRGeomFieldDefn *);
    virtual ~OGRGeomFieldDefn();

    // Copy constructor
    OGRGeomFieldDefn(const OGRGeomFieldDefn &oOther);

    // Copy assignment operator
    OGRGeomFieldDefn &operator=(const OGRGeomFieldDefn &oOther);

    void SetName(const char *);

    const char *GetNameRef() const
    {
        return pszName;
    }

    OGRwkbGeometryType GetType() const
    {
        return eGeomType;
    }

    void SetType(OGRwkbGeometryType eTypeIn);

    virtual const OGRSpatialReference *GetSpatialRef() const;
    void SetSpatialRef(const OGRSpatialReference *poSRSIn);

    int IsIgnored() const
    {
        return bIgnore;
    }

    void SetIgnored(int bIgnoreIn)
    {
        bIgnore = bIgnoreIn;
    }

    int IsNullable() const
    {
        return bNullable;
    }

    void SetNullable(int bNullableIn);

    const OGRGeomCoordinatePrecision &GetCoordinatePrecision() const
    {
        return m_oCoordPrecision;
    }

    void SetCoordinatePrecision(const OGRGeomCoordinatePrecision &prec);

    int IsSame(const OGRGeomFieldDefn *) const;

    /** Convert a OGRGeomFieldDefn* to a OGRGeomFieldDefnH.
     * @since GDAL 2.3
     */
    static inline OGRGeomFieldDefnH ToHandle(OGRGeomFieldDefn *poGeomFieldDefn)
    {
        return reinterpret_cast<OGRGeomFieldDefnH>(poGeomFieldDefn);
    }

    /** Convert a OGRGeomFieldDefnH to a OGRGeomFieldDefn*.
     * @since GDAL 2.3
     */
    static inline OGRGeomFieldDefn *FromHandle(OGRGeomFieldDefnH hGeomFieldDefn)
    {
        return reinterpret_cast<OGRGeomFieldDefn *>(hGeomFieldDefn);
    }

    void Seal();

    void Unseal();

    /*! @cond Doxygen_Suppress */
    struct CPL_DLL TemporaryUnsealer
    {
      private:
        OGRGeomFieldDefn *m_poFieldDefn = nullptr;
        CPL_DISALLOW_COPY_ASSIGN(TemporaryUnsealer)
      public:
        explicit TemporaryUnsealer(OGRGeomFieldDefn *poFieldDefn)
            : m_poFieldDefn(poFieldDefn)
        {
            m_poFieldDefn->Unseal();
        }

        TemporaryUnsealer(TemporaryUnsealer &&) = default;
        TemporaryUnsealer &operator=(TemporaryUnsealer &&) = default;

        ~TemporaryUnsealer()
        {
            m_poFieldDefn->Seal();
        }

        OGRGeomFieldDefn *operator->()
        {
            return m_poFieldDefn;
        }
    };

    /*! @endcond */

    TemporaryUnsealer GetTemporaryUnsealer();
};

#ifdef GDAL_COMPILATION
/** Return an object that temporary unseals the OGRGeomFieldDefn.
 *
 * The returned object calls Unseal() initially, and when it is destroyed
 * it calls Seal().
 *
 * This method should only be called by driver implementations.
 *
 * Usage: whileUnsealing(poGeomFieldDefn)->some_method();
 *
 * @since GDAL 3.9
 */
inline OGRGeomFieldDefn::TemporaryUnsealer
whileUnsealing(OGRGeomFieldDefn *object)
{
    return object->GetTemporaryUnsealer();
}
#endif

/************************************************************************/
/*                            OGRFeatureDefn                            */
/************************************************************************/

/**
 * Definition of a feature class or feature layer.
 *
 * This object contains schema information for a set of OGRFeatures.  In
 * table based systems, an OGRFeatureDefn is essentially a layer.  In more
 * object oriented approaches (such as SF CORBA) this can represent a class
 * of features but doesn't necessarily relate to all of a layer, or just one
 * layer.
 *
 * This object also can contain some other information such as a name and
 * potentially other metadata.
 *
 * It is essentially a collection of field descriptions (OGRFieldDefn class).
 * Starting with GDAL 1.11, in addition to attribute fields, it can also
 * contain multiple geometry fields (OGRGeomFieldDefn class).
 *
 * It is reasonable for different translators to derive classes from
 * OGRFeatureDefn with additional translator specific information.
 *
 * Note that adding, modifying, removing, reordering a OGRFieldDefn (or a
 * OGRGeomFieldDefn) from/to a OGRFeatureDefn that belongs to a OGRLayer should
 * not be done through the OGRFeatureDefn::AddFieldDefn(),
 * OGRFeatureDefn::DeleteFieldDefn() or OGRFeatureDefn::ReorderFieldDefns()
 * methods, but rather through OGRLayer::CreateField(),
 * OGRLayer::AlterFieldDefn() or OGRLayer::ReorderFields(), for drivers that
 * support those operations.
 */

class CPL_DLL OGRFeatureDefn
{
  protected:
    //! @cond Doxygen_Suppress
    volatile int nRefCount = 0;

    mutable std::vector<std::unique_ptr<OGRFieldDefn>> apoFieldDefn{};
    mutable std::vector<std::unique_ptr<OGRGeomFieldDefn>> apoGeomFieldDefn{};

    char *pszFeatureClassName = nullptr;

    bool bIgnoreStyle = false;

    friend class TemporaryUnsealer;
    bool m_bSealed = false;
    int m_nTemporaryUnsealCount = 0;
    //! @endcond

  public:
    explicit OGRFeatureDefn(const char *pszName = nullptr);
    virtual ~OGRFeatureDefn();

    void SetName(const char *pszName);
    virtual const char *GetName() const;

    virtual int GetFieldCount() const;
    virtual OGRFieldDefn *GetFieldDefn(int i);
    virtual const OGRFieldDefn *GetFieldDefn(int i) const;
    virtual int GetFieldIndex(const char *) const;
    int GetFieldIndexCaseSensitive(const char *) const;

    //! @cond Doxygen_Suppress
    /** Helper class to iterate over non-geometry fields.
     *
     * Note: fields should not be added or removed while iterating over them.
     */
    struct CPL_DLL Fields
    {
      private:
        OGRFeatureDefn *m_poFDefn;

      public:
        inline explicit Fields(OGRFeatureDefn *poFDefn) : m_poFDefn(poFDefn)
        {
        }

        struct CPL_DLL ConstIterator
        {
          private:
            OGRFeatureDefn *m_poFDefn;
            int m_nIdx;

          public:
            inline ConstIterator(OGRFeatureDefn *poFDefn, int nIdx)
                : m_poFDefn(poFDefn), m_nIdx(nIdx)
            {
            }

            inline const OGRFieldDefn *operator*() const
            {
                return m_poFDefn->GetFieldDefn(m_nIdx);
            }

            inline ConstIterator &operator++()
            {
                m_nIdx++;
                return *this;
            }

            inline bool operator!=(const ConstIterator &it) const
            {
                return m_nIdx != it.m_nIdx;
            }
        };

        inline ConstIterator begin()
        {
            return ConstIterator(m_poFDefn, 0);
        }

        inline ConstIterator end()
        {
            return ConstIterator(m_poFDefn, m_poFDefn->GetFieldCount());
        }

        inline size_t size() const
        {
            return static_cast<std::size_t>(m_poFDefn->GetFieldCount());
        }

        inline OGRFieldDefn *operator[](size_t i)
        {
            return m_poFDefn->GetFieldDefn(static_cast<int>(i));
        }

        inline const OGRFieldDefn *operator[](size_t i) const
        {
            return m_poFDefn->GetFieldDefn(static_cast<int>(i));
        }
    };

    //! @endcond

    /** Return an object that can be used to iterate over non-geometry fields.
        \verbatim
        for( const auto* poFieldDefn: poFeatureDefn->GetFields() )
        {
            // do something
        }
        \endverbatim

        @since GDAL 3.7
     */
    inline Fields GetFields()
    {
        return Fields(this);
    }

    //! @cond Doxygen_Suppress
    // That method should only be called if there's a guarantee that
    // GetFieldCount() has been called before
    int GetFieldCountUnsafe() const
    {
        return static_cast<int>(apoFieldDefn.size());
    }

    // Those methods don't check i is n range.
    OGRFieldDefn *GetFieldDefnUnsafe(int i)
    {
        if (apoFieldDefn.empty())
            GetFieldDefn(i);
        return apoFieldDefn[static_cast<std::size_t>(i)].get();
    }

    const OGRFieldDefn *GetFieldDefnUnsafe(int i) const
    {
        if (apoFieldDefn.empty())
            GetFieldDefn(i);
        return apoFieldDefn[static_cast<std::size_t>(i)].get();
    }

    //! @endcond

    virtual void AddFieldDefn(const OGRFieldDefn *);
    virtual OGRErr DeleteFieldDefn(int iField);

    /**
     * @brief StealFieldDefn takes ownership of the field definition at index detaching
     *        it from the feature definition.
     * This is an advanced method designed to be only used for driver implementations.
     * @param iField index of the field definition to detach.
     * @return a unique pointer to the detached field definition or nullptr if the index is out of range.
     * @since GDAL 3.11
     */
    virtual std::unique_ptr<OGRFieldDefn> StealFieldDefn(int iField);

    virtual void AddFieldDefn(std::unique_ptr<OGRFieldDefn> &&poFieldDefn);

    virtual OGRErr ReorderFieldDefns(const int *panMap);

    /**
     * @brief StealGeomFieldDefn takes ownership of the the geometry field definition at index
     *        detaching it from the feature definition.
     * This is an advanced method designed to be only used for driver implementations.
     * @param iField index of the geometry field definition to detach.
     * @return a unique pointer to the detached geometry field definition or nullptr if the index is out of range.
     * @since GDAL 3.11
     */
    virtual std::unique_ptr<OGRGeomFieldDefn> StealGeomFieldDefn(int iField);

    virtual int GetGeomFieldCount() const;
    virtual OGRGeomFieldDefn *GetGeomFieldDefn(int i);
    virtual const OGRGeomFieldDefn *GetGeomFieldDefn(int i) const;
    virtual int GetGeomFieldIndex(const char *) const;

    //! @cond Doxygen_Suppress
    /** Helper class to iterate over geometry fields.
     *
     * Note: fields should not be added or removed while iterating over them.
     */
    struct CPL_DLL GeomFields
    {
      private:
        OGRFeatureDefn *m_poFDefn;

      public:
        inline explicit GeomFields(OGRFeatureDefn *poFDefn) : m_poFDefn(poFDefn)
        {
        }

        struct CPL_DLL ConstIterator
        {
          private:
            OGRFeatureDefn *m_poFDefn;
            int m_nIdx;

          public:
            inline ConstIterator(OGRFeatureDefn *poFDefn, int nIdx)
                : m_poFDefn(poFDefn), m_nIdx(nIdx)
            {
            }

            inline const OGRGeomFieldDefn *operator*() const
            {
                return m_poFDefn->GetGeomFieldDefn(m_nIdx);
            }

            inline ConstIterator &operator++()
            {
                m_nIdx++;
                return *this;
            }

            inline bool operator!=(const ConstIterator &it) const
            {
                return m_nIdx != it.m_nIdx;
            }
        };

        inline ConstIterator begin()
        {
            return ConstIterator(m_poFDefn, 0);
        }

        inline ConstIterator end()
        {
            return ConstIterator(m_poFDefn, m_poFDefn->GetGeomFieldCount());
        }

        inline size_t size() const
        {
            return static_cast<std::size_t>(m_poFDefn->GetGeomFieldCount());
        }

        inline OGRGeomFieldDefn *operator[](size_t i)
        {
            return m_poFDefn->GetGeomFieldDefn(static_cast<int>(i));
        }

        inline const OGRGeomFieldDefn *operator[](size_t i) const
        {
            return m_poFDefn->GetGeomFieldDefn(static_cast<int>(i));
        }
    };

    //! @endcond

    /** Return an object that can be used to iterate over geometry fields.
        \verbatim
        for( const auto* poGeomFieldDefn: poFeatureDefn->GetGeomFields() )
        {
            // do something
        }
        \endverbatim

        @since GDAL 3.7
     */
    inline GeomFields GetGeomFields()
    {
        return GeomFields(this);
    }

    virtual void AddGeomFieldDefn(const OGRGeomFieldDefn *);
    virtual void AddGeomFieldDefn(std::unique_ptr<OGRGeomFieldDefn> &&);
    virtual OGRErr DeleteGeomFieldDefn(int iGeomField);

    virtual OGRwkbGeometryType GetGeomType() const;
    virtual void SetGeomType(OGRwkbGeometryType);

    virtual OGRFeatureDefn *Clone() const;

    int Reference()
    {
        return CPLAtomicInc(&nRefCount);
    }

    int Dereference()
    {
        return CPLAtomicDec(&nRefCount);
    }

    int GetReferenceCount() const
    {
        return nRefCount;
    }

    void Release();

    virtual int IsGeometryIgnored() const;
    virtual void SetGeometryIgnored(int bIgnore);

    virtual bool IsStyleIgnored() const
    {
        return bIgnoreStyle;
    }

    virtual void SetStyleIgnored(bool bIgnore)
    {
        bIgnoreStyle = bIgnore;
    }

    virtual int IsSame(const OGRFeatureDefn *poOtherFeatureDefn) const;

    //! @cond Doxygen_Suppress
    void ReserveSpaceForFields(int nFieldCountIn);
    //! @endcond

    std::vector<int> ComputeMapForSetFrom(const OGRFeatureDefn *poSrcFDefn,
                                          bool bForgiving = true) const;

    static OGRFeatureDefn *CreateFeatureDefn(const char *pszName = nullptr);
    static void DestroyFeatureDefn(OGRFeatureDefn *);

    /** Convert a OGRFeatureDefn* to a OGRFeatureDefnH.
     * @since GDAL 2.3
     */
    static inline OGRFeatureDefnH ToHandle(OGRFeatureDefn *poFeatureDefn)
    {
        return reinterpret_cast<OGRFeatureDefnH>(poFeatureDefn);
    }

    /** Convert a OGRFeatureDefnH to a OGRFeatureDefn*.
     * @since GDAL 2.3
     */
    static inline OGRFeatureDefn *FromHandle(OGRFeatureDefnH hFeatureDefn)
    {
        return reinterpret_cast<OGRFeatureDefn *>(hFeatureDefn);
    }

    void Seal(bool bSealFields);

    void Unseal(bool bUnsealFields);

    /*! @cond Doxygen_Suppress */
    struct CPL_DLL TemporaryUnsealer
    {
      private:
        OGRFeatureDefn *m_poFeatureDefn = nullptr;
        bool m_bSealFields = false;
        CPL_DISALLOW_COPY_ASSIGN(TemporaryUnsealer)
      public:
        explicit TemporaryUnsealer(OGRFeatureDefn *poFeatureDefn,
                                   bool bSealFields);

        TemporaryUnsealer(TemporaryUnsealer &&) = default;
        TemporaryUnsealer &operator=(TemporaryUnsealer &&) = default;

        ~TemporaryUnsealer();

        OGRFeatureDefn *operator->()
        {
            return m_poFeatureDefn;
        }
    };

    /*! @endcond */

    TemporaryUnsealer GetTemporaryUnsealer(bool bSealFields = true);

  private:
    CPL_DISALLOW_COPY_ASSIGN(OGRFeatureDefn)
};

#ifdef GDAL_COMPILATION
/** Return an object that temporary unseals the OGRFeatureDefn
 *
 * The returned object calls Unseal() initially, and when it is destroyed
 * it calls Seal().
 * This method should be called on a OGRFeatureDefn that has been sealed
 * previously.
 * GetTemporaryUnsealer() calls may be nested, in which case only the first
 * one has an effect (similarly to a recursive mutex locked in a nested way
 * from the same thread).
 *
 * This method should only be called by driver implementations.
 *
 * Usage: whileUnsealing(poFeatureDefn)->some_method();
 *
 * @param bSealFields Whether fields and geometry fields should be unsealed and
 *                    resealed.
 *                    This is generally desirabled, but in case of deferred
 *                    resolution of them, this parameter should be set to false.
 * @since GDAL 3.9
 */
inline OGRFeatureDefn::TemporaryUnsealer whileUnsealing(OGRFeatureDefn *object,
                                                        bool bSealFields = true)
{
    return object->GetTemporaryUnsealer(bSealFields);
}
#endif

/************************************************************************/
/*                              OGRFeature                              */
/************************************************************************/

/**
 * A simple feature, including geometry and attributes.
 */

class CPL_DLL OGRFeature
{
  private:
    GIntBig nFID;
    OGRFeatureDefn *poDefn;
    OGRGeometry **papoGeometries;
    OGRField *pauFields;
    char *m_pszNativeData;
    char *m_pszNativeMediaType;

    bool SetFieldInternal(int i, const OGRField *puValue);

  protected:
    //! @cond Doxygen_Suppress
    mutable char *m_pszStyleString;
    mutable OGRStyleTable *m_poStyleTable;
    mutable char *m_pszTmpFieldValue;
    //! @endcond

    bool CopySelfTo(OGRFeature *poNew) const;

  public:
    explicit OGRFeature(OGRFeatureDefn *);
    virtual ~OGRFeature();

    /** Field value. */
    class CPL_DLL FieldValue
    {
        friend class OGRFeature;
        struct Private;
        std::unique_ptr<Private> m_poPrivate;

        FieldValue(OGRFeature *poFeature, int iFieldIndex);
        FieldValue(const OGRFeature *poFeature, int iFieldIndex);
        FieldValue(const FieldValue &oOther) = delete;
        FieldValue &Assign(const FieldValue &oOther);

      public:
        //! @cond Doxygen_Suppress
        ~FieldValue();

        FieldValue &operator=(FieldValue &&oOther);
        //! @endcond

        /** Set a field value from another one. */
        FieldValue &operator=(const FieldValue &oOther);
        /** Set an integer value to the field. */
        FieldValue &operator=(int nVal);
        /** Set an integer value to the field. */
        FieldValue &operator=(GIntBig nVal);
        /** Set a real value to the field. */
        FieldValue &operator=(double dfVal);
        /** Set a string value to the field. */
        FieldValue &operator=(const char *pszVal);
        /** Set a string value to the field. */
        FieldValue &operator=(const std::string &osVal);
        /** Set an array of integer to the field. */
        FieldValue &operator=(const std::vector<int> &oArray);
        /** Set an array of big integer to the field. */
        FieldValue &operator=(const std::vector<GIntBig> &oArray);
        /** Set an array of double to the field. */
        FieldValue &operator=(const std::vector<double> &oArray);
        /** Set an array of strings to the field. */
        FieldValue &operator=(const std::vector<std::string> &oArray);
        /** Set an array of strings to the field. */
        FieldValue &operator=(CSLConstList papszValues);
        /** Set a null value to the field. */
        void SetNull();
        /** Unset the field. */
        void clear();

        /** Unset the field. */
        void Unset()
        {
            clear();
        }

        /** Set date time value/ */
        void SetDateTime(int nYear, int nMonth, int nDay, int nHour = 0,
                         int nMinute = 0, float fSecond = 0.f, int nTZFlag = 0);

        /** Return field index. */
        int GetIndex() const;
        /** Return field definition. */
        const OGRFieldDefn *GetDefn() const;

        /** Return field name. */
        const char *GetName() const
        {
            return GetDefn()->GetNameRef();
        }

        /** Return field type. */
        OGRFieldType GetType() const
        {
            return GetDefn()->GetType();
        }

        /** Return field subtype. */
        OGRFieldSubType GetSubType() const
        {
            return GetDefn()->GetSubType();
        }

        /** Return whether the field value is unset/empty. */
        // cppcheck-suppress functionStatic
        bool empty() const
        {
            return IsUnset();
        }

        /** Return whether the field value is unset/empty. */
        // cppcheck-suppress functionStatic
        bool IsUnset() const;

        /** Return whether the field value is null. */
        // cppcheck-suppress functionStatic
        bool IsNull() const;

        /** Return the raw field value */
        const OGRField *GetRawValue() const;

        /** Return the integer value.
         * Only use that method if and only if GetType() == OFTInteger.
         */
        // cppcheck-suppress functionStatic
        int GetInteger() const
        {
            return GetRawValue()->Integer;
        }

        /** Return the 64-bit integer value.
         * Only use that method if and only if GetType() == OFTInteger64.
         */
        // cppcheck-suppress functionStatic
        GIntBig GetInteger64() const
        {
            return GetRawValue()->Integer64;
        }

        /** Return the double value.
         * Only use that method if and only if GetType() == OFTReal.
         */
        // cppcheck-suppress functionStatic
        double GetDouble() const
        {
            return GetRawValue()->Real;
        }

        /** Return the string value.
         * Only use that method if and only if GetType() == OFTString.
         */
        // cppcheck-suppress functionStatic
        const char *GetString() const
        {
            return GetRawValue()->String;
        }

        /** Return the date/time/datetime value. */
        bool GetDateTime(int *pnYear, int *pnMonth, int *pnDay, int *pnHour,
                         int *pnMinute, float *pfSecond, int *pnTZFlag) const;

        /** Return the field value as integer, with potential conversion */
        operator int() const
        {
            return GetAsInteger();
        }

        /** Return the field value as 64-bit integer, with potential conversion
         */
        operator GIntBig() const
        {
            return GetAsInteger64();
        }

        /** Return the field value as double, with potential conversion */
        operator double() const
        {
            return GetAsDouble();
        }

        /** Return the field value as string, with potential conversion */
        operator const char *() const
        {
            return GetAsString();
        }

        /** Return the field value as integer list, with potential conversion */
        operator const std::vector<int> &() const
        {
            return GetAsIntegerList();
        }

        /** Return the field value as 64-bit integer list, with potential
         * conversion */
        operator const std::vector<GIntBig> &() const
        {
            return GetAsInteger64List();
        }

        /** Return the field value as double list, with potential conversion */
        operator const std::vector<double> &() const
        {
            return GetAsDoubleList();
        }

        /** Return the field value as string list, with potential conversion */
        operator const std::vector<std::string> &() const
        {
            return GetAsStringList();
        }

        /** Return the field value as string list, with potential conversion */
        operator CSLConstList() const;

        /** Return the field value as integer, with potential conversion */
        int GetAsInteger() const;
        /** Return the field value as 64-bit integer, with potential conversion
         */
        GIntBig GetAsInteger64() const;
        /** Return the field value as double, with potential conversion */
        double GetAsDouble() const;
        /** Return the field value as string, with potential conversion */
        const char *GetAsString() const;
        /** Return the field value as integer list, with potential conversion */
        const std::vector<int> &GetAsIntegerList() const;
        /** Return the field value as 64-bit integer list, with potential
         * conversion */
        const std::vector<GIntBig> &GetAsInteger64List() const;
        /** Return the field value as double list, with potential conversion */
        const std::vector<double> &GetAsDoubleList() const;
        /** Return the field value as string list, with potential conversion */
        const std::vector<std::string> &GetAsStringList() const;
    };

    /** Field value iterator class. */
    class CPL_DLL ConstFieldIterator
    {
        friend class OGRFeature;
        struct Private;
        std::unique_ptr<Private> m_poPrivate;

        ConstFieldIterator(const OGRFeature *poSelf, int nPos);

      public:
        //! @cond Doxygen_Suppress
        ConstFieldIterator(
            ConstFieldIterator &&oOther) noexcept;  // declared but not defined.
        // Needed for gcc 5.4 at least
        ~ConstFieldIterator();
        const FieldValue &operator*() const;
        ConstFieldIterator &operator++();
        bool operator!=(const ConstFieldIterator &it) const;
        //! @endcond
    };

    /** Return begin of field value iterator.
     *
     * Using this iterator for standard range-based loops is safe, but
     * due to implementation limitations, you shouldn't try to access
     * (dereference) more than one iterator step at a time, since you will get
     * a reference to the same object (FieldValue) at each iteration step.
     *
     * \code{.cpp}
     * for( auto&& oField: poFeature )
     * {
     *      std::cout << oField.GetIndex() << "," << oField.GetName()<< ": " <<
     * oField.GetAsString() << std::endl;
     * }
     * \endcode
     *
     * @since GDAL 2.3
     */
    ConstFieldIterator begin() const;
    /** Return end of field value iterator. */
    ConstFieldIterator end() const;

    const FieldValue operator[](int iField) const;
    FieldValue operator[](int iField);

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
#endif

    /** Exception raised by operator[](const char*) when a field is not found.
     */
    class FieldNotFoundException : public std::exception
    {
    };

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    const FieldValue operator[](const char *pszFieldName) const;
    FieldValue operator[](const char *pszFieldName);

    OGRFeatureDefn *GetDefnRef()
    {
        return poDefn;
    }

    const OGRFeatureDefn *GetDefnRef() const
    {
        return poDefn;
    }

    //! @cond Doxygen_Suppress
    void SetFDefnUnsafe(OGRFeatureDefn *poNewFDefn);
    //! @endcond

    OGRErr SetGeometryDirectly(OGRGeometry *);
    OGRErr SetGeometry(const OGRGeometry *);
    OGRErr SetGeometry(std::unique_ptr<OGRGeometry>);
    OGRGeometry *GetGeometryRef();
    const OGRGeometry *GetGeometryRef() const;
    OGRGeometry *StealGeometry() CPL_WARN_UNUSED_RESULT;

    int GetGeomFieldCount() const
    {
        return poDefn->GetGeomFieldCount();
    }

    OGRGeomFieldDefn *GetGeomFieldDefnRef(int iField)
    {
        return poDefn->GetGeomFieldDefn(iField);
    }

    const OGRGeomFieldDefn *GetGeomFieldDefnRef(int iField) const
    {
        return poDefn->GetGeomFieldDefn(iField);
    }

    int GetGeomFieldIndex(const char *pszName) const
    {
        return poDefn->GetGeomFieldIndex(pszName);
    }

    OGRGeometry *GetGeomFieldRef(int iField);
    const OGRGeometry *GetGeomFieldRef(int iField) const;
    OGRGeometry *StealGeometry(int iField);
    OGRGeometry *GetGeomFieldRef(const char *pszFName);
    const OGRGeometry *GetGeomFieldRef(const char *pszFName) const;
    OGRErr SetGeomFieldDirectly(int iField, OGRGeometry *);
    OGRErr SetGeomField(int iField, const OGRGeometry *);
    OGRErr SetGeomField(int iField, std::unique_ptr<OGRGeometry>);

    void Reset();

    OGRFeature *Clone() const CPL_WARN_UNUSED_RESULT;
    virtual OGRBoolean Equal(const OGRFeature *poFeature) const;

    int GetFieldCount() const
    {
        return poDefn->GetFieldCount();
    }

    const OGRFieldDefn *GetFieldDefnRef(int iField) const
    {
        return poDefn->GetFieldDefn(iField);
    }

    OGRFieldDefn *GetFieldDefnRef(int iField)
    {
        return poDefn->GetFieldDefn(iField);
    }

    int GetFieldIndex(const char *pszName) const
    {
        return poDefn->GetFieldIndex(pszName);
    }

    int IsFieldSet(int iField) const;

    void UnsetField(int iField);

    bool IsFieldNull(int iField) const;

    void SetFieldNull(int iField);

    bool IsFieldSetAndNotNull(int iField) const;

    OGRField *GetRawFieldRef(int i)
    {
        return pauFields + i;
    }

    const OGRField *GetRawFieldRef(int i) const
    {
        return pauFields + i;
    }

    int GetFieldAsInteger(int i) const;
    GIntBig GetFieldAsInteger64(int i) const;
    double GetFieldAsDouble(int i) const;
    const char *GetFieldAsString(int i) const;
    const char *GetFieldAsISO8601DateTime(int i,
                                          CSLConstList papszOptions) const;
    const int *GetFieldAsIntegerList(int i, int *pnCount) const;
    const GIntBig *GetFieldAsInteger64List(int i, int *pnCount) const;
    const double *GetFieldAsDoubleList(int i, int *pnCount) const;
    char **GetFieldAsStringList(int i) const;
    GByte *GetFieldAsBinary(int i, int *pnCount) const;
    int GetFieldAsDateTime(int i, int *pnYear, int *pnMonth, int *pnDay,
                           int *pnHour, int *pnMinute, int *pnSecond,
                           int *pnTZFlag) const;
    int GetFieldAsDateTime(int i, int *pnYear, int *pnMonth, int *pnDay,
                           int *pnHour, int *pnMinute, float *pfSecond,
                           int *pnTZFlag) const;
    char *GetFieldAsSerializedJSon(int i) const;

    //! @cond Doxygen_Suppress
    bool IsFieldSetUnsafe(int i) const
    {
        return !(pauFields[i].Set.nMarker1 == OGRUnsetMarker &&
                 pauFields[i].Set.nMarker2 == OGRUnsetMarker &&
                 pauFields[i].Set.nMarker3 == OGRUnsetMarker);
    }

    bool IsFieldNullUnsafe(int i) const
    {
        return (pauFields[i].Set.nMarker1 == OGRNullMarker &&
                pauFields[i].Set.nMarker2 == OGRNullMarker &&
                pauFields[i].Set.nMarker3 == OGRNullMarker);
    }

    bool IsFieldSetAndNotNullUnsafe(int i) const
    {
        return IsFieldSetUnsafe(i) && !IsFieldNullUnsafe(i);
    }

    // Those methods should only be called on a field that is of the type
    // consistent with the value, and that is set.
    int GetFieldAsIntegerUnsafe(int i) const
    {
        return pauFields[i].Integer;
    }

    GIntBig GetFieldAsInteger64Unsafe(int i) const
    {
        return pauFields[i].Integer64;
    }

    double GetFieldAsDoubleUnsafe(int i) const
    {
        return pauFields[i].Real;
    }

    const char *GetFieldAsStringUnsafe(int i) const
    {
        return pauFields[i].String;
    }

    //! @endcond

    int GetFieldAsInteger(const char *pszFName) const
    {
        return GetFieldAsInteger(GetFieldIndex(pszFName));
    }

    GIntBig GetFieldAsInteger64(const char *pszFName) const
    {
        return GetFieldAsInteger64(GetFieldIndex(pszFName));
    }

    double GetFieldAsDouble(const char *pszFName) const
    {
        return GetFieldAsDouble(GetFieldIndex(pszFName));
    }

    const char *GetFieldAsString(const char *pszFName) const
    {
        return GetFieldAsString(GetFieldIndex(pszFName));
    }

    const char *GetFieldAsISO8601DateTime(const char *pszFName,
                                          CSLConstList papszOptions) const
    {
        return GetFieldAsISO8601DateTime(GetFieldIndex(pszFName), papszOptions);
    }

    const int *GetFieldAsIntegerList(const char *pszFName, int *pnCount) const
    {
        return GetFieldAsIntegerList(GetFieldIndex(pszFName), pnCount);
    }

    const GIntBig *GetFieldAsInteger64List(const char *pszFName,
                                           int *pnCount) const
    {
        return GetFieldAsInteger64List(GetFieldIndex(pszFName), pnCount);
    }

    const double *GetFieldAsDoubleList(const char *pszFName, int *pnCount) const
    {
        return GetFieldAsDoubleList(GetFieldIndex(pszFName), pnCount);
    }

    char **GetFieldAsStringList(const char *pszFName) const
    {
        return GetFieldAsStringList(GetFieldIndex(pszFName));
    }

    void SetField(int i, int nValue);
    void SetField(int i, GIntBig nValue);
    void SetField(int i, double dfValue);
    void SetField(int i, const char *pszValue);
    void SetField(int i, int nCount, const int *panValues);
    void SetField(int i, int nCount, const GIntBig *panValues);
    void SetField(int i, int nCount, const double *padfValues);
    void SetField(int i, const char *const *papszValues);
    void SetField(int i, const OGRField *puValue);
    void SetField(int i, int nCount, const void *pabyBinary);
    void SetField(int i, int nYear, int nMonth, int nDay, int nHour = 0,
                  int nMinute = 0, float fSecond = 0.f, int nTZFlag = 0);

    //! @cond Doxygen_Suppress
    // Those methods should only be called on a field that is of the type
    // consistent with the value, and in a unset state.
    void SetFieldSameTypeUnsafe(int i, int nValue)
    {
        pauFields[i].Integer = nValue;
        pauFields[i].Set.nMarker2 = 0;
        pauFields[i].Set.nMarker3 = 0;
    }

    void SetFieldSameTypeUnsafe(int i, GIntBig nValue)
    {
        pauFields[i].Integer64 = nValue;
    }

    void SetFieldSameTypeUnsafe(int i, double dfValue)
    {
        pauFields[i].Real = dfValue;
    }

    void SetFieldSameTypeUnsafe(int i, char *pszValueTransferred)
    {
        pauFields[i].String = pszValueTransferred;
    }

    //! @endcond

    void SetField(const char *pszFName, int nValue)
    {
        SetField(GetFieldIndex(pszFName), nValue);
    }

    void SetField(const char *pszFName, GIntBig nValue)
    {
        SetField(GetFieldIndex(pszFName), nValue);
    }

    void SetField(const char *pszFName, double dfValue)
    {
        SetField(GetFieldIndex(pszFName), dfValue);
    }

    void SetField(const char *pszFName, const char *pszValue)
    {
        SetField(GetFieldIndex(pszFName), pszValue);
    }

    void SetField(const char *pszFName, int nCount, const int *panValues)
    {
        SetField(GetFieldIndex(pszFName), nCount, panValues);
    }

    void SetField(const char *pszFName, int nCount, const GIntBig *panValues)
    {
        SetField(GetFieldIndex(pszFName), nCount, panValues);
    }

    void SetField(const char *pszFName, int nCount, const double *padfValues)
    {
        SetField(GetFieldIndex(pszFName), nCount, padfValues);
    }

    void SetField(const char *pszFName, const char *const *papszValues)
    {
        SetField(GetFieldIndex(pszFName), papszValues);
    }

    void SetField(const char *pszFName, const OGRField *puValue)
    {
        SetField(GetFieldIndex(pszFName), puValue);
    }

    void SetField(const char *pszFName, int nYear, int nMonth, int nDay,
                  int nHour = 0, int nMinute = 0, float fSecond = 0.f,
                  int nTZFlag = 0)
    {
        SetField(GetFieldIndex(pszFName), nYear, nMonth, nDay, nHour, nMinute,
                 fSecond, nTZFlag);
    }

    GIntBig GetFID() const
    {
        return nFID;
    }

    virtual OGRErr SetFID(GIntBig nFIDIn);

    void DumpReadable(FILE *, CSLConstList papszOptions = nullptr) const;
    std::string DumpReadableAsString(CSLConstList papszOptions = nullptr) const;

    OGRErr SetFrom(const OGRFeature *, int bForgiving = TRUE);
    OGRErr SetFrom(const OGRFeature *, const int *panMap, int bForgiving = TRUE,
                   bool bUseISO8601ForDateTimeAsString = false);
    OGRErr SetFieldsFrom(const OGRFeature *, const int *panMap,
                         int bForgiving = TRUE,
                         bool bUseISO8601ForDateTimeAsString = false);

    //! @cond Doxygen_Suppress
    OGRErr RemapFields(OGRFeatureDefn *poNewDefn, const int *panRemapSource);
    void AppendField();
    OGRErr RemapGeomFields(OGRFeatureDefn *poNewDefn,
                           const int *panRemapSource);
    //! @endcond

    int Validate(int nValidateFlags, int bEmitError) const;
    void FillUnsetWithDefault(int bNotNullableOnly, char **papszOptions);

    bool SerializeToBinary(std::vector<GByte> &abyBuffer) const;
    bool DeserializeFromBinary(const GByte *pabyBuffer, size_t nSize);

    virtual const char *GetStyleString() const;
    virtual void SetStyleString(const char *);
    virtual void SetStyleStringDirectly(char *);

    /** Return style table.
     * @return style table.
     */
    virtual OGRStyleTable *GetStyleTable() const
    {
        return m_poStyleTable;
    } /* f.i.x.m.e: add a const qualifier for return type */

    virtual void SetStyleTable(OGRStyleTable *poStyleTable);
    virtual void SetStyleTableDirectly(OGRStyleTable *poStyleTable);

    const char *GetNativeData() const
    {
        return m_pszNativeData;
    }

    const char *GetNativeMediaType() const
    {
        return m_pszNativeMediaType;
    }

    void SetNativeData(const char *pszNativeData);
    void SetNativeMediaType(const char *pszNativeMediaType);

    static OGRFeature *CreateFeature(OGRFeatureDefn *);
    static void DestroyFeature(OGRFeature *);

    /** Convert a OGRFeature* to a OGRFeatureH.
     * @since GDAL 2.3
     */
    static inline OGRFeatureH ToHandle(OGRFeature *poFeature)
    {
        return reinterpret_cast<OGRFeatureH>(poFeature);
    }

    /** Convert a OGRFeatureH to a OGRFeature*.
     * @since GDAL 2.3
     */
    static inline OGRFeature *FromHandle(OGRFeatureH hFeature)
    {
        return reinterpret_cast<OGRFeature *>(hFeature);
    }

  private:
    CPL_DISALLOW_COPY_ASSIGN(OGRFeature)
};

//! @cond Doxygen_Suppress
struct CPL_DLL OGRFeatureUniquePtrDeleter
{
    void operator()(OGRFeature *) const;
};

//! @endcond

/** Unique pointer type for OGRFeature.
 * @since GDAL 2.3
 */
typedef std::unique_ptr<OGRFeature, OGRFeatureUniquePtrDeleter>
    OGRFeatureUniquePtr;

//! @cond Doxygen_Suppress
/** @see OGRFeature::begin() const */
inline OGRFeature::ConstFieldIterator begin(const OGRFeature *poFeature)
{
    return poFeature->begin();
}

/** @see OGRFeature::end() const */
inline OGRFeature::ConstFieldIterator end(const OGRFeature *poFeature)
{
    return poFeature->end();
}

/** @see OGRFeature::begin() const */
inline OGRFeature::ConstFieldIterator
begin(const OGRFeatureUniquePtr &poFeature)
{
    return poFeature->begin();
}

/** @see OGRFeature::end() const */
inline OGRFeature::ConstFieldIterator end(const OGRFeatureUniquePtr &poFeature)
{
    return poFeature->end();
}

//! @endcond

/************************************************************************/
/*                           OGRFieldDomain                             */
/************************************************************************/

/* clang-format off */
/**
 * Definition of a field domain.
 *
 * A field domain is a set of constraints that apply to one or several fields.
 *
 * This is a concept found in
 * <a href="https://desktop.arcgis.com/en/arcmap/latest/manage-data/geodatabases/an-overview-of-attribute-domains.htm">File
 * Geodatabase</a> or GeoPackage (using the <a href="http://www.geopackage.org/spec/#extension_schema">schema extension</a>)
 * for example.
 *
 * A field domain can be:
 * <ul>
 * <li>OGRCodedFieldDomain: an enumerated list of (code, value) tuples.</li>
 * <li>OGRRangeFieldDomain: a range constraint (min, max).</li>
 * <li>OGRGlobFieldDomain: a glob expression.</li>
 * </ul>
 *
 * @since GDAL 3.3
 */
/* clang-format on */

class CPL_DLL OGRFieldDomain
{
  protected:
    /*! @cond Doxygen_Suppress */
    std::string m_osName;
    std::string m_osDescription;
    OGRFieldDomainType m_eDomainType;
    OGRFieldType m_eFieldType;
    OGRFieldSubType m_eFieldSubType;
    OGRFieldDomainSplitPolicy m_eSplitPolicy = OFDSP_DEFAULT_VALUE;
    OGRFieldDomainMergePolicy m_eMergePolicy = OFDMP_DEFAULT_VALUE;

    OGRFieldDomain(const std::string &osName, const std::string &osDescription,
                   OGRFieldDomainType eDomainType, OGRFieldType eFieldType,
                   OGRFieldSubType eFieldSubType);
    /*! @endcond */

  public:
    /** Destructor.
     *
     * This is the same as the C function OGR_FldDomain_Destroy().
     */
    virtual ~OGRFieldDomain();

    /** Clone.
     *
     * Return a cloned object, or nullptr in case of error.
     */
    virtual OGRFieldDomain *Clone() const = 0;

    /** Get the name of the field domain.
     *
     * This is the same as the C function OGR_FldDomain_GetName().
     */
    const std::string &GetName() const
    {
        return m_osName;
    }

    /** Get the description of the field domain.
     * Empty string if there is none.
     *
     * This is the same as the C function OGR_FldDomain_GetDescription().
     */
    const std::string &GetDescription() const
    {
        return m_osDescription;
    }

    /** Get the type of the field domain.
     *
     * This is the same as the C function OGR_FldDomain_GetDomainType().
     */
    OGRFieldDomainType GetDomainType() const
    {
        return m_eDomainType;
    }

    /** Get the field type.
     *
     * This is the same as the C function OGR_FldDomain_GetFieldType().
     */
    OGRFieldType GetFieldType() const
    {
        return m_eFieldType;
    }

    /** Get the field subtype.
     *
     * This is the same as the C function OGR_FldDomain_GetFieldSubType().
     */
    OGRFieldSubType GetFieldSubType() const
    {
        return m_eFieldSubType;
    }

    /** Convert a OGRFieldDomain* to a OGRFieldDomainH. */
    static inline OGRFieldDomainH ToHandle(OGRFieldDomain *poFieldDomain)
    {
        return reinterpret_cast<OGRFieldDomainH>(poFieldDomain);
    }

    /** Convert a OGRFieldDomainH to a OGRFieldDomain*. */
    static inline OGRFieldDomain *FromHandle(OGRFieldDomainH hFieldDomain)
    {
        return reinterpret_cast<OGRFieldDomain *>(hFieldDomain);
    }

    /** Get the split policy.
     *
     * This is the same as the C function OGR_FldDomain_GetSplitPolicy().
     */
    OGRFieldDomainSplitPolicy GetSplitPolicy() const
    {
        return m_eSplitPolicy;
    }

    /** Set the split policy.
     *
     * This is the same as the C function OGR_FldDomain_SetSplitPolicy().
     */
    void SetSplitPolicy(OGRFieldDomainSplitPolicy policy)
    {
        m_eSplitPolicy = policy;
    }

    /** Get the merge policy.
     *
     * This is the same as the C function OGR_FldDomain_GetMergePolicy().
     */
    OGRFieldDomainMergePolicy GetMergePolicy() const
    {
        return m_eMergePolicy;
    }

    /** Set the merge policy.
     *
     * This is the same as the C function OGR_FldDomain_SetMergePolicy().
     */
    void SetMergePolicy(OGRFieldDomainMergePolicy policy)
    {
        m_eMergePolicy = policy;
    }
};

/** Definition of a coded / enumerated field domain.
 *
 * A code field domain is a domain for which only a limited set of codes,
 * associated with their expanded value, are allowed.
 * The type of the code should be the one of the field domain.
 */
class CPL_DLL OGRCodedFieldDomain final : public OGRFieldDomain
{
  private:
    std::vector<OGRCodedValue> m_asValues{};

    OGRCodedFieldDomain(const OGRCodedFieldDomain &) = delete;
    OGRCodedFieldDomain &operator=(const OGRCodedFieldDomain &) = delete;

  public:
    /** Constructor.
     *
     * This is the same as the C function OGR_CodedFldDomain_Create()
     * (except that the C function copies the enumeration, whereas the C++
     * method moves it)
     *
     * @param osName         Domain name.
     * @param osDescription  Domain description.
     * @param eFieldType     Field type. Generally numeric. Potentially
     * OFTDateTime
     * @param eFieldSubType  Field subtype.
     * @param asValues       Enumeration as (code, value) pairs.
     *                       Each code should appear only once, but it is the
     *                       responsibility of the user to check it.
     */
    OGRCodedFieldDomain(const std::string &osName,
                        const std::string &osDescription,
                        OGRFieldType eFieldType, OGRFieldSubType eFieldSubType,
                        std::vector<OGRCodedValue> &&asValues);

    ~OGRCodedFieldDomain() override;

    OGRCodedFieldDomain *Clone() const override;

    /** Get the enumeration as (code, value) pairs.
     * The end of the enumeration is signaled by code == NULL.
     *
     * This is the same as the C function OGR_CodedFldDomain_GetEnumeration().
     */
    const OGRCodedValue *GetEnumeration() const
    {
        return m_asValues.data();
    }
};

/** Definition of a numeric field domain with a range of validity for values.
 */
class CPL_DLL OGRRangeFieldDomain final : public OGRFieldDomain
{
  private:
    OGRField m_sMin;
    OGRField m_sMax;
    bool m_bMinIsInclusive;
    bool m_bMaxIsInclusive;

    OGRRangeFieldDomain(const OGRRangeFieldDomain &) = delete;
    OGRRangeFieldDomain &operator=(const OGRRangeFieldDomain &) = delete;

  public:
    /** Constructor.
     *
     * This is the same as the C function OGR_RangeFldDomain_Create().
     *
     * @param osName          Domain name.
     * @param osDescription   Domain description.
     * @param eFieldType      Field type.
     *                        One among OFTInteger, OFTInteger64, OFTReal or
     * OFTDateTime
     * @param eFieldSubType   Field subtype.
     * @param sMin            Minimum value.
     *                        Which member in the OGRField enum must be read
     *                        depends on the field type.
     *                        If no minimum is set (might not be supported by
     *                        all backends), then initialize the value with
     *                        OGR_RawField_SetUnset().
     * @param bMinIsInclusive Whether the minimum value is included in the
     * range.
     * @param sMax            Minimum value.
     *                        Which member in the OGRField enum must be read
     *                        depends on the field type.
     *                        If no maximum is set (might not be supported by
     *                        all backends), then initialize the value with
     *                        OGR_RawField_SetUnset().
     * @param bMaxIsInclusive Whether the minimum value is included in the
     * range.
     */
    OGRRangeFieldDomain(const std::string &osName,
                        const std::string &osDescription,
                        OGRFieldType eFieldType, OGRFieldSubType eFieldSubType,
                        const OGRField &sMin, bool bMinIsInclusive,
                        const OGRField &sMax, bool bMaxIsInclusive);

    OGRRangeFieldDomain *Clone() const override;

    /** Get the minimum value.
     *
     * Which member in the returned OGRField enum must be read depends on the
     * field type.
     *
     * If no minimum value is set, the OGR_RawField_IsUnset() will return true
     * when called on the result.
     *
     * This is the same as the C function OGR_RangeFldDomain_GetMin().
     *
     * @param bIsInclusiveOut set to true if the minimum is included in the
     * range.
     */
    const OGRField &GetMin(bool &bIsInclusiveOut) const
    {
        bIsInclusiveOut = m_bMinIsInclusive;
        return m_sMin;
    }

    /** Get the maximum value.
     *
     * Which member in the returned OGRField enum must be read depends on the
     * field type.
     *
     * If no maximum value is set, the OGR_RawField_IsUnset() will return true
     * when called on the result.
     *
     * This is the same as the C function OGR_RangeFldDomain_GetMax().
     *
     * @param bIsInclusiveOut set to true if the maximum is included in the
     * range.
     */
    const OGRField &GetMax(bool &bIsInclusiveOut) const
    {
        bIsInclusiveOut = m_bMaxIsInclusive;
        return m_sMax;
    }
};

/** Definition of a field domain for field content validated by a glob.
 *
 * Globs are matching expression like "*[a-z][0-1]?"
 */
class CPL_DLL OGRGlobFieldDomain final : public OGRFieldDomain
{
  private:
    std::string m_osGlob;

    OGRGlobFieldDomain(const OGRGlobFieldDomain &) = delete;
    OGRGlobFieldDomain &operator=(const OGRGlobFieldDomain &) = delete;

  public:
    /** Constructor.
     *
     * This is the same as the C function OGR_GlobFldDomain_Create().
     *
     * @param osName          Domain name.
     * @param osDescription   Domain description.
     * @param eFieldType      Field type.
     * @param eFieldSubType   Field subtype.
     * @param osBlob          Blob expression
     */
    OGRGlobFieldDomain(const std::string &osName,
                       const std::string &osDescription,
                       OGRFieldType eFieldType, OGRFieldSubType eFieldSubType,
                       const std::string &osBlob);

    OGRGlobFieldDomain *Clone() const override;

    /** Get the glob expression.
     *
     * This is the same as the C function OGR_GlobFldDomain_GetGlob().
     */
    const std::string &GetGlob() const
    {
        return m_osGlob;
    }
};

/************************************************************************/
/*                           OGRFeatureQuery                            */
/************************************************************************/

//! @cond Doxygen_Suppress
class OGRLayer;
class swq_expr_node;
class swq_custom_func_registrar;
struct swq_evaluation_context;

class CPL_DLL OGRFeatureQuery
{
  private:
    OGRFeatureDefn *poTargetDefn;
    void *pSWQExpr;
    swq_evaluation_context *m_psContext = nullptr;

    char **FieldCollector(void *, char **);

    static GIntBig *EvaluateAgainstIndices(const swq_expr_node *, OGRLayer *,
                                           GIntBig &nFIDCount);

    static int CanUseIndex(const swq_expr_node *, OGRLayer *);

    OGRErr Compile(OGRLayer *, OGRFeatureDefn *, const char *, int bCheck,
                   swq_custom_func_registrar *poCustomFuncRegistrar);

    CPL_DISALLOW_COPY_ASSIGN(OGRFeatureQuery)

  public:
    OGRFeatureQuery();
    ~OGRFeatureQuery();

    OGRErr Compile(OGRLayer *, const char *, int bCheck = TRUE,
                   swq_custom_func_registrar *poCustomFuncRegistrar = nullptr);
    OGRErr Compile(OGRFeatureDefn *, const char *, int bCheck = TRUE,
                   swq_custom_func_registrar *poCustomFuncRegistrar = nullptr);
    int Evaluate(OGRFeature *);

    GIntBig *EvaluateAgainstIndices(OGRLayer *, OGRErr *);

    int CanUseIndex(OGRLayer *);

    char **GetUsedFields();

    void *GetSWQExpr()
    {
        return pSWQExpr;
    }
};

//! @endcond

#endif /* ndef OGR_FEATURE_H_INCLUDED */
