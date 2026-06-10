target_sources(
  neixx
  PRIVATE include/neixx/common/location.h
          include/neixx/common/time.h
          include/neixx/common/time_source.h
          src/location.cpp
          src/time.cpp
          src/time_source.cpp
)

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
