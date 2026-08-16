# HTTP client integration tests
target_sources(nei_tests PRIVATE http_client_integration_test.cpp)

# HTTP parser unit tests
target_sources(nei_tests PRIVATE http_parser_test.cpp)

# WebSocket protocol tests
target_sources(nei_tests PRIVATE websocket_test.cpp)

# HTTP client tests
target_sources(nei_tests PRIVATE http_client_test.cpp)

# HTTP/2 client session tests (raw nghttp2 test server inside the test)
target_sources(nei_tests PRIVATE http2_client_session_test.cpp test_cert.h)
target_link_libraries(nei_tests PRIVATE nghttp2)

# HTTP/2 server integration tests (against Http2ClientSession)
target_sources(nei_tests PRIVATE http2_server_test.cpp)

# Single-port HTTP/1.1 + HTTP/2 multiplexing tests (unified HttpServer)
target_sources(nei_tests PRIVATE http_server_mux_test.cpp)

# Fused HttpClient (h1/h2) multiplexing tests (unified HttpServer)
target_sources(nei_tests PRIVATE http_client_mux_test.cpp)

# HTTP/WebSocket multi-threaded stress tests
target_sources(nei_tests PRIVATE http_multithread_stress_test.cpp)

# HTTP/2 multi-threaded stress tests (TLS, h2 ALPN)
target_sources(nei_tests PRIVATE http2_multithread_stress_test.cpp)

# TCP socket unit tests
target_sources(nei_tests PRIVATE tcp_socket_test.cpp)

# TLS socket unit tests
target_sources(nei_tests PRIVATE tls_socket_test.cpp)

# UDP socket unit tests
target_sources(nei_tests PRIVATE udp_socket_test.cpp)

# HostResolver unit tests (c-ares async DNS)
target_sources(nei_tests PRIVATE host_resolver_test.cpp)

if(WIN32)
  target_link_libraries(nei_tests PRIVATE ws2_32)
endif()
