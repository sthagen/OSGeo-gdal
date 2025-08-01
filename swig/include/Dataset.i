/******************************************************************************
 *
 * Name:     Dataset.i
 * Project:  GDAL Python Interface
 * Purpose:  GDAL Core SWIG Interface declarations.
 * Author:   Kevin Ruland, kruland@ku.edu
 *
 ******************************************************************************
 * Copyright (c) 2005, Kevin Ruland
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

%{
/* Returned size is in bytes or 0 if an error occurred. */
static
GIntBig ComputeDatasetRasterIOSize (int buf_xsize, int buf_ysize, int nPixelSize,
                                int nBands, int* bandMap, int nBandMapArrayLength,
                                GIntBig nPixelSpace, GIntBig nLineSpace, GIntBig nBandSpace,
                                int bSpacingShouldBeMultipleOfPixelSize )
{
    if (buf_xsize <= 0 || buf_ysize <= 0)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Illegal values for buffer size");
        return 0;
    }

    if (nPixelSpace < 0 || nLineSpace < 0 || nBandSpace < 0)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Illegal values for space arguments");
        return 0;
    }

    if (nPixelSize == 0)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Illegal value for data type");
        return 0;
    }

    if( nPixelSpace == 0 )
        nPixelSpace = nPixelSize;
    else if ( bSpacingShouldBeMultipleOfPixelSize && (nPixelSpace % nPixelSize) != 0 )
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "nPixelSpace should be a multiple of nPixelSize");
        return 0;
    }

    if( nLineSpace == 0 )
    {
        nLineSpace = nPixelSpace * buf_xsize;
    }
    else if ( bSpacingShouldBeMultipleOfPixelSize && (nLineSpace % nPixelSize) != 0 )
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "nLineSpace should be a multiple of nPixelSize");
        return 0;
    }

    if( nBandSpace == 0 )
    {
        nBandSpace = nLineSpace * buf_ysize;
    }
    else if ( bSpacingShouldBeMultipleOfPixelSize && (nBandSpace % nPixelSize) != 0 )
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "nLineSpace should be a multiple of nPixelSize");
        return 0;
    }

    if (nBands <= 0 || (bandMap != NULL && nBands > nBandMapArrayLength))
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Invalid band count");
        return 0;
    }

    GIntBig nRet = (GIntBig)(buf_ysize - 1) * nLineSpace + (GIntBig)(buf_xsize - 1) * nPixelSpace + (GIntBig)(nBands - 1) * nBandSpace + nPixelSize;
#if SIZEOF_VOIDP == 4
    if (nRet > INT_MAX)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Integer overflow");
        return 0;
    }
#endif

    return nRet;
}
%}

#if !defined(SWIGJAVA)

//************************************************************************/
//
// Define the extensions for GDALAsyncReader (nee GDALAsyncReaderShadow)
//
//************************************************************************/
%rename (AsyncReader) GDALAsyncReaderShadow;


%{
typedef struct
{
    GDALAsyncReaderH  hAsyncReader;
    void             *pyObject;
} GDALAsyncReaderWrapper;

typedef void* GDALAsyncReaderWrapperH;

static GDALAsyncReaderH AsyncReaderWrapperGetReader(GDALAsyncReaderWrapperH hWrapper)
{
    GDALAsyncReaderWrapper* psWrapper = (GDALAsyncReaderWrapper*)hWrapper;
    if (psWrapper->hAsyncReader == NULL)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "AsyncReader object is defunct");
    }
    return psWrapper->hAsyncReader;
}

#if defined(SWIGPYTHON)
static void* AsyncReaderWrapperGetPyObject(GDALAsyncReaderWrapperH hWrapper)
{
    GDALAsyncReaderWrapper* psWrapper = (GDALAsyncReaderWrapper*)hWrapper;
    return psWrapper->pyObject;
}
#endif

static void DeleteAsyncReaderWrapper(GDALAsyncReaderWrapperH hWrapper)
{
    GDALAsyncReaderWrapper* psWrapper = (GDALAsyncReaderWrapper*)hWrapper;
    if (psWrapper->hAsyncReader != NULL)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Native AsyncReader object will leak. EndAsyncReader() should have been called before");
    }
    CPLFree(psWrapper);
}

%}

#if defined(SWIGPYTHON)

%nothread;

%{

static GDALAsyncReaderWrapper* CreateAsyncReaderWrapper(GDALAsyncReaderH  hAsyncReader,
                                                        void             *pyObject)
{
    GDALAsyncReaderWrapper* psWrapper = (GDALAsyncReaderWrapper* )CPLMalloc(sizeof(GDALAsyncReaderWrapper));
    psWrapper->hAsyncReader = hAsyncReader;
    psWrapper->pyObject = pyObject;
    Py_INCREF((PyObject*) psWrapper->pyObject);
    return psWrapper;
}

static void DisableAsyncReaderWrapper(GDALAsyncReaderWrapperH hWrapper)
{
    GDALAsyncReaderWrapper* psWrapper = (GDALAsyncReaderWrapper*)hWrapper;
    if (psWrapper->pyObject)
    {
        Py_XDECREF((PyObject*) psWrapper->pyObject);
    }
    psWrapper->pyObject = NULL;
    psWrapper->hAsyncReader = NULL;
}
%}

