cmake_minimum_required(VERSION 3.20)
project(calf CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

set(CALF_SANITIZE "" CACHE STRING "Sanitizer build: address, thread, or empty")
if(CALF_SANITIZE STREQUAL "address")
    set(sanitize_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined
                       -fno-omit-frame-pointer)
elseif(CALF_SANITIZE STREQUAL "thread")
    set(sanitize_flags -fsanitize=thread)
elseif(NOT CALF_SANITIZE STREQUAL "")
    message(FATAL_ERROR "CALF_SANITIZE must be address, thread, or empty")
endif()
add_compile_options(${sanitize_flags})
add_link_options(${sanitize_flags})

add_library(calf_protocol STATIC src/protocol.cpp)
target_include_directories(calf_protocol PUBLIC src)

add_executable(server src/main.cpp src/server.cpp)
target_link_libraries(server PRIVATE calf_protocol)

add_executable(client tools/client.cpp)
target_link_libraries(client PRIVATE calf_protocol)
