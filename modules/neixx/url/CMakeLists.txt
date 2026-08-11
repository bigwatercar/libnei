#url module — RFC 3986 URL parser and percent - encoding utilities

target_sources(nei PRIVATE include / neixx / url / url.h include / neixx / url / url_encoding.h src / url.cpp src
               / url_encoding.cpp)

    target_include_directories(nei PUBLIC $<BUILD_INTERFACE : ${CMAKE_CURRENT_SOURCE_DIR} / include>
                                   $<INSTALL_INTERFACE : ${CMAKE_INSTALL_INCLUDEDIR}>)
