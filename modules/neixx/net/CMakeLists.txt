set(NEIXX_NET_SOURCES
    include/neixx/net/ip_address.h
    include/neixx/net/ip_end_point.h
    include/neixx/net/address_list.h
    include/neixx/net/host_resolver.h
    include/neixx/net/tcp_server_socket.h
    include/neixx/net/tcp_client_socket.h
    include/neixx/net/udp_socket.h
    include/neixx/net/ssl_context.h
    src/ssl_context.cpp
    include/neixx/net/tls_client_socket.h
    src/tls_client_socket.cpp
    include/neixx/net/tls_server_socket.h
    src/tls_server_socket.cpp
    src/ip_address.cpp
    src/host_resolver_impl.h
    src/host_resolver_impl.cpp
    src/cares_context.h
    src/cares_context.cpp
    src/tcp_server_socket.cpp
    src/tcp_client_socket.cpp
    src/udp_socket.cpp
)

if(WIN32)
  list(
    APPEND
    NEIXX_NET_SOURCES
    include/neixx/net/wsa_init.h
    src/wsa_init.cpp
    src/tcp_server_socket_win.h
    src/tcp_server_socket_win.cpp
    src/tcp_client_socket_win.h
    src/tcp_client_socket_win.cpp
    src/udp_socket_win.h
    src/udp_socket_win.cpp
  )
else()
  list(
    APPEND
    NEIXX_NET_SOURCES
    src/tcp_server_socket_posix.h
    src/tcp_server_socket_posix.cpp
    src/tcp_client_socket_posix.h
    src/tcp_client_socket_posix.cpp
    src/udp_socket_posix.h
    src/udp_socket_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_NET_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
  target_link_libraries(nei PRIVATE ws2_32)
endif()

# c-ares — asynchronous DNS resolver (3rdparty/)
target_link_libraries(nei PRIVATE cares)

# mbedTLS — TLS/SSL library (3rdparty/)
target_link_libraries(nei PRIVATE mbedtls)

# HTTP/1.1 module (3rdparty/llhttp)
set(NEIXX_NET_HTTP_SOURCES
    include/neixx/net/http/http_common.h
    include/neixx/net/http/http_parser.h
    include/neixx/net/http/http_request.h
    include/neixx/net/http/http_response.h
    include/neixx/net/http/http_response_writer.h
    include/neixx/net/http/http_server.h
    include/neixx/net/http/http_server_request_handle.h
    include/neixx/net/http/http_client.h
    include/neixx/net/http/http_client_pool.h
    include/neixx/net/http/http_request_handle.h
    include/neixx/net/http/http_file_transfer.h
    src/http/http_common.cpp
    src/http/http_parser.cpp
    src/http/http_request.cpp
    src/http/http_response.cpp
    src/http/http_response_writer.cpp
    src/http/http_engine_internal.h
    src/http/http_server.cpp
    src/http/http_server_request_handle.cpp
    src/http/http_client.cpp
    src/http/http_client_pool.cpp
    src/http/http_request_handle.cpp
    src/http/http_file_transfer.cpp
    src/http/gzip_stream.cpp
    include/neixx/net/http/gzip_stream.h
)
target_sources(nei PRIVATE ${NEIXX_NET_HTTP_SOURCES})
target_link_libraries(nei PRIVATE llhttp zlibstatic)

# HTTP/2 module (3rdparty/nghttp2)
set(NEIXX_NET_HTTP2_SOURCES
    include/neixx/net/http/http2_client_session.h
    src/http2/http2_client_session.cpp
    src/http2/http2_engine.cpp
    src/http2/nghttp2_internal.h
)
target_sources(nei PRIVATE ${NEIXX_NET_HTTP2_SOURCES})
target_link_libraries(nei PRIVATE nghttp2)

# WebSocket module (RFC 6455)
set(NEIXX_NET_WEBSOCKET_SOURCES
    include/neixx/net/websocket/websocket_frame.h
    include/neixx/net/websocket/websocket_connection.h
    include/neixx/net/websocket/websocket_client.h
    src/websocket/websocket_handshake.cpp
    src/websocket/websocket_handshake_internal.h
    src/websocket/websocket_frame.cpp
    src/websocket/websocket_frame_internal.h
    src/websocket/websocket_connection.cpp
    src/websocket/websocket_client.cpp
)
target_sources(nei PRIVATE ${NEIXX_NET_WEBSOCKET_SOURCES})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