%thread;

#endif

class GDALAsyncReaderShadow {
private:
  GDALAsyncReaderShadow();
public:
%extend {
    ~GDALAsyncReaderShadow()
    {
        DeleteAsyncReaderWrapper(self);
    }

    %apply (int *OUTPUT) {(int *)};
    GDALAsyncStatusType GetNextUpdatedRegion(double timeout, int* xoff, int* yoff, int* buf_xsize, int* buf_ysize )
    {
        GDALAsyncReaderH hReader = AsyncReaderWrapperGetReader(self);
        if (hReader == NULL)
        {
            *xoff = 0;
            *yoff = 0;
            *buf_xsize = 0;
            *buf_ysize = 0;
            return GARIO_ERROR;
        }
        return GDALARGetNextUpdatedRegion(hReader, timeout, xoff, yoff, buf_xsize, buf_ysize );
    }
    %clear (int *);

#if defined(SWIGPYTHON)
    %apply ( void **outPythonObject ) { (void** ppRetPyObject ) };
    void GetBuffer(void** ppRetPyObject)
    {
        GDALAsyncReaderH hReader = AsyncReaderWrapperGetReader(self);
        if (hReader == NULL)
        {
            *ppRetPyObject = NULL;
            return;
        }
        *ppRetPyObject = AsyncReaderWrapperGetPyObject(self);
        Py_INCREF((PyObject*)*ppRetPyObject);
    }
    %clear (void** ppRetPyObject );
#endif

    int LockBuffer( double timeout )
    {
        GDALAsyncReaderH hReader = AsyncReaderWrapperGetReader(self);
        if (hReader == NULL)
        {
            return 0;
        }
        return GDALARLockBuffer(hReader,timeout);
    }

    void UnlockBuffer()
    {
        GDALAsyncReaderH hReader = AsyncReaderWrapperGetReader(self);
        if (hReader == NULL)
        {
            return;
        }
        GDALARUnlockBuffer(hReader);
    }

    } /* extend */
}; /* GDALAsyncReaderShadow */

#endif // !defined(SWIGJAVA)

//************************************************************************/
//
// Define the extensions for Dataset (nee GDALDatasetShadow)
//
//************************************************************************/

%rename (Dataset) GDALDatasetShadow;

#ifdef SWIGPYTHON
%rename (_SetGCPs) SetGCPs;
%rename (_SetGCPs2) SetGCPs2;
#endif

