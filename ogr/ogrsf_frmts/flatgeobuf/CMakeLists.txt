add_gdal_driver(
  TARGET ogr_FlatGeobuf
  SOURCES ogrflatgeobufdataset.cpp
          ogrflatgeobuflayer.cpp
          packedrtree.cpp
          geometryreader.cpp
          geometrywriter.cpp
          ogrflatgeobufeditablelayer.cpp
  PLUGIN_CAPABLE
  NO_DEPS
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(ogr_FlatGeobuf)
target_include_directories(ogr_FlatGeobuf PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                                  $<TARGET_PROPERTY:ogrsf_generic,SOURCE_DIR>)
# Quick and dirty way of modifying the default flatbuffers namespace to gdal_flatbuffers
target_compile_definitions(ogr_FlatGeobuf PRIVATE -Dflatbuffers=gdal_flatbuffers)
