# Superbuild for every Phase 1 C++ tool. Each subproject also builds standalone.
cmake_minimum_required(VERSION 3.16)
project(netlab_phase1 LANGUAGES CXX)

include(cmake/netlab.cmake)

add_subdirectory(echo-server)
add_subdirectory(dns-resolver)
add_subdirectory(udp-ping)
add_subdirectory(mini-tcpdump)
