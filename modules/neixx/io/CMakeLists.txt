target_sources(
    neixx
    PRIVATE
    src/io_operation.cpp
    src/file_handle.cpp
    src/io_buffer.cpp
    src/io_context.cpp
    src/async_handle.cpp
)

if(WIN32)
    target_sources(
        neixx
        PRIVATE
        src/file_handle_win.cpp
        src/io_buffer_win.cpp
        src/io_context_win.cpp
        src/async_handle_win.cpp
    )
else()
    target_sources(
        neixx
        PRIVATE
        src/file_handle_posix.cpp
        src/io_buffer_posix.cpp
        src/io_context_posix.cpp
        src/async_handle_posix.cpp
    )
endif()

target_include_directories(
    neixx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
