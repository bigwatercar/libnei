# TCP socket unit tests
target_sources(nei_tests PRIVATE tcp_socket_unittest.cpp)

if(WIN32)
  target_link_libraries(nei_tests PRIVATE ws2_32)
endif()
