option(NEIXX_BUILD_STRINGS_DEMO "Build neixx strings demo" OFF)

target_sources(
    neixx
    PRIVATE
        src/utf_string_conversions.cpp
    src/string_util.cpp
)

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

if(NEIXX_BUILD_STRINGS_DEMO)
    add_executable(neixx_strings_demo main.cpp)
    target_link_libraries(neixx_strings_demo PRIVATE neixx)
    target_compile_features(neixx_strings_demo PRIVATE cxx_std_17)
endif()