class GDALDatasetShadow : public GDALMajorObjectShadow {
private:
  GDALDatasetShadow();
public:
%extend {

%immutable;
  int RasterXSize;
  int RasterYSize;
  int RasterCount;
//
// Needed
// _band list?
%mutable;

  ~GDALDatasetShadow() {
    if ( GDALDereferenceDataset( self ) <= 0 ) {
      if( GDALClose(self) != CE_None )
      {
          if( CPLGetLastErrorType() == CE_None )
              CPLError(CE_Failure, CPLE_AppDefined, "Error occurred in GDALClose()");
      }
    }
  }

  void MarkSuppressOnClose() {
    GDALDatasetMarkSuppressOnClose(self);
  }

#ifdef SWIGJAVA
  %rename (CloseInternal) Close;
  %javamethodmodifiers Close() "private";
#endif
  CPLErr Close() {
     return GDALClose(self);
  }

  GDALDriverShadow* GetDriver() {
    return (GDALDriverShadow*) GDALGetDatasetDriver( self );
  }

  GDALRasterBandShadow* GetRasterBand(int nBand ) {
    return (GDALRasterBandShadow*) GDALGetRasterBand( self, nBand );
  }

  bool IsThreadSafe(int nScopeFlags)
  {
      return GDALDatasetIsThreadSafe(self, nScopeFlags, nullptr);
  }

%newobject GetThreadSafeDataset;
  GDALDatasetShadow* GetThreadSafeDataset(int nScopeFlags)
  {
      return GDALGetThreadSafeDataset(self, nScopeFlags, nullptr);
  }

%newobject GetRootGroup;
  GDALGroupHS* GetRootGroup() {
    return GDALDatasetGetRootGroup(self);
  }

  char const *GetProjection() {
    return GDALGetProjectionRef( self );
  }

  char const *GetProjectionRef() {
    return GDALGetProjectionRef( self );
  }

#ifdef SWIGPYTHON
  int GetRefCount() {
    return OGR_DS_GetRefCount(self);
  }

  int GetSummaryRefCount() {
    return OGR_DS_GetSummaryRefCount(self);
  }
#endif

  %newobject GetSpatialRef;
  OSRSpatialReferenceShadow *GetSpatialRef() {
    OGRSpatialReferenceH ref = GDALGetSpatialRef(self);
    if( ref )
       ref = OSRClone( ref );
    return (OSRSpatialReferenceShadow*) ref;
  }

  %apply Pointer NONNULL {char const *prj};
  CPLErr SetProjection( char const *prj ) {
    return GDALSetProjection( self, prj );
  }
  %clear char const *prj;

  CPLErr SetSpatialRef(OSRSpatialReferenceShadow* srs)
  {
     return GDALSetSpatialRef( self, (OGRSpatialReferenceH)srs );
  }

#ifdef SWIGPYTHON
%feature("kwargs") GetGeoTransform;
%apply (int *optional_int) { (int*) };
  void GetGeoTransform( double argout[6], int* isvalid, int* can_return_null = 0 ) {
    if (can_return_null && *can_return_null)
    {
        *isvalid = (GDALGetGeoTransform( self, argout ) == CE_None );
    }
    else
    {
        *isvalid = TRUE;
        if ( GDALGetGeoTransform( self, argout ) != CE_None ) {
            argout[0] = 0.0;
            argout[1] = 1.0;
            argout[2] = 0.0;
            argout[3] = 0.0;
            argout[4] = 0.0;
            argout[5] = 1.0;
        }
    }
  }
%clear (int*);
#else
  void GetGeoTransform( double argout[6] ) {
    if ( GDALGetGeoTransform( self, argout ) != CE_None ) {
      argout[0] = 0.0;
      argout[1] = 1.0;
      argout[2] = 0.0;
      argout[3] = 0.0;
      argout[4] = 0.0;
      argout[5] = 1.0;
    }
  }
#endif

  CPLErr SetGeoTransform( double argin[6] ) {
    return GDALSetGeoTransform( self, argin );
  }


#if defined(SWIGCSHARP)
  %feature( "kwargs" ) GetExtent;
  CPLErr GetExtent(OGREnvelope* extent, OSRSpatialReferenceShadow* srs = NULL) {
    return GDALGetExtent(self, extent, srs);
  }
#elif defined(SWIGPYTHON)
  %feature( "kwargs" ) GetExtent;
  void GetExtent(double argout[4], int* isvalid, OSRSpatialReferenceShadow* srs = NULL) {
    CPLErr eErr = GDALGetExtent(self, (OGREnvelope*)argout, srs);
    *isvalid = (eErr == CE_None);
    return;
  }
#else
  CPLErr GetExtent(double argout[4], OSRSpatialReferenceShadow* srs = NULL) {
    return GDALGetExtent(self, (OGREnvelope*)argout, srs);
  }
#endif


#if defined(SWIGCSHARP)
  %feature( "kwargs" ) GetExtentWGS84LongLat;
  CPLErr GetExtentWGS84LongLat(OGREnvelope* extent) {
    return GDALGetExtentWGS84LongLat(self, extent);
  }
#elif defined(SWIGPYTHON)
  %feature( "kwargs" ) GetExtentWGS84LongLat;
  void GetExtentWGS84LongLat(double argout[4], int* isvalid) {
    CPLErr eErr = GDALGetExtentWGS84LongLat(self, (OGREnvelope*)argout);
    *isvalid = (eErr == CE_None);
    return;
  }
#else
  CPLErr GetExtentWGS84LongLat(double argout[4]) {
    return GDALGetExtentWGS84LongLat(self, (OGREnvelope*)argout);
  }
#endif


  // The (int,int*) arguments are typemapped.  The name of the first argument
  // becomes the kwarg name for it.
#ifndef SWIGCSHARP
#ifndef SWIGJAVA
%feature("kwargs") BuildOverviews;
#endif
%apply (int nList, int* pList) { (int overviewlist, int *pOverviews) };
#else
%apply (void *buffer_ptr) {int *pOverviews};
#endif
#ifdef SWIGJAVA
%apply (const char* stringWithDefaultValue) {const char* resampling};
  int BuildOverviews( const char *resampling,
                      int overviewlist, int *pOverviews,
                      GDALProgressFunc callback = NULL,
                      void* callback_data=NULL,
                      char** options = NULL ) {
#else
  int BuildOverviews( const char *resampling = "NEAREST",
                      int overviewlist = 0 , int *pOverviews = 0,
                      GDALProgressFunc callback = NULL,
                      void* callback_data=NULL,
                      char** options = NULL ) {
#endif
    return GDALBuildOverviewsEx(  self,
                                resampling ? resampling : "NEAREST",
                                overviewlist,
                                pOverviews,
                                0,
                                0,
                                callback,
                                callback_data,
                                options);
  }
#ifndef SWIGCSHARP
%clear (int overviewlist, int *pOverviews);
#else
%clear (int *pOverviews);
#endif
#ifdef SWIGJAVA
%clear (const char *resampling);
#endif

  int GetGCPCount() {
    return GDALGetGCPCount( self );
  }

  const char *GetGCPProjection() {
    return GDALGetGCPProjection( self );
  }

#ifndef SWIGCSHARP
  %newobject GetGCPSpatialRef;
  OSRSpatialReferenceShadow *GetGCPSpatialRef() {
    OGRSpatialReferenceH ref = GDALGetGCPSpatialRef(self);
    if( ref )
       ref = OSRClone( ref );
    return (OSRSpatialReferenceShadow*) ref;
  }
#endif

#ifndef SWIGCSHARP
  void GetGCPs( int *nGCPs, GDAL_GCP const **pGCPs ) {
    *nGCPs = GDALGetGCPCount( self );
    *pGCPs = GDALGetGCPs( self );
  }

  CPLErr SetGCPs( int nGCPs, GDAL_GCP const *pGCPs, const char *pszGCPProjection ) {
    return GDALSetGCPs( self, nGCPs, pGCPs, pszGCPProjection );
  }

  CPLErr SetGCPs2( int nGCPs, GDAL_GCP const *pGCPs, OSRSpatialReferenceShadow* hSRS ) {
    return GDALSetGCPs2( self, nGCPs, pGCPs, (OGRSpatialReferenceH)hSRS );
  }

#endif

  CPLErr FlushCache() {
    return GDALFlushCache( self );
  }

#ifndef SWIGJAVA
%feature ("kwargs") AddBand;
#endif
/* uses the defined char **options typemap */
  CPLErr AddBand( GDALDataType datatype = GDT_Byte, char **options = 0 ) {
    return GDALAddBand( self, datatype, options );
  }

  CPLErr CreateMaskBand( int nFlags ) {
      return GDALCreateDatasetMaskBand( self, nFlags );
  }

%apply (char **CSL) {char **};
  char **GetFileList() {
    return GDALGetFileList( self );
  }
%clear char **;

#if defined(SWIGPYTHON)
%feature("kwargs") WriteRaster;
%apply (GIntBig nLen, char *pBuf) { (GIntBig buf_len, char *buf_string) };
%apply (GIntBig *optional_GIntBig) { (GIntBig*) };
%apply (int *optional_int) { (int*) };
#if defined(SWIGPYTHON)
%apply (GDALDataType *optional_GDALDataType) { (GDALDataType *buf_type) };
#else
%apply (int *optional_int) { (GDALDataType *buf_type) };
#endif
%apply (int nList, int *pList ) { (int band_list, int *pband_list ) };
  CPLErr WriteRaster( int xoff, int yoff, int xsize, int ysize,
                      GIntBig buf_len, char *buf_string,
                      int *buf_xsize = 0, int *buf_ysize = 0,
                      GDALDataType *buf_type = 0,
                      int band_list = 0, int *pband_list = 0,
                      GIntBig* buf_pixel_space = 0, GIntBig* buf_line_space = 0, GIntBig* buf_band_space = 0) {
    CPLErr eErr;
    int nxsize = (buf_xsize==0) ? xsize : *buf_xsize;
    int nysize = (buf_ysize==0) ? ysize : *buf_ysize;
    GDALDataType ntype;
    if ( buf_type != 0 ) {
      ntype = (GDALDataType) *buf_type;
    } else {
      int lastband = GDALGetRasterCount( self );
      if (lastband <= 0)
        return CE_Failure;
      ntype = GDALGetRasterDataType( GDALGetRasterBand( self, lastband ) );
    }

    GIntBig pixel_space = (buf_pixel_space == 0) ? 0 : *buf_pixel_space;
    GIntBig line_space = (buf_line_space == 0) ? 0 : *buf_line_space;
    GIntBig band_space = (buf_band_space == 0) ? 0 : *buf_band_space;

    GIntBig min_buffer_size =
      ComputeDatasetRasterIOSize (nxsize, nysize, GDALGetDataTypeSizeBytes( ntype ),
                                  band_list ? band_list : GDALGetRasterCount(self), pband_list, band_list,
                                  pixel_space, line_space, band_space, FALSE);
    if (min_buffer_size == 0)
        return CE_Failure;

    if ( buf_len < min_buffer_size )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Buffer too small");
        return CE_Failure;
    }

    GDALRasterIOExtraArg* psExtraArg = NULL;

    eErr = GDALDatasetRasterIOEx( self, GF_Write, xoff, yoff, xsize, ysize,
                                  (void*) buf_string, nxsize, nysize, ntype,
                                  band_list, pband_list, pixel_space, line_space, band_space, psExtraArg );

    return eErr;
  }
%clear (int band_list, int *pband_list );
%clear (GDALDataType *buf_type);
%clear (int*);
%clear (GIntBig*);
%clear (GIntBig buf_len, char *buf_string);
#endif

%apply (int *optional_int) { (GDALDataType *buf_type) };
#if defined(SWIGCSHARP)
%apply int PINNED[] {int *pband_list};
#else
%apply (int nList, int *pList ) { (int band_list, int *pband_list ) };
#endif
CPLErr AdviseRead(  int xoff, int yoff, int xsize, int ysize,
                    int *buf_xsize = 0, int *buf_ysize = 0,
                    GDALDataType *buf_type = 0,
                    int band_list = 0, int *pband_list = 0,
                    char** options = NULL )
{
    int nxsize = (buf_xsize==0) ? xsize : *buf_xsize;
    int nysize = (buf_ysize==0) ? ysize : *buf_ysize;
    GDALDataType ntype;
    if ( buf_type != 0 ) {
      ntype = (GDALDataType) *buf_type;
    } else {
      int lastband = GDALGetRasterCount( self );
      if (lastband <= 0)
        return CE_Failure;
      ntype = GDALGetRasterDataType( GDALGetRasterBand( self, lastband ) );
    }
    return GDALDatasetAdviseRead(self, xoff, yoff, xsize, ysize,
                                 nxsize, nysize, ntype,
                                 band_list, pband_list, options);
}
%clear (GDALDataType *buf_type);
#if defined(SWIGCSHARP)
%clear int *pband_list;
#else
%clear (int band_list, int *pband_list );
#endif

/* NEEDED */
/* GetSubDatasets */
/* ReadAsArray */
/* AddBand */

#if defined(SWIGPYTHON)
%feature("kwargs") BeginAsyncReader;
%newobject BeginAsyncReader;
%apply (int nList, int *pList ) { (int band_list, int *pband_list ) };
%apply (size_t nLenKeepObject, char *pBufKeepObject, void* pyObject) { (size_t buf_len, char *buf_string, void* pyObject) };
%apply (int *optional_int) { (int*) };
  GDALAsyncReaderShadow* BeginAsyncReader(
       int xOff, int yOff, int xSize, int ySize,
       size_t buf_len, char *buf_string, void* pyObject,
       int buf_xsize, int buf_ysize, GDALDataType bufType = (GDALDataType)0,
       int band_list = 0, int *pband_list = 0, int nPixelSpace = 0,
       int nLineSpace = 0, int nBandSpace = 0, char **options = 0)  {

    if ((options != NULL) && (buf_xsize ==0) && (buf_ysize == 0))
    {
        // calculate an appropriate buffer size
        const char* pszLevel = CSLFetchNameValue(options, "LEVEL");
        if (pszLevel)
        {
            // round up
            int nLevel = atoi(pszLevel);
            if( nLevel < 0 || nLevel > 30 )
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid LEVEL: %d", nLevel);
            }
            else
            {
                int nRes = 1 << nLevel;
                buf_xsize = static_cast<int>(ceil(xSize / (1.0 * nRes)));
                buf_ysize = static_cast<int>(ceil(ySize / (1.0 * nRes)));
            }
        }
    }

    int nxsize = (buf_xsize == 0) ? xSize : buf_xsize;
    int nysize = (buf_ysize == 0) ? ySize : buf_ysize;

    GDALDataType ntype;
    if (bufType != 0) {
        ntype = (GDALDataType) bufType;
    }
    else {
        ntype = GDT_Byte;
    }

    int nBCount = (band_list) != 0 ? band_list : GDALGetRasterCount(self);
    uint64_t nMinSize = (uint64_t)nxsize * nysize * nBCount * GDALGetDataTypeSizeBytes(ntype);
    if (buf_string == NULL || buf_len < nMinSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Buffer is too small");
        return NULL;
    }

    bool myBandList = false;
    int* pBandList;

    if (band_list != 0){
        myBandList = false;
        pBandList = pband_list;
    }
    else
    {
        myBandList = true;
        pBandList = (int*)CPLMalloc(sizeof(int) * nBCount);
        for (int i = 0; i < nBCount; ++i) {
            pBandList[i] = i + 1;
        }
    }

    GDALAsyncReaderH hAsyncReader =
            GDALBeginAsyncReader(self, xOff, yOff, xSize, ySize, (void*) buf_string, nxsize, nysize, ntype, nBCount, pBandList, nPixelSpace, nLineSpace,
    nBandSpace, options);

    if ( myBandList ) {
       CPLFree( pBandList );
    }

    if (hAsyncReader)
    {
        return (GDALAsyncReaderShadow*) CreateAsyncReaderWrapper(hAsyncReader, pyObject);
    }
    else
    {
        return NULL;
    }

  }

%clear(int band_list, int *pband_list);
%clear (size_t buf_len, char *buf_string, void* pyObject);
%clear(int*);

