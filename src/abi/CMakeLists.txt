# This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
# The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

add_library(thespeon_c SHARED
  thespeon_c_abi.cpp)
target_compile_definitions(thespeon_c PRIVATE THESPEON_C_ABI_BUILD)
target_include_directories(thespeon_c
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(thespeon_c PRIVATE thespeon_core)
set_target_properties(thespeon_c PROPERTIES
  OUTPUT_NAME thespeon
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  VERSION 1.0.0
  SOVERSION 1
  PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/thespeon.h")

if(APPLE)
  target_link_options(thespeon_c PRIVATE
    "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/thespeon.exports")
  set_property(TARGET thespeon_c APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/thespeon.exports")
elseif(UNIX)
  target_link_options(thespeon_c PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/thespeon.map")
  set_property(TARGET thespeon_c APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/thespeon.map")
endif()

if(MSVC)
  target_compile_options(thespeon_c PRIVATE /W4 /utf-8)
else()
  target_compile_options(thespeon_c PRIVATE -Wall -Wextra -Wpedantic)
endif()

metagraph_stage_runtime(thespeon_c)
