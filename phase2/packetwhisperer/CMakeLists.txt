# Native half of PacketWhisperer: the Phase 1 C++ parser exposed to Python.
cmake_minimum_required(VERSION 3.16)
project(packetwhisperer_native LANGUAGES CXX)

# Everything linked into a Python extension must be position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(core)

find_package(pybind11 CONFIG REQUIRED)
pybind11_add_module(_pktcore bindings/pktcore_py.cpp)
target_link_libraries(_pktcore PRIVATE pktcore)
netlab_warnings(_pktcore)
install(TARGETS _pktcore DESTINATION packetwhisperer)