  void EndAsyncReader(GDALAsyncReaderShadow* ario){
    if( ario == NULL ) return;
    GDALAsyncReaderH hReader = AsyncReaderWrapperGetReader(ario);
    if (hReader == NULL)
    {
        return;
    }
    GDALEndAsyncReader(self, hReader);
    DisableAsyncReaderWrapper(ario);
  }

%feature( "kwargs" ) GetVirtualMem;
%newobject GetVirtualMem;
%apply (int nList, int *pList ) { (int band_list, int *pband_list ) };
  CPLVirtualMemShadow* GetVirtualMem( GDALRWFlag eRWFlag,
                                      int nXOff, int nYOff,
                                      int nXSize, int nYSize,
                                      int nBufXSize, int nBufYSize,
                                      GDALDataType eBufType,
                                      int band_list, int *pband_list,
                                      int bIsBandSequential,
                                      size_t nCacheSize,
                                      size_t nPageSizeHint,
                                      char** options = NULL )
    {
        int nPixelSpace;
        int nBandSpace;
        if( bIsBandSequential != 0 && bIsBandSequential != 1 )
            return NULL;
        if( band_list == 0 )
            return NULL;
        if( bIsBandSequential || band_list == 1 )
        {
            nPixelSpace = 0;
            nBandSpace = 0;
        }
        else
        {
            nBandSpace = GDALGetDataTypeSizeBytes(eBufType);
            nPixelSpace = nBandSpace * band_list;
        }
        CPLVirtualMem* vmem = GDALDatasetGetVirtualMem( self,
                                         eRWFlag,
                                         nXOff, nYOff,
                                         nXSize, nYSize,
                                         nBufXSize, nBufYSize,
                                         eBufType,
                                         band_list, pband_list,
                                         nPixelSpace,
                                         0,
                                         nBandSpace,
                                         nCacheSize,
                                         nPageSizeHint,
                                         FALSE,
                                         options );
        if( vmem == NULL )
            return NULL;
        CPLVirtualMemShadow* vmemshadow = (CPLVirtualMemShadow*)calloc(1, sizeof(CPLVirtualMemShadow));
        vmemshadow->vmem = vmem;
        vmemshadow->eBufType = eBufType;
        vmemshadow->bIsBandSequential = bIsBandSequential;
        vmemshadow->bReadOnly = (eRWFlag == GF_Read);
        vmemshadow->nBufXSize = nBufXSize;
        vmemshadow->nBufYSize = nBufYSize;
        vmemshadow->nBandCount = band_list;
        return vmemshadow;
    }
%clear(int band_list, int *pband_list);

%feature( "kwargs" ) GetTiledVirtualMem;
%newobject GetTiledVirtualMem;
%apply (int nList, int *pList ) { (int band_list, int *pband_list ) };
  CPLVirtualMemShadow* GetTiledVirtualMem( GDALRWFlag eRWFlag,
                                      int nXOff, int nYOff,
                                      int nXSize, int nYSize,
                                      int nTileXSize, int nTileYSize,
                                      GDALDataType eBufType,
                                      int band_list, int *pband_list,
                                      GDALTileOrganization eTileOrganization,
                                      size_t nCacheSize,
                                      char** options = NULL )
    {
        if( band_list == 0 )
            return NULL;
        CPLVirtualMem* vmem = GDALDatasetGetTiledVirtualMem( self,
                                         eRWFlag,
                                         nXOff, nYOff,
                                         nXSize, nYSize,
                                         nTileXSize, nTileYSize,
                                         eBufType,
                                         band_list, pband_list,
                                         eTileOrganization,
                                         nCacheSize,
                                         FALSE,
                                         options );
        if( vmem == NULL )
            return NULL;
        CPLVirtualMemShadow* vmemshadow = (CPLVirtualMemShadow*)calloc(1, sizeof(CPLVirtualMemShadow));
        vmemshadow->vmem = vmem;
        vmemshadow->eBufType = eBufType;
        vmemshadow->bIsBandSequential = -1;
        vmemshadow->bReadOnly = (eRWFlag == GF_Read);
        vmemshadow->nBufXSize = nXSize;
        vmemshadow->nBufYSize = nYSize;
        vmemshadow->eTileOrganization = eTileOrganization;
        vmemshadow->nTileXSize = nTileXSize;
        vmemshadow->nTileYSize = nTileYSize;
        vmemshadow->nBandCount = band_list;
        return vmemshadow;
    }
%clear(int band_list, int *pband_list);

#endif /* PYTHON */

#if defined(SWIGPYTHON) || defined(SWIGJAVA)

