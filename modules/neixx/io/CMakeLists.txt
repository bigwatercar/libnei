set(NEIXX_IO_SOURCES
  include/neixx/io/async_file.h
  include/neixx/io/async_stream.h
  include/neixx/io/async_line_reader.h
  include/neixx/io/async_file_win.h
  include/neixx/io/pipe_stream_factory.h
  src/async_line_reader.cpp
)

if(WIN32)
  list(APPEND NEIXX_IO_SOURCES
    src/pipe_stream_win.cpp
    src/async_file_win.cpp
  )
else()
  list(APPEND NEIXX_IO_SOURCES src/pipe_stream_posix.cpp)
endif()

target_sources(neixx PRIVATE ${NEIXX_IO_SOURCES})

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(WIN32)
  target_link_libraries(neixx PRIVATE ws2_32)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
