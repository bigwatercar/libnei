target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/native_library/native_library.h
          native_library.cpp
)

if(WIN32)
  target_sources(nei PRIVATE native_library_win.cpp)
else()
  target_sources(nei PRIVATE native_library_posix.cpp)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

