# Shared build settings. Included by the superbuild and by each project when
# built standalone, so it must be safe to include more than once.
include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

enable_testing()

function(netlab_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endfunction()

if(NOT TARGET netlab_common)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../common ${CMAKE_BINARY_DIR}/netlab_common)
endif()
