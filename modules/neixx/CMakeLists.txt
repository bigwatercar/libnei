add_library(neixx)
add_library(nei::neixx ALIAS neixx)

target_compile_features(neixx PUBLIC cxx_std_17)
target_link_libraries(neixx PUBLIC nei)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(neixx PRIVATE NEI_EXPORTS)
    if(NOT WIN32)
        set_target_properties(
            neixx
            PROPERTIES
                C_VISIBILITY_PRESET hidden
                CXX_VISIBILITY_PRESET hidden
                VISIBILITY_INLINES_HIDDEN YES
        )
    endif()
else()
    target_compile_definitions(neixx PUBLIC NEI_STATIC)
endif()

add_subdirectory(functional)
add_subdirectory(memory)
add_subdirectory(command_line)
add_subdirectory(io)
add_subdirectory(task)
add_subdirectory(strings)
