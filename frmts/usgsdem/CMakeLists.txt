add_gdal_driver(TARGET gdal_USGSDEM SOURCES usgsdemdataset.cpp PLUGIN_CAPABLE NO_DEPS)
gdal_standard_includes(gdal_USGSDEM)
target_include_directories(gdal_USGSDEM PRIVATE $<TARGET_PROPERTY:alg,SOURCE_DIR>)
