cmake_minimum_required(VERSION 3.16)
project(socket_basics LANGUAGES CXX)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/netlab.cmake)

# Each step builds twice: exercises/<step>.cpp (yours, starts as a stub) and
# solutions/<step>.cpp (the reference) as solution_<step>.
set(socket_steps step2_client step3_server step4_framed_server step5_udp_server step6_chat_server)

foreach(step IN LISTS socket_steps)
  add_executable(${step} exercises/${step}.cpp)
  add_executable(solution_${step} solutions/${step}.cpp)
  foreach(t ${step} solution_${step})
    target_link_libraries(${t} PRIVATE netlab_common)
    netlab_warnings(${t})
  endforeach()
endforeach()

# ctest checks the reference solutions, so the build stays green while your
# exercises are still stubs. Check your own with: python3 check.py <step>
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME socket_basics_solutions
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check.py all --solution
                   --build-dir ${CMAKE_CURRENT_BINARY_DIR})
endif()
