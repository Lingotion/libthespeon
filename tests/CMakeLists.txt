# This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
# The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(thespeon_tests
  build_inputs_test.cpp
  catalog_test.cpp
  engine_test.cpp
  input_builder_test.cpp
  module_test.cpp
  prosody_preprocess_test.cpp
  stream_buffer_test.cpp
  test_support.cpp
  text_preprocess_test.cpp
  thespeon_input_test.cpp
  unicode_case_test.cpp
  wav_writer_test.cpp)
target_include_directories(thespeon_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(thespeon_tests PRIVATE thespeon_core GTest::gtest_main)

if(MSVC)
  target_compile_options(thespeon_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(thespeon_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

metagraph_stage_runtime(thespeon_tests)

# The CLI tests run the built thespeon binary, so they exist only beside it.
if(TARGET thespeon)
  target_sources(thespeon_tests PRIVATE cli_test.cpp)
  target_compile_definitions(thespeon_tests PRIVATE
    "THESPEON_CLI_PATH=\"$<TARGET_FILE:thespeon>\"")
  add_dependencies(thespeon_tests thespeon)
endif()

include(GoogleTest)
# Discovery runs the binary, which needs the staged ONNX Runtime beside it.
gtest_discover_tests(thespeon_tests DISCOVERY_MODE PRE_TEST)

if(THESPEON_COVERAGE)
  # Unreferenced core objects must still be linked, or the report silently
  # omits every file the tests never reach.
  target_link_libraries(thespeon_tests PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,thespeon_core>")

  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl names the profile runtime as a default library of every
    # instrumented object, but link.exe needs its directory to find it.
    foreach(query /clang:-print-runtime-dir /clang:-print-resource-dir)
      execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${query}
        OUTPUT_VARIABLE clang_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
      file(TO_CMAKE_PATH "${clang_dir}" clang_dir)
      foreach(candidate "${clang_dir}" "${clang_dir}/lib/windows")
        if(IS_DIRECTORY "${candidate}")
          target_link_directories(thespeon_tests PRIVATE "${candidate}")
        endif()
      endforeach()
    endforeach()
  else()
    target_link_options(thespeon_tests PRIVATE -fprofile-instr-generate)
  endif()

  # The LLVM tools sit beside the compiler in Visual Studio's ClangCL
  # toolset, and outside PATH in Xcode.
  get_filename_component(llvm_tool_hints "${CMAKE_CXX_COMPILER}" DIRECTORY)
  if(APPLE)
    execute_process(COMMAND xcrun --find llvm-cov
      OUTPUT_VARIABLE xcrun_llvm_cov OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(xcrun_llvm_cov)
      get_filename_component(xcrun_dir "${xcrun_llvm_cov}" DIRECTORY)
      list(APPEND llvm_tool_hints "${xcrun_dir}")
    endif()
  endif()
  find_program(LLVM_COV_EXECUTABLE llvm-cov HINTS ${llvm_tool_hints} REQUIRED)
  find_program(LLVM_PROFDATA_EXECUTABLE llvm-profdata
    HINTS ${llvm_tool_hints} REQUIRED)

  set(coverage_dir "${CMAKE_BINARY_DIR}/coverage")
  set(coverage_raw "${coverage_dir}/thespeon_tests.profraw")
  set(coverage_data "${coverage_dir}/thespeon_tests.profdata")
  set(coverage_sources "${PROJECT_SOURCE_DIR}/src/core")
  add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${coverage_dir}"
    COMMAND ${CMAKE_COMMAND} -E env "LLVM_PROFILE_FILE=${coverage_raw}"
      "$<TARGET_FILE:thespeon_tests>" --gtest_brief=1
    COMMAND "${LLVM_PROFDATA_EXECUTABLE}" merge -sparse "${coverage_raw}"
      -o "${coverage_data}"
    COMMAND "${LLVM_COV_EXECUTABLE}" show "$<TARGET_FILE:thespeon_tests>"
      "-instr-profile=${coverage_data}" -format=html
      "-output-dir=${coverage_dir}/html" "${coverage_sources}"
    COMMAND "${LLVM_COV_EXECUTABLE}" report "$<TARGET_FILE:thespeon_tests>"
      "-instr-profile=${coverage_data}" "${coverage_sources}"
    COMMAND ${CMAKE_COMMAND} -E echo
      "HTML report: ${coverage_dir}/html/index.html"
    DEPENDS thespeon_tests
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VERBATIM)
endif()
