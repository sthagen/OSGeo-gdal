#!/bin/sh

set -eu

export CXXFLAGS="-std=c++17 -march=native -O2 -Wodr -flto-odr-type-merging -Werror"
export CFLAGS="-O2 -march=native -Werror"

cmake ${GDAL_SOURCE_DIR:=..} \
    -DUSE_CCACHE=ON \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DGDAL_USE_TIFF_INTERNAL=ON \
    -DGDAL_USE_GEOTIFF_INTERNAL=ON \
    -DECW_ROOT=/opt/libecwj2-3.3 \
    -DMRSID_ROOT=/usr/local \
    -DFileGDB_ROOT=/usr/local/FileGDB_API

unset CXXFLAGS
unset CFLAGS

make -j$(nproc)
