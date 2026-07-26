# TCP socket unit tests
target_sources(nei_tests PRIVATE tcp_socket_unittest.cpp)

# TLS socket unit tests
target_sources(nei_tests PRIVATE tls_socket_unittest.cpp)

# UDP socket unit tests
target_sources(nei_tests PRIVATE udp_socket_unittest.cpp)

# HostResolver unit tests (c-ares async DNS)
target_sources(nei_tests PRIVATE host_resolver_unittest.cpp)

if(WIN32)
  target_link_libraries(nei_tests PRIVATE ws2_32)
endif()
