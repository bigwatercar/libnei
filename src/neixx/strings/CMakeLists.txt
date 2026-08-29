target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/strings/cjk_width.h
          ${PROJECT_SOURCE_DIR}/include/neixx/strings/split_string.h
          ${PROJECT_SOURCE_DIR}/include/neixx/strings/string_number_conversions.h
          ${PROJECT_SOURCE_DIR}/include/neixx/strings/string_util.h
          ${PROJECT_SOURCE_DIR}/include/neixx/strings/text_normalization.h
          ${PROJECT_SOURCE_DIR}/include/neixx/strings/utf_string_conversions.h
          cjk_width.cpp
          split_string.cpp
          string_number_conversions.cpp
          string_util.cpp
          text_normalization.cpp
          utf_string_conversions_fallback.h
          utf_string_conversions.cpp
)

if(WIN32)
  target_sources(nei PRIVATE utf_string_conversions_win.cpp)
elseif(UNIX OR APPLE)
  target_sources(nei PRIVATE utf_string_conversions_posix.cpp)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

