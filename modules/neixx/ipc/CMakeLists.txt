target_sources(
  nei
  PRIVATE include/neixx/ipc/message_channel.h include/neixx/ipc/rpc_endpoint.h
          src/message_channel.cpp src/rpc_endpoint.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
