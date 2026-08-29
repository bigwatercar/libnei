target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/nei/utils/base64.h
          ${PROJECT_SOURCE_DIR}/include/nei/utils/crc32.h
          ${PROJECT_SOURCE_DIR}/include/nei/utils/flake_id.h
          ${PROJECT_SOURCE_DIR}/include/nei/utils/md5.h
          ${PROJECT_SOURCE_DIR}/include/nei/utils/sha1.h
          ${PROJECT_SOURCE_DIR}/include/nei/utils/sha256.h
          ${PROJECT_SOURCE_DIR}/include/nei/utils/uuid.h
          base64.c
          crc32.c
          flake_id.c
          md5.c
          sha1.c
          sha256.c
          uuid.c
)

if(WIN32)
  target_link_libraries(nei PRIVATE bcrypt)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

