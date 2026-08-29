target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/threading/platform_thread.h
          ${PROJECT_SOURCE_DIR}/include/neixx/threading/thread.h
          ${PROJECT_SOURCE_DIR}/include/neixx/threading/thread_local_storage.h
          ${PROJECT_SOURCE_DIR}/include/neixx/threading/thread_local.h
          ${PROJECT_SOURCE_DIR}/include/neixx/threading/simple_thread.h
          platform_thread.cpp
          platform_thread_win.cpp
          platform_thread_posix.cpp
          thread.cpp
          simple_thread.cpp
          thread_local.cpp
          thread_local_storage.cpp
          internal/tls_slot.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

