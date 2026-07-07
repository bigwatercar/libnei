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

function(nei_set_utf8_encoding target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /utf-8)
  endif()
endfunction()
