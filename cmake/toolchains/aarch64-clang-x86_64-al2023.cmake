# Cross-compile from the published aarch64 Clang 18 bundle to AL2023 x86-64-v2.
#
# The bundle is supplied by its publisher. This file deliberately does not
# provision a compiler or substitute host tools when that bundle is absent.

if(CMAKE_VERSION VERSION_LESS "3.21")
    message(FATAL_ERROR
        "COOP_CROSS_X86_64_MANIFEST_MALFORMED: CMake 3.21 or newer is required to validate "
        "the bundle manifest.")
endif()

if(NOT DEFINED ENV{COOP_CROSS_X86_64_ROOT} OR "$ENV{COOP_CROSS_X86_64_ROOT}" STREQUAL "")
    message(FATAL_ERROR
        "COOP_CROSS_X86_64_ROOT_MISSING: set COOP_CROSS_X86_64_ROOT to the published "
        "aarch64-clang-x86_64-al2023-v4 bundle.")
endif()

set(_coop_cross_root "$ENV{COOP_CROSS_X86_64_ROOT}")
if(NOT IS_DIRECTORY "${_coop_cross_root}")
    message(FATAL_ERROR
        "COOP_CROSS_X86_64_ROOT_MISSING: COOP_CROSS_X86_64_ROOT is not a directory: "
        "${_coop_cross_root}")
endif()

file(REAL_PATH "${_coop_cross_root}" _coop_cross_real_root)

find_program(_coop_cross_host_test NAMES test NO_CMAKE_FIND_ROOT_PATH)
if(NOT _coop_cross_host_test)
    message(FATAL_ERROR
        "COOP_CROSS_X86_64_HOST_TEST_MISSING: a host POSIX test executable is required to "
        "validate the cross-toolchain bundle.")
endif()

function(_coop_cross_require_contained _path _diagnostic _description)
    file(REAL_PATH "${_path}" _real_path)
    file(RELATIVE_PATH _relative_path "${_coop_cross_real_root}" "${_real_path}")
    if(_relative_path MATCHES "^\\.\\.(/|$)")
        message(FATAL_ERROR "${_diagnostic}: ${_description} is outside ${_coop_cross_real_root}.")
    endif()
endfunction()

function(_coop_cross_require_regular_file _path _diagnostic _description)
    if(IS_SYMLINK "${_path}")
        message(FATAL_ERROR "${_diagnostic}: ${_description} must not be a symlink.")
    endif()

    execute_process(
        COMMAND "${_coop_cross_host_test}" -f "${_path}"
        RESULT_VARIABLE _regular_result)
    if(NOT "${_regular_result}" STREQUAL "0")
        message(FATAL_ERROR "${_diagnostic}: ${_description} must be a regular file.")
    endif()

    _coop_cross_require_contained("${_path}" "${_diagnostic}" "${_description}")
endfunction()

function(_coop_cross_require_directory _path _diagnostic _description)
    if(IS_SYMLINK "${_path}" OR NOT IS_DIRECTORY "${_path}")
        message(FATAL_ERROR "${_diagnostic}: ${_description} must be a directory, not a symlink.")
    endif()

    _coop_cross_require_contained("${_path}" "${_diagnostic}" "${_description}")
endfunction()

function(_coop_cross_require_tool _path _diagnostic _description)
    _coop_cross_require_regular_file("${_path}" "${_diagnostic}" "${_description}")

    execute_process(
        COMMAND "${_coop_cross_host_test}" -x "${_path}"
        RESULT_VARIABLE _executable_result)
    if(NOT "${_executable_result}" STREQUAL "0")
        message(FATAL_ERROR "${_diagnostic}: ${_description} must be executable.")
    endif()

    execute_process(
        COMMAND "${_path}" --version
        RESULT_VARIABLE _version_result
        TIMEOUT 5
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT "${_version_result}" STREQUAL "0")
        message(FATAL_ERROR "${_diagnostic}: ${_description} must execute successfully.")
    endif()
endfunction()

set(_coop_cross_manifest "${_coop_cross_root}/manifest.json")
_coop_cross_require_regular_file("${_coop_cross_manifest}"
    COOP_CROSS_X86_64_MANIFEST_MALFORMED "manifest.json")

