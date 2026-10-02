# This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
# The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

cmake_minimum_required(VERSION 3.24)

project(libthespeon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(THESPEON_BUILD_C_ABI "Build the thespeon C ABI shared library" ON)
option(THESPEON_BUILD_CLI "Build the thespeon command-line tool" ON)
set(THESPEON_PROFILER "none" CACHE STRING "Profiler backend: none or tracy")
set_property(CACHE THESPEON_PROFILER PROPERTY STRINGS none tracy)
if(NOT THESPEON_PROFILER MATCHES "^(none|tracy)$")
  message(FATAL_ERROR "Unknown THESPEON_PROFILER: ${THESPEON_PROFILER}")
endif()
option(THESPEON_BUILD_TESTS "Build the thespeon unit tests"
  ${PROJECT_IS_TOP_LEVEL})
option(THESPEON_COVERAGE "Instrument core for Clang source-based coverage" OFF)


include(FetchContent)

set(METAGRAPH_PROFILER "${THESPEON_PROFILER}" CACHE STRING "" FORCE)
set(METAGRAPH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(metagraph-cpp)

set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ZLIB
  URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
  URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(ZLIB)
if(NOT TARGET zlibstatic)
  message(FATAL_ERROR "The pinned Zlib build did not define zlibstatic")
endif()
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
set(ZLIB_LIBRARIES ZLIB::ZLIB)
set(ZLIB_LINK_LIBRARY_NAME z)

foreach(libzip_option
    ENABLE_BZIP2 ENABLE_LZMA ENABLE_ZSTD
    ENABLE_COMMONCRYPTO ENABLE_GNUTLS ENABLE_MBEDTLS ENABLE_OPENSSL
    ENABLE_WINDOWS_CRYPTO
    BUILD_TOOLS BUILD_REGRESS BUILD_OSSFUZZ BUILD_EXAMPLES BUILD_DOC
    LIBZIP_DO_INSTALL)
  set(${libzip_option} OFF)
endforeach()
FetchContent_Declare(libzip
  GIT_REPOSITORY https://github.com/nih-at/libzip.git
  GIT_TAG v1.11.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libzip)

add_library(thespeon_core STATIC
  src/core/engine.cpp
  src/core/inference/build_inputs.cpp
  src/core/inference/inference_plan.cpp
  src/core/inference/inference_runtime.cpp
  src/core/inference/prosody_preprocess.cpp
  src/core/inference/stream_buffer.cpp
  src/core/inference/text_preprocess.cpp
  src/core/input/input_builder.cpp
  src/core/input/thespeon_input.cpp
  src/core/module/catalog.cpp
  src/core/module/module.cpp
  src/core/package/importer.cpp
  src/core/package/remover.cpp
  src/core/utils/wav_writer.cpp)
target_include_directories(thespeon_core PUBLIC
  "${PROJECT_SOURCE_DIR}/src")
target_include_directories(thespeon_core PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(thespeon_core
  PUBLIC metagraph::runner
  PRIVATE libzip::zip ZLIB::ZLIB)
if(THESPEON_PROFILER STREQUAL "tracy")
  target_link_libraries(thespeon_core PUBLIC Tracy::TracyClient)
  target_compile_definitions(thespeon_core PUBLIC THESPEON_PROFILER_TRACY)
endif()
if(UNIX)
  target_compile_definitions(thespeon_core PRIVATE _FILE_OFFSET_BITS=64)
endif()

if(THESPEON_BUILD_C_ABI)
  add_subdirectory(src/abi)
endif()

if(MSVC)
  target_compile_options(thespeon_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(thespeon_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(THESPEON_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "THESPEON_COVERAGE requires Clang, AppleClang, or "
      "clang-cl; on Windows configure with -T ClangCL")
  endif()
  if(NOT THESPEON_BUILD_TESTS)
    message(FATAL_ERROR "THESPEON_COVERAGE requires THESPEON_BUILD_TESTS")
  endif()
  # Only core is measured; dependencies stay uninstrumented.
  target_compile_options(thespeon_core PRIVATE
    -fprofile-instr-generate -fcoverage-mapping)
endif()

if(THESPEON_BUILD_CLI)
  add_subdirectory(src/cli)
endif()

if(THESPEON_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
