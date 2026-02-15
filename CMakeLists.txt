cmake_minimum_required(VERSION 3.16)
project(coop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# spdlog — header-only structured logging
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
)
FetchContent_MakeAvailable(spdlog)

# Collect library sources
file(GLOB_RECURSE COOP_SOURCES
    coop/*.cpp
    coop/network/*.cpp
    coop/time/*.cpp
    coop/io/*.cpp
    coop/http/*.cpp
)

add_library(coop STATIC ${COOP_SOURCES})
target_include_directories(coop PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(coop PRIVATE -Wcpp)

# Cooperative threading uses longjmp across user-allocated stacks. glibc's _FORTIFY_SOURCE
# replaces longjmp with __longjmp_chk which validates stack pointers against a single-stack
# model, producing false positives. Disable it for the library.
target_compile_definitions(coop PRIVATE _FORTIFY_SOURCE=0)

# External libraries
find_library(URING_LIB uring REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
target_link_libraries(coop PUBLIC ${URING_LIB} OpenSSL::SSL OpenSSL::Crypto spdlog::spdlog)

# Executables
add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server PRIVATE coop)

add_executable(echo_client examples/echo_client.cpp)
target_link_libraries(echo_client PRIVATE coop)

add_executable(chat_server examples/chat_server.cpp)
target_link_libraries(chat_server PRIVATE coop)

add_executable(tcp_proxy examples/tcp_proxy.cpp)
target_link_libraries(tcp_proxy PRIVATE coop)

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
)
target_link_libraries(coop_tests PRIVATE coop GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(coop_tests)

# Benchmarks
add_executable(coop_benchmarks
    benchmarks/bench_embedded_list.cpp
    benchmarks/bench_coordinator.cpp
    benchmarks/bench_io.cpp
    benchmarks/bench_pthread.cpp
    benchmarks/bench_io_baseline.cpp
)
target_link_libraries(coop_benchmarks PRIVATE coop benchmark::benchmark_main)
