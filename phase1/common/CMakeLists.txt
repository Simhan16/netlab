add_library(netlab_common STATIC src/socket.cpp)
target_include_directories(netlab_common PUBLIC include)
netlab_warnings(netlab_common)
