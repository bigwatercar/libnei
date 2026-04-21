add_executable(log_demo log_demo.c)
target_link_libraries(log_demo PRIVATE nei::all)

add_executable(task_thread_demo task_thread_demo.cpp)
target_link_libraries(task_thread_demo PRIVATE nei::all)

add_executable(task_weak_ptr_demo task_weak_ptr_demo.cpp)
target_link_libraries(task_weak_ptr_demo PRIVATE nei::all)

add_executable(task_location_delay_demo task_location_delay_demo.cpp)
target_link_libraries(task_location_delay_demo PRIVATE nei::all)

add_executable(task_priority_demo task_priority_demo.cpp)
target_link_libraries(task_priority_demo PRIVATE nei::all)

add_executable(task_shutdown_demo task_shutdown_demo.cpp)
target_link_libraries(task_shutdown_demo PRIVATE nei::all)

add_executable(task_may_block_demo task_may_block_demo.cpp)
target_link_libraries(task_may_block_demo PRIVATE nei::all)

add_executable(string_util_demo string_util_demo.cpp)
target_link_libraries(string_util_demo PRIVATE nei::all)

add_executable(flake_id_demo flake_id_demo.c)
target_link_libraries(flake_id_demo PRIVATE nei::all)

if(WIN32 AND BUILD_SHARED_LIBS)
  function(nei_copy_runtime_dlls target_name)
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target_name}> $<TARGET_FILE_DIR:${target_name}>
      COMMAND_EXPAND_LISTS
    )
  endfunction()

  nei_copy_runtime_dlls(log_demo)
  nei_copy_runtime_dlls(task_thread_demo)
  nei_copy_runtime_dlls(task_weak_ptr_demo)
  nei_copy_runtime_dlls(task_location_delay_demo)
  nei_copy_runtime_dlls(task_priority_demo)
  nei_copy_runtime_dlls(task_shutdown_demo)
  nei_copy_runtime_dlls(task_may_block_demo)
  nei_copy_runtime_dlls(string_util_demo)
  nei_copy_runtime_dlls(flake_id_demo)
endif()
