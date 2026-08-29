target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/memory/internal_flag.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/memory_pressure_monitor.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/pass_key.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/ref_counted.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/shared_memory.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/small_object_allocator.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/unretained_wrapper.h
          ${PROJECT_SOURCE_DIR}/include/neixx/memory/weak_ptr.h
          internal_flag.cpp
          memory_pressure_monitor.cpp
          shared_memory.cpp
          small_object_allocator.cpp
)

if(WIN32)
  list(APPEND NEIXX_MEMORY_PLATFORM_SRCS shared_memory_win.h
       shared_memory_win.cpp
  )
else()
  list(APPEND NEIXX_MEMORY_PLATFORM_SRCS shared_memory_posix.h
       shared_memory_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_MEMORY_PLATFORM_SRCS})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

