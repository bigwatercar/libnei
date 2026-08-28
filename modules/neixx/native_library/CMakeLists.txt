target_sources(
  nei
  PRIVATE include/neixx/native_library/native_library.h
          src/native_library.cpp
)

if(WIN32)
  target_sources(nei PRIVATE src/native_library_win.cpp)
else()
  target_sources(nei PRIVATE src/native_library_posix.cpp)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
