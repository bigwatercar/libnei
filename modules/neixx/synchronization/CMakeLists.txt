target_sources(
    neixx
    PRIVATE
    src/condition_variable.cpp
    src/lock.cpp
        src/waitable_event.cpp
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
