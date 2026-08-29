target_sources(nei PRIVATE ${PROJECT_SOURCE_DIR}/include/nei/xdr/xdr.h xdr.c)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