  /* Note that datasources own their layers */
#ifndef SWIGJAVA
  %feature( "kwargs" ) CreateLayer;
#endif
  OGRLayerShadow *CreateLayer(const char* name,
              OSRSpatialReferenceShadow* srs=NULL,
              OGRwkbGeometryType geom_type=wkbUnknown,
              char** options=0) {
    OGRLayerShadow* layer = (OGRLayerShadow*) GDALDatasetCreateLayer( self,
                                  name,
                                  srs,
                                  geom_type,
                                  options);
    return layer;
  }

  /* Note that datasources own their layers */
#ifndef SWIGJAVA
  %feature( "kwargs" ) CreateLayer;
#endif
  OGRLayerShadow *CreateLayerFromGeomFieldDefn(const char* name,
              OGRGeomFieldDefnShadow* geom_field,
              char** options=0) {
    OGRLayerShadow* layer = (OGRLayerShadow*) GDALDatasetCreateLayerFromGeomFieldDefn( self,
                                  name,
                                  geom_field,
                                  options);
    return layer;
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) CopyLayer;
#endif
%apply Pointer NONNULL {OGRLayerShadow *src_layer};
  OGRLayerShadow *CopyLayer(OGRLayerShadow *src_layer,
            const char* new_name,
            char** options=0) {
    OGRLayerShadow* layer = (OGRLayerShadow*) GDALDatasetCopyLayer( self,
                                                      src_layer,
                                                      new_name,
                                                      options);
    return layer;
  }

