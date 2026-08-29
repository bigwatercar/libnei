set(NEIXX_NET_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/net/ip_address.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/ip_end_point.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/address_list.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/host_resolver.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/tcp_server_socket.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/tcp_client_socket.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/udp_socket.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/ssl_context.h
    ssl_context.cpp
    ${PROJECT_SOURCE_DIR}/include/neixx/net/tls_client_socket.h
    tls_client_socket.cpp
    ${PROJECT_SOURCE_DIR}/include/neixx/net/tls_server_socket.h
    tls_server_socket.cpp
    ip_address.cpp
    host_resolver_impl.h
    host_resolver_impl.cpp
    cares_context.h
    cares_context.cpp
    tcp_server_socket.cpp
    tcp_client_socket.cpp
    udp_socket.cpp
)

if(WIN32)
  list(
    APPEND
    NEIXX_NET_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/net/wsa_init.h
    wsa_init.cpp
    tcp_server_socket_win.h
    tcp_server_socket_win.cpp
    tcp_client_socket_win.h
    tcp_client_socket_win.cpp
    udp_socket_win.h
    udp_socket_win.cpp
  )
else()
  list(
    APPEND
    NEIXX_NET_SOURCES
    tcp_server_socket_posix.h
    tcp_server_socket_posix.cpp
    tcp_client_socket_posix.h
    tcp_client_socket_posix.cpp
    udp_socket_posix.h
    udp_socket_posix.cpp
  )
endif()

target_sources(nei PRIVATE ${NEIXX_NET_SOURCES})

target_include_directories(
  nei
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
  target_link_libraries(nei PRIVATE ws2_32)
endif()

 # c-ares — asynchronous DNS resolver (external/)
target_link_libraries(nei PRIVATE cares)

 # mbedTLS — TLS/SSL library (external/)
target_link_libraries(nei PRIVATE mbedtls)

 # HTTP/1.1 module (external/llhttp)
set(NEIXX_NET_HTTP_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_common.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_parser.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_request.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_response.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_response_writer.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_server.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_server_request_handle.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_client.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_client_pool.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_request_handle.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http_file_transfer.h
    http/http_common.cpp
    http/http_parser.cpp
    http/http_request.cpp
    http/http_response.cpp
    http/http_response_writer.cpp
    http/http_engine_internal.h
    http/http_server.cpp
    http/http_server_request_handle.cpp
    http/http_client.cpp
    http/http_client_pool.cpp
    http/http_request_handle.cpp
    http/http_file_transfer.cpp
    http/gzip_stream.cpp
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/gzip_stream.h
    http/cookie.cpp
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/cookie.h
    http/redirect_handler.cpp
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/redirect_handler.h
    http/multipart.cpp
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/multipart.h
)
target_sources(nei PRIVATE ${NEIXX_NET_HTTP_SOURCES})
target_link_libraries(nei PRIVATE llhttp zlibstatic)

 # HTTP/2 module (external/nghttp2)
set(NEIXX_NET_HTTP2_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/net/http/http2_client_session.h
    http2/http2_client_session.cpp
    http2/http2_engine.cpp
    http2/nghttp2_internal.h
)
target_sources(nei PRIVATE ${NEIXX_NET_HTTP2_SOURCES})
target_link_libraries(nei PRIVATE nghttp2)

# WebSocket module (RFC 6455)
set(NEIXX_NET_WEBSOCKET_SOURCES
    ${PROJECT_SOURCE_DIR}/include/neixx/net/websocket/websocket_frame.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/websocket/websocket_connection.h
    ${PROJECT_SOURCE_DIR}/include/neixx/net/websocket/websocket_client.h
    websocket/websocket_handshake.cpp
    websocket/websocket_handshake_internal.h
    websocket/websocket_frame.cpp
    websocket/websocket_frame_internal.h
    websocket/websocket_connection.cpp
    websocket/websocket_client.cpp
)
target_sources(nei PRIVATE ${NEIXX_NET_WEBSOCKET_SOURCES})
