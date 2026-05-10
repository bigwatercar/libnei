add_executable(log_demo log_demo.c)
target_link_libraries(log_demo PRIVATE nei::all)

add_executable(log_immediate_crash_demo log_immediate_crash_demo.c)
target_link_libraries(log_immediate_crash_demo PRIVATE nei::all)

add_executable(string_util_demo string_util_demo.cpp)
target_link_libraries(string_util_demo PRIVATE nei::all)

add_executable(command_line_demo command_line_demo.cpp)
target_link_libraries(command_line_demo PRIVATE nei::all)

add_executable(flake_id_demo flake_id_demo.c)
target_link_libraries(flake_id_demo PRIVATE nei::all)

if(WIN32 AND BUILD_SHARED_LIBS)
  function(nei_copy_runtime_dlls target_name)
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${target_name}> $<TARGET_FILE_DIR:${target_name}>
      COMMAND_EXPAND_LISTS
    )
  endfunction()

  nei_copy_runtime_dlls(log_demo)
  nei_copy_runtime_dlls(log_immediate_crash_demo)
  nei_copy_runtime_dlls(string_util_demo)
  nei_copy_runtime_dlls(command_line_demo)
  nei_copy_runtime_dlls(flake_id_demo)
endif()
