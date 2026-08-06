set(NEIXX_IO_SOURCES
    include/neixx/io/async_file.h
    include/neixx/io/async_line_reader.h
    include/neixx/io/async_stream.h
    include/neixx/io/buffer_stream.h
    include/neixx/io/file_stream_adapters.h
    include/neixx/io/io_buffer.h
    include/neixx/io/pipe_stream.h
    include/neixx/io/stream_reader.h
    include/neixx/io/stream_writer.h
    src/async_file.cpp
    src/async_line_reader.cpp
    src/buffer_stream.cpp
    src/file_stream_adapters.cpp
    src/internal/async_file_error_code.h
    src/io_buffer.cpp
    src/pipe_stream.cpp
    src/stream_reader.cpp
    src/stream_writer.cpp
)

if(WIN32)
  list(
    APPEND
    NEIXX_IO_SOURCES
    src/async_file_win.h
    src/async_file_win.cpp
    src/pipe_stream_win.h
    src/pipe_stream_win.cpp
  )
else()
  list(
    APPEND
    NEIXX_IO_SOURCES
    src/async_file_posix.h
    src/async_file_posix.cpp
    src/pipe_stream_posix.h
    src/pipe_stream_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_IO_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
  target_link_libraries(nei PRIVATE ws2_32)
endif()

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "internal" EXCLUDE
)
