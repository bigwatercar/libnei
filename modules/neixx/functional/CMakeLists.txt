target_sources(
  neixx
  PRIVATE include/neixx/functional/bind.h
          include/neixx/functional/callback.h
          include/neixx/functional/callback_base.h
          include/neixx/functional/callback_internal.h
          include/neixx/functional/cancelable_callback.h
          src/cancelable_callback.cpp
          src/callback_base.cpp
          src/callback.cpp
)

target_include_directories(
  neixx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
