cmake_minimum_required(VERSION 3.24)

project(metagraph_runner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
  set(METAGRAPH_BUILD_TOOLS_DEFAULT ON)
else()
  set(METAGRAPH_BUILD_TOOLS_DEFAULT OFF)
endif()
option(METAGRAPH_BUILD_TOOLS
  "Build the standalone MetaGraph runner executables"
  ${METAGRAPH_BUILD_TOOLS_DEFAULT})
unset(METAGRAPH_BUILD_TOOLS_DEFAULT)

include(FetchContent)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBPROTOC OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  protobuf
  URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz
  URL_HASH SHA256=22fdaf641b31655d4b2297f9981fa5203b2866f8332d3c6333f6b0107bb320de
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" METAGRAPH_SYSTEM_PROCESSOR)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(NOT METAGRAPH_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "The bundled ONNX Runtime supports Linux x86_64 only")
  endif()
  set(ONNXRUNTIME_ARCHIVE "onnxruntime-linux-x64-1.24.1.tgz")
  set(ONNXRUNTIME_SHA256 "9142552248b735920f9390027e4512a2cacf8946a1ffcbe9071a5c210531026f")
  set(ONNXRUNTIME_LIBRARY "libonnxruntime.so.1.24.1")
  set(ONNXRUNTIME_RUNTIME_LIBRARY "libonnxruntime.so.1")
  set(ONNXRUNTIME_BUILD_RPATH "$ORIGIN")
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    set(METAGRAPH_APPLE_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(METAGRAPH_APPLE_ARCHITECTURE "${METAGRAPH_SYSTEM_PROCESSOR}")
  endif()
  if(NOT METAGRAPH_APPLE_ARCHITECTURE STREQUAL "arm64")
    message(FATAL_ERROR
      "The official ONNX Runtime 1.24.1 macOS archive supports Apple Silicon only. "
      "Use a native arm64 build of CMake (not Rosetta).")
  endif()
  set(ONNXRUNTIME_ARCHIVE "onnxruntime-osx-arm64-1.24.1.tgz")
  set(ONNXRUNTIME_SHA256 "c2969315cd9ce0f5fa04f6b53ff72cb92f87f7dcf38e88cacfa40c8f983fbba9")
  set(ONNXRUNTIME_LIBRARY "libonnxruntime.1.24.1.dylib")
  set(ONNXRUNTIME_RUNTIME_LIBRARY "libonnxruntime.1.24.1.dylib")
  set(ONNXRUNTIME_BUILD_RPATH "@loader_path")
elseif(WIN32)
  if(NOT METAGRAPH_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "The bundled ONNX Runtime supports Windows x64 only")
  endif()
  set(ONNXRUNTIME_ARCHIVE "onnxruntime-win-x64-1.24.1.zip")
  set(ONNXRUNTIME_SHA256 "c72736c16442cad417ee4f45e3e2fbd10778e70a0fc1cb0e86f028d766e525af")
  set(ONNXRUNTIME_LIBRARY "onnxruntime.dll")
  set(ONNXRUNTIME_IMPORT_LIBRARY "onnxruntime.lib")
  set(ONNXRUNTIME_RUNTIME_LIBRARY "onnxruntime.dll")
else()
  message(FATAL_ERROR
    "Supported platforms are Linux x86_64, macOS arm64, and Windows x64")
endif()

FetchContent_Declare(
  onnxruntime
  URL "https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/${ONNXRUNTIME_ARCHIVE}"
  URL_HASH "SHA256=${ONNXRUNTIME_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(protobuf onnxruntime)

set(METAGRAPH_PROFILER "none" CACHE STRING "Profiler backend: none or tracy")
set_property(CACHE METAGRAPH_PROFILER PROPERTY STRINGS none tracy)
if(NOT METAGRAPH_PROFILER MATCHES "^(none|tracy)$")
  message(FATAL_ERROR "Unknown METAGRAPH_PROFILER: ${METAGRAPH_PROFILER}")
endif()
if(METAGRAPH_PROFILER STREQUAL "tracy" AND NOT TARGET Tracy::TracyClient)
  set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
  set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.14.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tracy)
endif()

add_library(onnxruntime SHARED IMPORTED GLOBAL)
if(WIN32)
  set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIBRARY}"
    IMPORTED_IMPLIB "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_IMPORT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")
else()
  set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")
endif()
set_property(TARGET onnxruntime PROPERTY
  METAGRAPH_RUNTIME_LIBRARY_NAME "${ONNXRUNTIME_RUNTIME_LIBRARY}")

add_library(metagraph_runner_impl STATIC
  src/metagraph_runner.cpp
  src/session_cache.cpp
  src/meta_graph.pb.cc)
add_library(metagraph::runner ALIAS metagraph_runner_impl)
target_include_directories(metagraph_runner_impl PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(metagraph_runner_impl PUBLIC
  protobuf::libprotobuf
  onnxruntime)
if(METAGRAPH_PROFILER STREQUAL "tracy")
  target_link_libraries(metagraph_runner_impl PUBLIC Tracy::TracyClient)
  target_compile_definitions(metagraph_runner_impl PRIVATE METAGRAPH_PROFILER_TRACY)
endif()

function(metagraph_stage_runtime target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR
      "metagraph_stage_runtime expected an existing target: ${target}")
  endif()

  get_target_property(runtime_name onnxruntime
    METAGRAPH_RUNTIME_LIBRARY_NAME)
  if(NOT runtime_name)
    message(FATAL_ERROR "The ONNX Runtime filename is not configured")
  endif()

  if(APPLE)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:onnxruntime>"
      "$<TARGET_FILE_DIR:${target}>/${runtime_name}"
    VERBATIM)
endfunction()

if(METAGRAPH_BUILD_TOOLS)
  add_executable(run_lara src/run_lara.cpp)
  target_link_libraries(run_lara PRIVATE metagraph::runner)
  metagraph_stage_runtime(run_lara)

  add_executable(run_phonemizer src/run_phonemizer.cpp)
  target_link_libraries(run_phonemizer PRIVATE metagraph::runner)
  metagraph_stage_runtime(run_phonemizer)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(metagraph_runner_impl PRIVATE -Wall -Wextra -Wpedantic)
  if(METAGRAPH_BUILD_TOOLS)
    target_compile_options(run_lara PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(run_phonemizer PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
