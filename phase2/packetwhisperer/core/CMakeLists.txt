# The extraction layer is phase1/mini-tcpdump's pktcore library, built from
# the same sources rather than copied, so a parser fix lands in both tools.
set(NETLAB_BUILD_TOOLS OFF CACHE BOOL "Skip the Phase 1 CLIs and tests" FORCE)
set(NETLAB_PHASE1_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../phase1" CACHE PATH "Path to phase1/")

include(${NETLAB_PHASE1_DIR}/cmake/netlab.cmake)
add_subdirectory(${NETLAB_PHASE1_DIR}/mini-tcpdump ${CMAKE_BINARY_DIR}/mini-tcpdump)
