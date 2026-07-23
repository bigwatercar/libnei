set(NEIXX_FILES_SOURCES
  include/neixx/files/file_path_watcher.h
  src/file_path_watcher.cpp
)

if(WIN32)
  list(APPEND NEIXX_FILES_SOURCES
    src/file_path_watcher_win.h
    src/file_path_watcher_win.cpp
  )
else()
  list(APPEND NEIXX_FILES_SOURCES
    src/file_path_watcher_posix.h
    src/file_path_watcher_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_FILES_SOURCES})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "internal" EXCLUDE
)
