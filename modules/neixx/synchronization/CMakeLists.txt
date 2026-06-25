target_sources(
  nei
  PRIVATE include/neixx/synchronization/condition_variable.h
          include/neixx/synchronization/lock.h
          include/neixx/synchronization/waitable_event.h
          src/condition_variable.cpp
          src/lock.cpp
          src/waitable_event.cpp
)

target_include_directories(
  nei PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
