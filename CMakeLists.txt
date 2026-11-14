cmake_minimum_required(VERSION 3.20)
project(calf CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(calf_protocol STATIC src/protocol.cpp)
target_include_directories(calf_protocol PUBLIC src)

add_executable(server src/main.cpp src/server.cpp)
target_link_libraries(server PRIVATE calf_protocol)

add_executable(client tools/client.cpp)
target_link_libraries(client PRIVATE calf_protocol)