file(READ "${_coop_cross_manifest}" _coop_cross_manifest_json)
string(JSON _coop_cross_manifest_type ERROR_VARIABLE _coop_cross_manifest_error TYPE
    "${_coop_cross_manifest_json}")
if(NOT _coop_cross_manifest_error STREQUAL "NOTFOUND" OR
   NOT _coop_cross_manifest_type STREQUAL "OBJECT")
    message(FATAL_ERROR
        "COOP_CROSS_X86_64_MANIFEST_MALFORMED: manifest.json is not a JSON object.")
endif()

function(_coop_cross_require_manifest_string _member _expected)
    string(JSON _type ERROR_VARIABLE _error TYPE "${_coop_cross_manifest_json}" "${_member}")
    if(NOT _error STREQUAL "NOTFOUND" OR NOT _type STREQUAL "STRING")
        message(FATAL_ERROR
            "COOP_CROSS_X86_64_MANIFEST_MALFORMED: manifest.json.${_member} must be a string.")
    endif()

    string(JSON _actual GET "${_coop_cross_manifest_json}" "${_member}")
    if(NOT _actual STREQUAL "${_expected}")
        message(FATAL_ERROR
            "COOP_CROSS_X86_64_MANIFEST_MALFORMED: manifest.json.${_member} must be "
            "${_expected}, got ${_actual}.")
    endif()
endfunction()

function(_coop_cross_require_manifest_number _member _expected)
    string(JSON _type ERROR_VARIABLE _error TYPE "${_coop_cross_manifest_json}" "${_member}")
    if(NOT _error STREQUAL "NOTFOUND" OR NOT _type STREQUAL "NUMBER")
        message(FATAL_ERROR
            "COOP_CROSS_X86_64_MANIFEST_MALFORMED: manifest.json.${_member} must be a number.")
    endif()

    string(JSON _actual GET "${_coop_cross_manifest_json}" "${_member}")
    if(NOT _actual STREQUAL "${_expected}")
        message(FATAL_ERROR
            "COOP_CROSS_X86_64_MANIFEST_MALFORMED: manifest.json.${_member} must be "
            "${_expected}, got ${_actual}.")
    endif()
endfunction()

_coop_cross_require_manifest_number(schema_version 1)
_coop_cross_require_manifest_string(bundle_id aarch64-clang-x86_64-al2023-v4)
_coop_cross_require_manifest_string(exec_arch aarch64)
_coop_cross_require_manifest_string(target_arch x86_64)
_coop_cross_require_manifest_string(target_cpu x86-64-v2)
_coop_cross_require_manifest_string(target_libc glibc-2.34)
_coop_cross_require_manifest_string(cxx_standard_library gcc-11-libstdc++)

set(_coop_cross_compiler "${_coop_cross_root}/bin/clang")
set(_coop_cross_cxx_compiler "${_coop_cross_root}/bin/clang++")
_coop_cross_require_tool("${_coop_cross_compiler}" COOP_CROSS_X86_64_COMPILER_MISSING
    "expected bin/clang in ${_coop_cross_root}")
_coop_cross_require_tool("${_coop_cross_cxx_compiler}" COOP_CROSS_X86_64_COMPILER_MISSING
    "expected bin/clang++ in ${_coop_cross_root}")

set(_coop_cross_linker "${_coop_cross_root}/bin/ld.lld")
_coop_cross_require_tool("${_coop_cross_linker}" COOP_CROSS_X86_64_LINKER_MISSING
    "expected bin/ld.lld in ${_coop_cross_root}")

set(_coop_cross_resource_dir "${_coop_cross_root}/usr/lib/clang/18")
_coop_cross_require_directory("${_coop_cross_resource_dir}"
    COOP_CROSS_X86_64_RESOURCE_DIR_MISSING "expected usr/lib/clang/18 in ${_coop_cross_root}")

set(_coop_cross_sysroot "${_coop_cross_root}/sysroot")
_coop_cross_require_directory("${_coop_cross_sysroot}" COOP_CROSS_X86_64_SYSROOT_MISSING
    "expected sysroot in ${_coop_cross_root}")

