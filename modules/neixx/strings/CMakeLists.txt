target_sources(
    neixx
    PRIVATE
        src/utf_string_conversions.cpp
        src/string_util.cpp
        src/split_string.cpp
        src/string_number_conversions.cpp
)

if(NEI_ENABLE_CHINESE_LOCALE)
    target_sources(
        neixx
        PRIVATE
            src/text_normalization.cpp
            src/cjk_width.cpp
            src/chinese_search_key.cpp
    )
endif()

if(NEI_ENABLE_CHINESE_LOCALE)
    target_compile_definitions(neixx PUBLIC NEI_ENABLE_CHINESE_LOCALE=1)
else()
    target_compile_definitions(neixx PUBLIC NEI_ENABLE_CHINESE_LOCALE=0)
endif()

if(WIN32)
    target_sources(
        neixx
        PRIVATE
            src/utf_string_conversions_win.cpp
    )
elseif(UNIX OR APPLE)
    target_sources(
        neixx
        PRIVATE
            src/utf_string_conversions_posix.cpp
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
