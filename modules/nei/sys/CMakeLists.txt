target_sources(
  nei
  PRIVATE include/nei/sys/process.h
          include/nei/sys/cpu_info.h
          include/nei/sys/memory_info.h
          include/nei/sys/os_info.h
          include/nei/sys/host_info.h
          include/nei/sys/process_info.h
          include/nei/sys/disk_info.h

          src/cpu_info.c
          src/memory_info.c
          src/os_info.c
          src/host_info.c
          src/process_info.c
          src/disk_info.c
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
