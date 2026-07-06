set(NEIXX_NET_SOURCES
  include/neixx/net/ip_address.h
  include/neixx/net/ip_end_point.h
  include/neixx/net/address_list.h
  include/neixx/net/host_resolver.h
  include/neixx/net/tcp_server_socket.h
  include/neixx/net/tcp_client_socket.h
  src/ip_address.cpp
  src/host_resolver_impl.h
  src/host_resolver_impl.cpp
  src/cares_context.h
  src/cares_context.cpp
  src/tcp_server_socket.cpp
  src/tcp_client_socket.cpp
)

if(WIN32)
  list(APPEND NEIXX_NET_SOURCES
    include/neixx/net/wsa_init.h
    src/wsa_init.cpp
    src/tcp_server_socket_win.h
    src/tcp_server_socket_win.cpp
    src/tcp_client_socket_win.h
    src/tcp_client_socket_win.cpp
  )
else()
  list(APPEND NEIXX_NET_SOURCES
    src/tcp_server_socket_posix.h
    src/tcp_server_socket_posix.cpp
    src/tcp_client_socket_posix.h
    src/tcp_client_socket_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_NET_SOURCES})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
  target_link_libraries(nei PRIVATE ws2_32)
endif()

# c-ares — asynchronous DNS resolver (3rdparty/)
target_link_libraries(nei PRIVATE cares)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
