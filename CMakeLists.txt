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

add_library(coop STATIC ${COOP_SOURCES})
target_include_directories(coop PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(coop PRIVATE -Wcpp)

# External libraries
find_library(URING_LIB uring REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
target_link_libraries(coop PUBLIC ${URING_LIB} OpenSSL::SSL OpenSSL::Crypto)

# Executables
add_executable(server server.cpp)
target_link_libraries(server PRIVATE coop)

add_executable(client client.cpp)
target_link_libraries(client PRIVATE coop)

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
add_dependencies(server test-certs)
