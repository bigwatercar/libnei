target_sources(
    neixx
    PRIVATE
        src/platform_thread_win.cpp
        src/platform_thread_posix.cpp
)

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
