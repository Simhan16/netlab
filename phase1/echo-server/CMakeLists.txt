cmake_minimum_required(VERSION 3.16)
project(echo_server LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/netlab.cmake)

add_executable(echo_server_blocking src/echo_server_blocking.cpp)
add_executable(echo_client src/echo_client.cpp)
set(echo_targets echo_server_blocking echo_client)

# epoll is Linux-only; on macOS you still get the blocking server and client.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(echo_server_epoll src/echo_server_epoll.cpp)
  list(APPEND echo_targets echo_server_epoll)
endif()

foreach(t IN LISTS echo_targets)
  target_link_libraries(${t} PRIVATE netlab_common)
  netlab_warnings(${t})
endforeach()
