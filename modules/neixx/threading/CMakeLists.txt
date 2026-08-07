target_sources(
  nei
  PRIVATE include/neixx/threading/platform_thread.h
          include/neixx/threading/thread.h
          include/neixx/threading/thread_local_storage.h
          include/neixx/threading/thread_local.h
          include/neixx/threading/simple_thread.h
          src/platform_thread.cpp
          src/platform_thread_win.cpp
          src/platform_thread_posix.cpp
          src/thread.cpp
          src/simple_thread.cpp
          src/thread_local.cpp
          src/thread_local_storage.cpp
          src/internal/tls_slot.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
