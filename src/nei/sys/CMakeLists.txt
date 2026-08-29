target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/nei/sys/process.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/cpu_info.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/memory_info.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/os_info.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/host_info.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/process_info.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/disk_info.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/win/utils.h
          ${PROJECT_SOURCE_DIR}/include/nei/sys/fs_util.h
          cpu_info.c
          memory_info.c
          os_info.c
          host_info.c
          process_info.c
          disk_info.c
)

if(WIN32)
  target_sources(nei PRIVATE win/utils.c fs_util_win.c)
  target_link_libraries(nei PRIVATE ole32)
else()
  target_sources(nei PRIVATE fs_util_posix.c)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

