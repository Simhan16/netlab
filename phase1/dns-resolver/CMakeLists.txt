cmake_minimum_required(VERSION 3.16)
project(dns_resolver LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/netlab.cmake)

# The codec is a library so mini-tcpdump (and PacketWhisperer) can decode DNS too.
add_library(dnscodec STATIC src/dns.cpp)
target_include_directories(dnscodec PUBLIC src)
target_link_libraries(dnscodec PUBLIC netlab_common)
netlab_warnings(dnscodec)

if(NETLAB_BUILD_TOOLS OR NOT DEFINED NETLAB_BUILD_TOOLS)
  add_executable(dnsq src/main.cpp)
  target_link_libraries(dnsq PRIVATE dnscodec)
  netlab_warnings(dnsq)

  add_executable(test_dns tests/test_dns.cpp)
  target_link_libraries(test_dns PRIVATE dnscodec)
  netlab_warnings(test_dns)
  add_test(NAME dns_codec COMMAND test_dns)
endif()
