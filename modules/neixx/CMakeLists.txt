add_library(neixx)
add_library(nei::neixx ALIAS neixx)

target_compile_features(neixx PUBLIC cxx_std_17)
target_link_libraries(neixx PUBLIC nei)

if(WIN32)
  target_compile_definitions(neixx PUBLIC NOMINMAX)
  # Suppress C4251: needs to have dll-interface for use by clients
  # This warning is expected when exporting STL containers and template classes
  if(MSVC)
    target_compile_options(neixx PRIVATE /wd4251)
  endif()
endif()

if(BUILD_SHARED_LIBS)
  target_compile_definitions(neixx PRIVATE NEI_EXPORTS)
  if(NOT WIN32)
    set_target_properties(
      neixx
      PROPERTIES C_VISIBILITY_PRESET hidden
                 CXX_VISIBILITY_PRESET hidden
                 VISIBILITY_INLINES_HIDDEN YES
    )
  endif()
else()
  target_compile_definitions(neixx PUBLIC NEI_STATIC)
endif()

add_subdirectory(common)
add_subdirectory(memory)
add_subdirectory(functional)
add_subdirectory(synchronization)
add_subdirectory(threading)
add_subdirectory(task)
add_subdirectory(strings)
add_subdirectory(command_line)
add_subdirectory(io)
add_subdirectory(log)
add_subdirectory(process)
