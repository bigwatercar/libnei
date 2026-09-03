set(NEIXX_IO_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/io/async_file.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/async_line_reader.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/async_stream.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/buffer_stream.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/file_stream_adapters.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/io_buffer.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/io_thread.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/pipe_stream.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/stream_reader.h
    ${PROJECT_SOURCE_DIR}/include/neixx/io/stream_writer.h
    async_file.cpp
    async_line_reader.cpp
    buffer_stream.cpp
    file_stream_adapters.cpp
    internal/async_file_error_code.h
    io_buffer.cpp
    io_thread.cpp
    pipe_stream.cpp
    stream_reader.cpp
    stream_writer.cpp
)

if(WIN32)
  list(
    APPEND
    NEIXX_IO_SOURCES
    async_file_win.h
    async_file_win.cpp
    pipe_stream_win.h
    pipe_stream_win.cpp
  )
else()
  list(
    APPEND
    NEIXX_IO_SOURCES
    async_file_posix.h
    async_file_posix.cpp
    pipe_stream_posix.h
    pipe_stream_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_IO_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
  target_link_libraries(nei PRIVATE ws2_32)
endif()
