target_sources(
  nei
  PRIVATE include/neixx/strings/cjk_width.h
          include/neixx/strings/split_string.h
          include/neixx/strings/string_number_conversions.h
          include/neixx/strings/string_util.h
          include/neixx/strings/text_normalization.h
          include/neixx/strings/utf_string_conversions.h
          src/cjk_width.cpp
          src/split_string.cpp
          src/string_number_conversions.cpp
          src/string_util.cpp
          src/text_normalization.cpp
          src/utf_string_conversions_fallback.h
          src/utf_string_conversions.cpp
)

if(WIN32)
  target_sources(nei PRIVATE src/utf_string_conversions_win.cpp)
elseif(UNIX OR APPLE)
  target_sources(nei PRIVATE src/utf_string_conversions_posix.cpp)
endif()

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
