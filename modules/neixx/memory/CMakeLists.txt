target_sources(
  nei
  PRIVATE include/neixx/memory/internal_flag.h
          include/neixx/memory/memory_pressure_monitor.h
          include/neixx/memory/pass_key.h
          include/neixx/memory/ref_counted.h
          include/neixx/memory/shared_memory.h
          include/neixx/memory/small_object_allocator.h
          include/neixx/memory/unretained_wrapper.h
          include/neixx/memory/weak_ptr.h
          src/internal_flag.cpp
          src/memory_pressure_monitor.cpp
          src/shared_memory.cpp
          src/small_object_allocator.cpp
)

if(WIN32)
  list(APPEND NEIXX_MEMORY_PLATFORM_SRCS
    src/shared_memory_win.h
    src/shared_memory_win.cpp
  )
else()
  list(APPEND NEIXX_MEMORY_PLATFORM_SRCS
    src/shared_memory_posix.h
    src/shared_memory_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_MEMORY_PLATFORM_SRCS})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
