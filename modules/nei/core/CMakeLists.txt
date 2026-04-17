target_sources(
    nei
    PRIVATE
        src/endian.c
)

if(WIN32)
    target_sources(
        nei
        PRIVATE
            src/endian_win.c
    )
else()
    target_sources(
        nei
        PRIVATE
            src/endian_posix.c
    )
endif()

target_include_directories(
    nei
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
