target_sources(
    nei
    PRIVATE
        src/base64.c
        src/crc32.c
        src/flake_id.c
        src/md5.c
        src/sha1.c
        src/sha256.c
        src/uuid.c
)

if(WIN32)
    target_link_libraries(nei PRIVATE bcrypt)
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
