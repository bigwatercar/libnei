target_sources(
  nei
  PRIVATE ${PROJECT_SOURCE_DIR}/include/neixx/synchronization/atomic_event.h
          ${PROJECT_SOURCE_DIR}/include/neixx/synchronization/condition_variable.h
          ${PROJECT_SOURCE_DIR}/include/neixx/synchronization/lock.h
          ${PROJECT_SOURCE_DIR}/include/neixx/synchronization/waitable_event.h
          atomic_event.cpp
          condition_variable.cpp
          lock.cpp
          waitable_event.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

