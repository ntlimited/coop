cmake_minimum_required(VERSION 3.16)
project(coop LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Code coverage instrumentation (use with build/coverage)
option(COOP_COVERAGE "Build with gcov coverage instrumentation" OFF)
if(COOP_COVERAGE)
    add_compile_options(--coverage -fno-inline)
    add_link_options(--coverage)
endif()

# spdlog — header-only structured logging
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
)
FetchContent_MakeAvailable(spdlog)

# coop/chan — channel toolkit (compiled separately, folded into libcoop.a)
#
# Keeping chan as its own CMake target gives clean module boundaries and dependency
# tracking as the toolkit grows. The OBJECT library approach folds the compiled objects
# directly into the final libcoop.a so consumers see a single library.
#
file(GLOB COOP_CHAN_SOURCES coop/chan/*.cpp)
add_library(coop_chan OBJECT ${COOP_CHAN_SOURCES})
target_include_directories(coop_chan PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(coop_chan PRIVATE -Wcpp -fno-omit-frame-pointer)

# Collect remaining library sources (exclude coop/chan/ — handled by coop_chan above)
#
file(GLOB_RECURSE COOP_SOURCES
    coop/*.cpp
    coop/*.S
    coop/network/*.cpp
    coop/time/*.cpp
    coop/io/*.cpp
    coop/http/*.cpp
)
list(FILTER COOP_SOURCES EXCLUDE REGEX ".*/coop/chan/.*")

add_library(coop STATIC ${COOP_SOURCES} $<TARGET_OBJECTS:coop_chan>)
target_include_directories(coop PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(coop PRIVATE -Wcpp -fno-omit-frame-pointer -msse4.2)

# Compile out trace/debug logging in release builds — eliminates ~1% overhead from runtime
# level checks in the IO hot path (spdlog::trace calls on every Submit/Complete/Poll).
#
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(coop PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN)
endif()

# Performance counter mode: 0=disabled, 1=always-on, 2=dynamic (NOP/JMP patching)
# Default: always-on in debug (useful diagnostics, negligible cost), dynamic in release
# (zero overhead when off, togglable at runtime for observability).
#
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(COOP_PERF_MODE "1" CACHE STRING "Performance counter mode: 0=disabled, 1=always-on, 2=dynamic")
else()
    set(COOP_PERF_MODE "2" CACHE STRING "Performance counter mode: 0=disabled, 1=always-on, 2=dynamic")
endif()
target_compile_definitions(coop PUBLIC COOP_PERF_MODE=${COOP_PERF_MODE})

# External libraries
find_library(URING_LIB uring REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
target_link_libraries(coop PUBLIC ${URING_LIB} OpenSSL::SSL OpenSSL::Crypto spdlog::spdlog dl)

# Executables
add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server PRIVATE coop)
target_link_options(echo_server PRIVATE -rdynamic)


add_executable(echo_client examples/echo_client.cpp)
target_link_libraries(echo_client PRIVATE coop)

add_executable(chat_server examples/chat_server.cpp)
target_link_libraries(chat_server PRIVATE coop)

add_executable(tcp_proxy examples/tcp_proxy.cpp)
target_link_libraries(tcp_proxy PRIVATE coop)

add_executable(pipeline examples/pipeline.cpp)
target_link_libraries(pipeline PRIVATE coop)

# Generate a self-signed TLS certificate for testing if one doesn't already exist
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bin/cert.pem ${CMAKE_BINARY_DIR}/bin/key.pem
    COMMAND openssl req -x509 -newkey rsa:2048
        -keyout ${CMAKE_BINARY_DIR}/bin/key.pem
        -out ${CMAKE_BINARY_DIR}/bin/cert.pem
        -days 365 -nodes -subj "/CN=localhost"
    COMMENT "Generating self-signed TLS certificate for testing"
)
add_custom_target(test-certs DEPENDS
    ${CMAKE_BINARY_DIR}/bin/cert.pem
    ${CMAKE_BINARY_DIR}/bin/key.pem
)
add_dependencies(echo_server test-certs)

# Copy static files to build output so they sit next to executables at runtime
file(GLOB_RECURSE STATIC_FILES coop/http/static/*)
add_custom_target(static-files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/static
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/coop/http/static
        ${CMAKE_BINARY_DIR}/bin/static
    DEPENDS ${STATIC_FILES}
    COMMENT "Copying static files to build output"
)

# Google Test
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.0
)
FetchContent_MakeAvailable(googletest)

# Google Benchmark
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

enable_testing()

add_executable(coop_tests
    tests/test_coordinator.cpp
    tests/test_spawn.cpp
    tests/test_signal.cpp
    tests/test_channel.cpp
    tests/test_shutdown.cpp
    tests/test_io.cpp
    tests/test_stress.cpp
    tests/test_http.cpp
    tests/test_perf.cpp
    tests/test_alloc.cpp
    tests/test_topology.cpp
    tests/test_epoch.cpp
    tests/test_coop_var.cpp
)
target_link_libraries(coop_tests PRIVATE coop GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(coop_tests)

# Benchmarks
add_executable(coop_benchmarks
    benchmarks/bench_embedded_list.cpp
    benchmarks/bench_channel.cpp
    benchmarks/bench_coordinator.cpp
    benchmarks/bench_io.cpp
    benchmarks/bench_pthread.cpp
    benchmarks/bench_io_baseline.cpp
    benchmarks/bench_scheduler.cpp
    benchmarks/bench_uring_config.cpp
    benchmarks/bench_ssl.cpp
    benchmarks/bench_http.cpp
    benchmarks/bench_perf.cpp
)
target_link_libraries(coop_benchmarks PRIVATE coop benchmark::benchmark_main)

add_executable(bench_server benchmarks/bench_server.cpp)
target_link_libraries(bench_server PRIVATE coop)
target_link_options(bench_server PRIVATE -rdynamic)

add_executable(bench_client benchmarks/bench_client.cpp)
target_link_libraries(bench_client PRIVATE coop)
target_link_options(bench_client PRIVATE -rdynamic)

add_executable(diag_ktls benchmarks/diag_ktls.cpp)
target_link_libraries(diag_ktls PRIVATE coop)
add_dependencies(diag_ktls test-certs)
