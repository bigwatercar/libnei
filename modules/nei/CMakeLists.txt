# C standard (applies to all .c sources in this target)
target_compile_features(nei PUBLIC c_std_99)

if(NOT WIN32)
  target_link_options(nei INTERFACE -rdynamic)
endif()

# --- Log ring buffer configuration (C component) ---
set(NEI_LOG_RING_SLOTS
    "256"
    CACHE STRING "Log MPSC ring slots (power of two)"
)

if(NOT NEI_LOG_RING_SLOTS MATCHES "^[0-9]+$")
  message(
    FATAL_ERROR
      "NEI_LOG_RING_SLOTS must be a positive integer, got: ${NEI_LOG_RING_SLOTS}"
  )
endif()
math(EXPR _nei_log_ring_slots_val "${NEI_LOG_RING_SLOTS}")
if(_nei_log_ring_slots_val LESS 64)
  message(
    FATAL_ERROR "NEI_LOG_RING_SLOTS must be >= 64, got: ${NEI_LOG_RING_SLOTS}"
  )
endif()
math(EXPR _nei_log_ring_slots_pow2_check
     "${_nei_log_ring_slots_val} & (${_nei_log_ring_slots_val} - 1)"
)
if(NOT _nei_log_ring_slots_pow2_check EQUAL 0)
  message(
    FATAL_ERROR
      "NEI_LOG_RING_SLOTS must be a power of two, got: ${NEI_LOG_RING_SLOTS}"
  )
endif()

add_subdirectory(macros)
add_subdirectory(log)
add_subdirectory(debug)
add_subdirectory(core)
add_subdirectory(utils)
add_subdirectory(sys)
add_subdirectory(xdr)
