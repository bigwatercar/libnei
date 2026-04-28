add_library(nei)
add_library(nei::nei ALIAS nei)

target_compile_features(nei PUBLIC c_std_99)

if(NOT WIN32)
    target_link_options(nei INTERFACE -rdynamic)
endif()

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(nei PUBLIC Threads::Threads)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(nei PRIVATE NEI_EXPORTS)
    if(NOT WIN32)
        set_target_properties(
            nei
            PROPERTIES
                C_VISIBILITY_PRESET hidden
                CXX_VISIBILITY_PRESET hidden
                VISIBILITY_INLINES_HIDDEN YES
        )
    endif()
else()
    target_compile_definitions(nei PUBLIC NEI_STATIC)
endif()

add_subdirectory(macros)
add_subdirectory(debug)
add_subdirectory(log)
add_subdirectory(utils)
add_subdirectory(xdr)
add_subdirectory(core)
