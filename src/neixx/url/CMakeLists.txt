# url module — RFC 3986 URL parser and percent-encoding utilities

target_sources(nei PRIVATE
    ${PROJECT_SOURCE_DIR}/include/neixx/url/url.h
    ${PROJECT_SOURCE_DIR}/include/neixx/url/url_encoding.h
    url.cpp
    url_encoding.cpp
)

target_include_directories(nei
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
