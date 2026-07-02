set(NEIXX_NET_SOURCES
  include/neixx/net/ip_address.h
  include/neixx/net/ip_end_point.h
  include/neixx/net/address_list.h
  include/neixx/net/host_resolver.h
  src/ip_address.cpp
  src/host_resolver_impl.h
  src/host_resolver_impl.cpp
)

target_sources(nei PRIVATE ${NEIXX_NET_SOURCES})

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
  target_link_libraries(nei PRIVATE ws2_32)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
