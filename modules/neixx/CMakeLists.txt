# neixx sources are merged into the nei target (single DLL).
# This file only adds C++ compile requirements and subdirectories.

# C++ standard (applies to all .cpp sources in this target)
target_compile_features(nei PUBLIC cxx_std_17)

if(WIN32 AND MSVC)
  # Suppress C4251: needs to have dll-interface for use by clients
  # This warning is expected when exporting STL containers and template classes
  target_compile_options(nei PRIVATE /wd4251)
endif()

# Trace event instrumentation (C++ component)
if(NEI_ENABLE_TRACE_EVENTS)
  target_compile_definitions(nei PUBLIC NEI_ENABLE_TRACE_EVENTS)
endif()

add_subdirectory(common)
add_subdirectory(memory)
add_subdirectory(functional)
add_subdirectory(synchronization)
add_subdirectory(threading)
add_subdirectory(trace_event)
add_subdirectory(task)
add_subdirectory(strings)
add_subdirectory(command_line)
add_subdirectory(io)
add_subdirectory(log)
add_subdirectory(process)
