# neixx/storage — storage-device event monitoring.
#
# POSIX backend: raw NETLINK_KOCKET_UEVENT netlink socket (kernel uevents),
# no libudev dependency.  The Windows backend is reserved for a future
# revision (RegisterDeviceNotification / DMIOCP); this module is skipped
# entirely on Windows so the build never breaks on a host without a POSIX
# netlink stack.
if(NOT WIN32)
  set(NEIXX_STORAGE_SOURCES
      ${PROJECT_SOURCE_DIR}/include/neixx/storage/storage_device_monitor.h
      storage_device_monitor.cpp
      storage_device_monitor_posix.h
      storage_device_monitor_posix.cpp
  )
  target_sources(nei PRIVATE ${NEIXX_STORAGE_SOURCES})
endif()

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
