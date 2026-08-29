set(NEIXX_FILES_SOURCES ${PROJECT_SOURCE_DIR}/include/neixx/files/file_path_watcher.h
                        file_path_watcher.cpp
)

if(WIN32)
  list(APPEND NEIXX_FILES_SOURCES file_path_watcher_win.h
       file_path_watcher_win.cpp
  )
else()
  list(APPEND NEIXX_FILES_SOURCES file_path_watcher_posix.h
       file_path_watcher_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_FILES_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "internal" EXCLUDE
)