  OGRErr DeleteLayer(int index){
    return GDALDatasetDeleteLayer(self, index);
  }

  bool IsLayerPrivate( int index ) {
    return GDALDatasetIsLayerPrivate(self, index);
  }




#ifdef SWIGPYTHON
%newobject GetNextFeature;
%feature( "kwargs" ) GetNextFeature;
  OGRFeatureShadow* GetNextFeature( bool include_layer = true,
                                    bool include_pct = false,
                                    OGRLayerShadow** ppoBelongingLayer = NULL,
                                    double* pdfProgressPct = NULL,
                                    GDALProgressFunc callback = NULL,
                                    void* callback_data=NULL )
  {
    OGRLayerH hLayer = NULL;
    OGRFeatureShadow* feat = (OGRFeatureShadow*)GDALDatasetGetNextFeature( self, &hLayer, pdfProgressPct,
                                      callback, callback_data );
    *ppoBelongingLayer = (OGRLayerShadow*)hLayer;
    return feat;
  }
#else
    // FIXME: return layer
%newobject GetNextFeature;
  OGRFeatureShadow* GetNextFeature()
  {
    return (OGRFeatureShadow*)GDALDatasetGetNextFeature( self, NULL, NULL, NULL, NULL );
  }
#endif

  bool TestCapability(const char * cap) {
    return (GDALDatasetTestCapability(self, cap) > 0);
  }

#ifndef SWIGJAVA
  %feature( "kwargs" ) ExecuteSQL;
#endif
  %apply Pointer NONNULL {const char * statement};
  OGRLayerShadow *ExecuteSQL(const char* statement,
                        OGRGeometryShadow* spatialFilter=NULL,
                        const char* dialect="") {
    OGRLayerShadow* layer = (OGRLayerShadow*) GDALDatasetExecuteSQL(self,
                                                      statement,
                                                      spatialFilter,
                                                      dialect);
    return layer;
  }

%apply SWIGTYPE *DISOWN {OGRLayerShadow *layer};
  void ReleaseResultSet(OGRLayerShadow *layer){
    GDALDatasetReleaseResultSet(self, layer);
  }
%clear OGRLayerShadow *layer;

