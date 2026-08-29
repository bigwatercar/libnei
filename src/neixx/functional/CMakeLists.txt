target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/functional/bind.h
          ${PROJECT_SOURCE_DIR}/include/neixx/functional/callback.h
          ${PROJECT_SOURCE_DIR}/include/neixx/functional/callback_base.h
          ${PROJECT_SOURCE_DIR}/include/neixx/functional/callback_internal.h
          ${PROJECT_SOURCE_DIR}/include/neixx/functional/cancelable_callback.h
          cancelable_callback.cpp
          callback.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

