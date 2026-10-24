cmake_minimum_required(VERSION 3.16)
project(mini_tcpdump LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/netlab.cmake)

option(NETLAB_BUILD_TOOLS "Build CLIs and tests (PacketWhisperer turns this off)" ON)

find_path(PCAP_INCLUDE_DIR pcap/pcap.h REQUIRED)
find_library(PCAP_LIBRARY pcap REQUIRED)

if(NOT TARGET dnscodec)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../dns-resolver ${CMAKE_BINARY_DIR}/dns-resolver)
endif()

# pktcore: decoding + flow analysis. Shared with phase2/packetwhisperer.
add_library(pktcore STATIC
  src/core/packet.cpp
  src/core/tls.cpp
  src/core/analyzer.cpp
  src/core/pcap_source.cpp)
target_include_directories(pktcore PUBLIC src/core)
target_include_directories(pktcore SYSTEM PUBLIC ${PCAP_INCLUDE_DIR})
target_link_libraries(pktcore PUBLIC dnscodec netlab_common ${PCAP_LIBRARY})
netlab_warnings(pktcore)

if(NETLAB_BUILD_TOOLS)
  add_executable(mini-tcpdump src/tool/main.cpp)
  target_link_libraries(mini-tcpdump PRIVATE pktcore)
  netlab_warnings(mini-tcpdump)

  add_executable(test_pktcore tests/test_pktcore.cpp)
  target_link_libraries(test_pktcore PRIVATE pktcore)
  netlab_warnings(test_pktcore)
  add_test(NAME pktcore COMMAND test_pktcore)
endif()