  OGRStyleTableShadow *GetStyleTable() {
    return (OGRStyleTableShadow*) GDALDatasetGetStyleTable(self);
  }

  void SetStyleTable(OGRStyleTableShadow* table) {
    if( table != NULL )
        GDALDatasetSetStyleTable(self, (OGRStyleTableH) table);
  }

#endif /* defined(SWIGPYTHON) || defined(SWIGJAVA) */


#ifdef SWIGJAVA
  OGRLayerShadow *GetLayerByIndex( int index ) {
#elif SWIGPYTHON
  OGRLayerShadow *GetLayerByIndex( int index=0) {
#else
  OGRLayerShadow *GetLayer( int index ) {
#endif
    OGRLayerShadow* layer = (OGRLayerShadow*) GDALDatasetGetLayer(self, index);
    return layer;
  }

  OGRLayerShadow *GetLayerByName(const char* layer_name) {
    OGRLayerShadow* layer = (OGRLayerShadow*) GDALDatasetGetLayerByName(self, layer_name);
    return layer;
  }

  void ResetReading()
  {
    GDALDatasetResetReading(self);
  }

  int GetLayerCount() {
    return GDALDatasetGetLayerCount(self);
  }

#ifdef SWIGCSHARP

  %newobject GetNextFeature;
  OGRFeatureShadow *GetNextFeature( OGRLayerShadow** ppoBelongingLayer = NULL,
                                    double* pdfProgressPct = NULL,
                                    GDALProgressFunc callback = NULL,
                                    void* callback_data=NULL )
  {
    OGRLayerH hLayer = NULL;
    OGRFeatureShadow* feat = (OGRFeatureShadow*)GDALDatasetGetNextFeature( self, &hLayer, pdfProgressPct,
                                      callback, callback_data );
    *ppoBelongingLayer = (OGRLayerShadow*)hLayer;
    return feat;
  }


#endif

OGRErr AbortSQL() {
    return GDALDatasetAbortSQL(self);
}

#ifndef SWIGJAVA
  %feature( "kwargs" ) StartTransaction;
#endif
  OGRErr StartTransaction(int force = FALSE)
  {
    return GDALDatasetStartTransaction(self, force);
  }

  OGRErr CommitTransaction()
  {
    return GDALDatasetCommitTransaction(self);
  }

  OGRErr RollbackTransaction()
  {
    return GDALDatasetRollbackTransaction(self);
  }

  void ClearStatistics()
  {
      GDALDatasetClearStatistics(self);
  }

%apply (char **CSL) {char **};
  char **GetFieldDomainNames(char** options = 0)
  {
    return GDALDatasetGetFieldDomainNames(self, options);
  }
%clear char **;

  %apply Pointer NONNULL {const char* name};
  OGRFieldDomainShadow* GetFieldDomain(const char* name)
  {
    return (OGRFieldDomainShadow*) GDALDatasetGetFieldDomain(self, name);
  }
  %clear const char* name;

  %apply Pointer NONNULL {OGRFieldDomainShadow* fieldDomain};
  bool AddFieldDomain(OGRFieldDomainShadow* fieldDomain)
  {
      char* pszReason = NULL;
      if( !GDALDatasetAddFieldDomain(self, (OGRFieldDomainH)fieldDomain, &pszReason) )
      {
          CPLError(CE_Failure, CPLE_AppDefined, "%s", pszReason);
          CPLFree(pszReason);
          return false;
      }
      return true;
  }
  %clear OGRFieldDomainShadow* fieldDomain;

  %apply Pointer NONNULL {const char* name};
  bool DeleteFieldDomain(const char* name)
  {
      return GDALDatasetDeleteFieldDomain(self, name, NULL);
  }
  %clear const char* name;

  %apply Pointer NONNULL {OGRFieldDomainShadow* fieldDomain};
  bool UpdateFieldDomain(OGRFieldDomainShadow* fieldDomain)
  {
      return GDALDatasetUpdateFieldDomain(self, (OGRFieldDomainH)fieldDomain, NULL);
  }
  %clear OGRFieldDomainShadow* fieldDomain;

%apply (char **CSL) {char **};
  char **GetRelationshipNames(char** options = 0)
  {
    return GDALDatasetGetRelationshipNames(self, options);
  }
%clear char **;

  %apply Pointer NONNULL {const char* name};
  GDALRelationshipShadow* GetRelationship(const char* name)
  {
    return (GDALRelationshipShadow*) GDALDatasetGetRelationship(self, name);
  }
  %clear const char* name;

  %apply Pointer NONNULL {GDALRelationshipShadow* relationship};
  bool AddRelationship(GDALRelationshipShadow* relationship)
  {
      return GDALDatasetAddRelationship(self, (GDALRelationshipH)relationship, NULL);
  }
  %clear GDALRelationshipShadow* relationship;

  %apply Pointer NONNULL {const char* name};
  bool DeleteRelationship(const char* name)
  {
      return GDALDatasetDeleteRelationship(self, name, NULL);
  }
  %clear const char* name;

  %apply Pointer NONNULL {GDALRelationshipShadow* relationship};
  bool UpdateRelationship(GDALRelationshipShadow* relationship)
  {
      return GDALDatasetUpdateRelationship(self, (GDALRelationshipH)relationship, NULL);
  }
  %clear GDALRelationshipShadow* relationship;

} /* extend */
}; /* GDALDatasetShadow */

%{
int GDALDatasetShadow_RasterXSize_get( GDALDatasetShadow *h ) {
  return GDALGetRasterXSize( h );
}
int GDALDatasetShadow_RasterYSize_get( GDALDatasetShadow *h ) {
  return GDALGetRasterYSize( h );
}
int GDALDatasetShadow_RasterCount_get( GDALDatasetShadow *h ) {
  return GDALGetRasterCount( h );
}
%}

