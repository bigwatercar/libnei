target_sources(
  neixx
  PRIVATE include/neixx/memory/internal_flag.h
          include/neixx/memory/pass_key.h
          include/neixx/memory/ref_counted.h
          include/neixx/memory/unretained_wrapper.h
          include/neixx/memory/weak_ptr.h
          src/internal_flag.cpp
)

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
