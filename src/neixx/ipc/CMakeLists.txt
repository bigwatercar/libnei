target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/ipc/message_channel.h ${PROJECT_SOURCE_DIR}/include/neixx/ipc/rpc_endpoint.h
          message_channel.cpp rpc_endpoint.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

