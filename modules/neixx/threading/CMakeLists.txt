target_sources(
    neixx
    PRIVATE
        src/platform_thread_win.cpp
        src/platform_thread_posix.cpp
        src/thread.cpp
    src/thread_id_name_manager.cpp
        src/waitable_event.cpp
        src/scoped_blocking_call.cpp
        src/thread_local_storage.cpp
        src/sequence_local_storage_slot.cpp
        src/thread_restrictions.cpp
)

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
