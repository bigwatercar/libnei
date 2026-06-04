set(NEIXX_IO_SOURCES
  include/neixx/io/io_buffer.h
  include/neixx/io/async_file.h
  include/neixx/io/async_stream.h
  include/neixx/io/async_line_reader.h
  include/neixx/io/stream_reader.h
  include/neixx/io/stream_writer.h
  include/neixx/io/file_stream_adapters.h
  src/internal/pipe_stream_factory_internal.h
  src/internal/async_file_error_code.h
  src/io_buffer.cpp
  src/async_file.cpp
  src/file_stream_adapters.cpp
  src/async_file_posix.h
  src/async_file_win.h
  src/async_line_reader.cpp
  src/stream_reader.cpp
  src/stream_writer.cpp
)

if(WIN32)
  list(APPEND NEIXX_IO_SOURCES
    src/pipe_stream_win.cpp
    src/async_file_win.cpp
  )
else()
  list(APPEND NEIXX_IO_SOURCES
    src/pipe_stream_posix.cpp
    src/async_file_posix.cpp
  )
endif()

target_sources(neixx PRIVATE ${NEIXX_IO_SOURCES})

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
  target_link_libraries(neixx PRIVATE ws2_32)
endif()

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "internal" EXCLUDE
)
