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

find_package(ZLIB REQUIRED)

add_library(calf_core STATIC src/record.cpp src/wal.cpp src/head.cpp src/writer.cpp
                             src/chunk/bitstream.cpp src/chunk/xor_chunk.cpp)
target_link_libraries(calf_core PUBLIC calf_protocol ZLIB::ZLIB)

add_executable(server src/main.cpp src/server.cpp)
target_link_libraries(server PRIVATE calf_core)

add_executable(client tools/client.cpp)
target_link_libraries(client PRIVATE calf_protocol)

add_executable(chunk_stats tools/chunk_stats.cpp)
target_link_libraries(chunk_stats PRIVATE calf_core)

add_executable(query tools/query.cpp)
target_link_libraries(query PRIVATE calf_protocol)

add_executable(bench tools/bench.cpp)
target_link_libraries(bench PRIVATE calf_protocol)

enable_testing()
add_executable(record_test tests/record_test.cpp)
target_link_libraries(record_test PRIVATE calf_core)
add_test(NAME record_test COMMAND record_test)

add_executable(chunk_test tests/chunk_test.cpp)
target_link_libraries(chunk_test PRIVATE calf_core)
add_test(NAME chunk_test COMMAND chunk_test)

add_executable(query_test tests/query_test.cpp)
target_link_libraries(query_test PRIVATE calf_core)
add_test(NAME query_test COMMAND query_test)

# The relative script path needs the source dir as the working directory.
add_test(NAME query_e2e COMMAND bash tests/query_e2e.sh $<TARGET_FILE_DIR:server>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(query_e2e PROPERTIES TIMEOUT 120)

# The empty seed makes the script pick one and print it, so a failed run can
# be repeated.
add_test(NAME crash_loop
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_loop.sh "" $<TARGET_FILE_DIR:server>)
set_tests_properties(crash_loop PROPERTIES LABELS slow TIMEOUT 300)

# bad_frames.py reuses the series names s0, s1, ..., so on an old WAL its valid
# cases get status 1. Each run starts a server on a new WAL in a temp dir.
# A TSan report does not stop the server, so its stderr is checked too.
add_test(NAME bad_frames
         COMMAND bash -c [=[
dir=$(mktemp -d)
"$1" 17171 "$dir/bad_frames.wal" 2>"$dir/server.err" &
pid=$!
trap 'kill -9 $pid 2>/dev/null; rm -rf "$dir"' EXIT
for _ in $(seq 400); do
    grep -q "listening on" "$dir/server.err" && break
    if ! kill -0 $pid 2>/dev/null; then cat "$dir/server.err"; exit 1; fi
    sleep 0.05
done
python3 "$2" 17171
rc=$?
if grep -qE "Sanitizer|runtime error" "$dir/server.err"; then cat "$dir/server.err"; rc=1; fi
exit $rc
]=] bad_frames $<TARGET_FILE:server> ${CMAKE_CURRENT_SOURCE_DIR}/tests/bad_frames.py)
set_tests_properties(bad_frames PROPERTIES TIMEOUT 120)
