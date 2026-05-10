target_sources(
    neixx
    PRIVATE
        include/neixx/task/message_loop/message_pump.h
        include/neixx/task/message_loop/message_pump_default.h
        src/message_loop/message_pump_default.cpp
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
