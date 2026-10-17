cmake_minimum_required(VERSION 3.19)
project(shen-c LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SHEN_RC_ACYCLIC_ONLY "Omit cycle collection and reject cycle-creating writes" OFF)
option(SHEN_RC_SANITIZE "Enable ASan/UBSan for the RC runtime and tests" OFF)
option(SHEN_RC_LTO "Enable supported Release interprocedural optimization" ON)
if(SHEN_RC_LTO AND NOT SHEN_RC_SANITIZE)
  # Forward toolchain/platform settings into the IPO project's link probe.
  if(POLICY CMP0137)
    cmake_policy(SET CMP0137 NEW)
  endif()
  if(POLICY CMP0138)
    cmake_policy(SET CMP0138 NEW)
  endif()
  list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    CMAKE_OSX_SYSROOT CMAKE_C_COMPILER_AR CMAKE_C_COMPILER_RANLIB)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT SHEN_RC_IPO_SUPPORTED OUTPUT SHEN_RC_IPO_ERROR LANGUAGES C)
  if(SHEN_RC_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    message(STATUS "Shen RC: Release LTO enabled")
  else()
    message(STATUS "Shen RC: Release LTO unavailable: ${SHEN_RC_IPO_ERROR}")
  endif()
endif()
add_library(shenrc STATIC src/rc/rc.c src/rc/primitives.c src/rc/eval.c src/rc/read.c)
target_include_directories(shenrc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/rc")
target_compile_options(shenrc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(shenrc PUBLIC m)
if(SHEN_RC_SANITIZE)
  target_compile_options(shenrc PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  target_link_options(shenrc PUBLIC -fsanitize=address,undefined)
endif()
if(SHEN_RC_ACYCLIC_ONLY)
  target_compile_definitions(shenrc PUBLIC SHEN_RC_ACYCLIC_ONLY)
  set(RC_TEST_NAMES acyclic)
else()
  set(RC_TEST_NAMES cycles primitives eval env bool_tail apply_fast object_cache immediates collection reuse acyclic symbols primitive_fast equal canonical_primitives canonical_numeric canonical_cons generic_builtin_tag trial_work_union callee identity_tail list_length eval_plan rooted_call)
endif()
enable_testing()
foreach(_test IN LISTS RC_TEST_NAMES)
  add_executable(test_rc_${_test} test/rc/test_${_test}.c)
  target_link_libraries(test_rc_${_test} PRIVATE shenrc)
  add_test(NAME rc_${_test} COMMAND test_rc_${_test})
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SHEN_RELEASE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/shen/releases/s43.1.json" CACHE FILEPATH "Pinned kernel/suite release")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHEN_RELEASE_MANIFEST}" scripts/release_manifest.py)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_manifest.py"
  --manifest "${SHEN_RELEASE_MANIFEST}" --kernel-inputs
  OUTPUT_VARIABLE RC_KERNEL_INPUTS_TEXT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(RC_KERNEL_INPUTS UNIX_COMMAND "${RC_KERNEL_INPUTS_TEXT}")
list(TRANSFORM RC_KERNEL_INPUTS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(GET RC_KERNEL_INPUTS 0 RC_KERNEL_FIRST)
get_filename_component(RC_KERNEL_DIRECTORY "${RC_KERNEL_FIRST}" DIRECTORY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_manifest.py"
  --manifest "${SHEN_RELEASE_MANIFEST}" --header "${CMAKE_CURRENT_BINARY_DIR}/release.h"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(rc-build tools/rc-build.c src/rc/emit.c)
target_include_directories(rc-build PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(rc-build PRIVATE shenrc)
set(RC_KERNEL_C "${CMAKE_CURRENT_BINARY_DIR}/rc-kernel.c")
add_custom_command(OUTPUT "${RC_KERNEL_C}"
  COMMAND $<TARGET_FILE:rc-build> --emit-kernel
          "${RC_KERNEL_DIRECTORY}" "${RC_KERNEL_C}"
  DEPENDS rc-build ${RC_KERNEL_INPUTS}
  COMMENT "Compiling the complete Shen kernel with the Boehm-free compiler"
  VERBATIM)
add_executable(shen-rc src/rc/main.c "${RC_KERNEL_C}")
target_link_libraries(shen-rc PRIVATE shenrc)
# Keep the conventional executable name without a second runtime.
add_custom_command(TARGET shen-rc POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:shen-rc>
          "$<TARGET_FILE_DIR:shen-rc>/shen-c"
  VERBATIM)
add_custom_target(shen-c DEPENDS shen-rc)