set(_coop_cross_gcc_install "${_coop_cross_sysroot}/usr/lib/gcc/x86_64-amazon-linux/11")
_coop_cross_require_directory("${_coop_cross_gcc_install}"
    COOP_CROSS_X86_64_GCC_INSTALL_MISSING "expected GCC 11 installation in ${_coop_cross_sysroot}")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "${_coop_cross_compiler}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_coop_cross_cxx_compiler}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_coop_cross_compiler}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_TARGET x86_64-amazon-linux-gnu CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET x86_64-amazon-linux-gnu CACHE STRING "" FORCE)
set(CMAKE_ASM_COMPILER_TARGET x86_64-amazon-linux-gnu CACHE STRING "" FORCE)
set(CMAKE_LINKER "${_coop_cross_linker}" CACHE FILEPATH "" FORCE)
set(CMAKE_SYSROOT "${_coop_cross_sysroot}" CACHE PATH "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

function(_coop_cross_quote_flag_path _output _path)
    string(REPLACE "\\" "\\\\" _escaped_path "${_path}")
    string(REPLACE "\"" "\\\"" _escaped_path "${_escaped_path}")
    string(REPLACE "$" "\\$" _escaped_path "${_escaped_path}")
    string(REPLACE "`" "\\`" _escaped_path "${_escaped_path}")
    set(${_output} "\"${_escaped_path}\"" PARENT_SCOPE)
endfunction()

_coop_cross_quote_flag_path(_coop_cross_sysroot_arg "${_coop_cross_sysroot}")
_coop_cross_quote_flag_path(_coop_cross_gcc_install_arg "${_coop_cross_gcc_install}")
_coop_cross_quote_flag_path(_coop_cross_resource_dir_arg "${_coop_cross_resource_dir}")
_coop_cross_quote_flag_path(_coop_cross_linker_arg "${_coop_cross_linker}")
_coop_cross_quote_flag_path(_coop_cross_cxx_include_arg "${_coop_cross_sysroot}/usr/include/c++/11")
_coop_cross_quote_flag_path(_coop_cross_cxx_target_include_arg
    "${_coop_cross_sysroot}/usr/include/c++/11/x86_64-amazon-linux")
_coop_cross_quote_flag_path(_coop_cross_cxx_backward_include_arg
    "${_coop_cross_sysroot}/usr/include/c++/11/backward")
_coop_cross_quote_flag_path(_coop_cross_usr_lib64_arg "${_coop_cross_sysroot}/usr/lib64")
_coop_cross_quote_flag_path(_coop_cross_lib64_arg "${_coop_cross_sysroot}/lib64")

string(CONCAT _coop_cross_common_flags
    "--no-default-config --target=x86_64-amazon-linux-gnu --sysroot=${_coop_cross_sysroot_arg} "
    "--gcc-install-dir=${_coop_cross_gcc_install_arg} -resource-dir=${_coop_cross_resource_dir_arg} "
    "-march=x86-64-v2 -msse4.2")
string(CONCAT _coop_cross_cxx_include_flags
    "-stdlib=libstdc++ -isystem ${_coop_cross_cxx_include_arg} "
    "-isystem ${_coop_cross_cxx_target_include_arg} "
    "-isystem ${_coop_cross_cxx_backward_include_arg}")
string(CONCAT _coop_cross_link_flags
    "--no-default-config --target=x86_64-amazon-linux-gnu --sysroot=${_coop_cross_sysroot_arg} "
    "--gcc-install-dir=${_coop_cross_gcc_install_arg} -resource-dir=${_coop_cross_resource_dir_arg} "
    "--ld-path=${_coop_cross_linker_arg} -stdlib=libstdc++ "
    "-L${_coop_cross_gcc_install_arg} -L${_coop_cross_usr_lib64_arg} "
    "-L${_coop_cross_lib64_arg}")

set(CMAKE_C_FLAGS_INIT "${_coop_cross_common_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_coop_cross_common_flags} ${_coop_cross_cxx_include_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_coop_cross_common_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_coop_cross_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_coop_cross_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_coop_cross_link_flags}")

set(CMAKE_FIND_ROOT_PATH "${_coop_cross_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
