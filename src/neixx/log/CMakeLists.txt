target_sources(nei PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/log/log.h)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
