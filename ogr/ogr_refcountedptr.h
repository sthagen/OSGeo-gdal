/******************************************************************************
 *
 * Project:  OGR
 * Purpose:  Smart pointer around a class that has built-in reference counting.
 * Author:   Even Rouault <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2026, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_REFCOUNTEDPTR_INCLUDED
#define OGR_REFCOUNTEDPTR_INCLUDED

/*! @cond Doxygen_Suppress */

template <class T> struct OGRRefCountedPtrBase
{
  public:
    inline ~OGRRefCountedPtrBase()
    {
        reset(nullptr);
    }

    inline void reset(T *poRawPtr)
    {
        if (m_poRawPtr)
            m_poRawPtr->Release();
        m_poRawPtr = poRawPtr;
        if (m_poRawPtr)
            m_poRawPtr->Reference();
    }

    inline T *get() const
    {
        return m_poRawPtr;
    }

    inline T &operator*() const
    {
        return *m_poRawPtr;
    }

    inline T *operator->() const
    {
        return m_poRawPtr;
    }

    inline operator bool() const
    {
        return m_poRawPtr != nullptr;
    }

  protected:
    inline explicit OGRRefCountedPtrBase(T *poRawPtr = nullptr)
        : m_poRawPtr(poRawPtr)
    {
        if (m_poRawPtr)
            m_poRawPtr->Reference();
    }

  private:
    T *m_poRawPtr{};

    OGRRefCountedPtrBase(const OGRRefCountedPtrBase &) = delete;
    OGRRefCountedPtrBase &operator=(const OGRRefCountedPtrBase &) = delete;
    OGRRefCountedPtrBase(OGRRefCountedPtrBase &&) = delete;
    OGRRefCountedPtrBase &operator=(OGRRefCountedPtrBase &&) = delete;
};

/** Smart pointer around a class that has built-in reference counting.
 *
 * It uses the Reference() and Release() methods of the wrapped class for
 * reference counting. The reference count is increased when assigning a raw
 * pointer to the smart pointer, and decreased when releasing it.
 * Somewhat similar to https://www.boost.org/doc/libs/latest/libs/smart_ptr/doc/html/smart_ptr.html#intrusive_ptr
 *
 * Only meant for T = OGRFeatureDefn and OGRSpatialReference
 */
template <class T> struct OGRRefCountedPtr : public OGRRefCountedPtrBase<T>
{
};

template <class T>
inline bool operator==(const OGRRefCountedPtr<T> &lhs, std::nullptr_t)
{
    return lhs.get() == nullptr;
}

template <class T>
inline bool operator!=(const OGRRefCountedPtr<T> &lhs, std::nullptr_t)
{
    return lhs.get() != nullptr;
}

/*! @endcond */

#endif /* OGR_REFCOUNTEDPTR_INCLUDED */
