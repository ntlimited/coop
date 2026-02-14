cmake_minimum_required(VERSION 3.16)
project(coop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Collect library sources
file(GLOB_RECURSE COOP_SOURCES
    coop/*.cpp
    coop/network/*.cpp
    coop/time/*.cpp
    coop/io/*.cpp
)
list(FILTER COOP_SOURCES EXCLUDE REGEX "coop/io/ssl\\.cpp$")

add_library(coop STATIC ${COOP_SOURCES})
target_include_directories(coop PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(coop PRIVATE -Wcpp)

# External libraries
find_library(URING_LIB uring REQUIRED)
find_library(WOLFSSL_LIB wolfssl REQUIRED)
target_link_libraries(coop PUBLIC ${URING_LIB} ${WOLFSSL_LIB})

# Executables
add_executable(server server.cpp)
target_link_libraries(server PRIVATE coop)

add_executable(client client.cpp)
target_link_libraries(client PRIVATE coop)
