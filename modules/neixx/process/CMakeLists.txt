target_sources(
    neixx
    PRIVATE
        src/process.cpp
        src/launch.cpp
)

if(WIN32)
    target_sources(
        neixx
        PRIVATE
            src/process_win.cpp
            src/launch_win.cpp
    )
else()
    target_sources(
        neixx
        PRIVATE
            src/process_posix.cpp
            src/launch_posix.cpp
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
