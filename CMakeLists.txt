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
add_library(shenrc STATIC src/rc/rc.c src/rc/primitives.c src/rc/eval.c src/rc/read.c src/rc/emit.c src/rc/native_compile.c)
target_include_directories(shenrc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/rc")
target_compile_options(shenrc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(shenrc PUBLIC m ${CMAKE_DL_LIBS})
target_include_directories(shenrc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(APPLE)
  target_link_options(shenrc INTERFACE -Wl,-export_dynamic)
else()
  target_link_options(shenrc INTERFACE -rdynamic)
endif()
if(SHEN_RC_SANITIZE)
  target_compile_options(shenrc PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  target_link_options(shenrc PUBLIC -fsanitize=address,undefined)
endif()
if(SHEN_RC_ACYCLIC_ONLY)
  target_compile_definitions(shenrc PUBLIC SHEN_RC_ACYCLIC_ONLY)
  set(RC_TEST_NAMES acyclic)
else()
  set(RC_TEST_NAMES cycles primitives eval env bool_tail apply_fast object_cache immediates collection reuse acyclic symbols primitive_fast equal canonical_primitives canonical_numeric canonical_hash canonical_cons generic_builtin_tag trial_work_union callee identity_tail list_length eval_plan rooted_call condition_borrow)
endif()
enable_testing()
foreach(_test IN LISTS RC_TEST_NAMES)
  add_executable(test_rc_${_test} test/rc/test_${_test}.c)
  target_link_libraries(test_rc_${_test} PRIVATE shenrc)
  add_test(NAME rc_${_test} COMMAND test_rc_${_test})
  set_tests_properties(rc_${_test} PROPERTIES ENVIRONMENT "SHEN_RC_EXECUTION=interpret")
endforeach()
add_executable(test_rc_native_compile test/rc/test_native_compile.c)
target_link_libraries(test_rc_native_compile PRIVATE shenrc)
add_test(NAME rc_native_compile COMMAND test_rc_native_compile)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SHEN_RELEASE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/shen/releases/shenoskernel-43.1.json" CACHE FILEPATH "Pinned kernel/suite release")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SHEN_RELEASE_MANIFEST}" scripts/release_manifest.py)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_manifest.py"
  --manifest "${SHEN_RELEASE_MANIFEST}" --kernel-inputs
  OUTPUT_VARIABLE RC_KERNEL_INPUTS_TEXT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(RC_KERNEL_INPUTS UNIX_COMMAND "${RC_KERNEL_INPUTS_TEXT}")
list(TRANSFORM RC_KERNEL_INPUTS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_manifest.py"
  --manifest "${SHEN_RELEASE_MANIFEST}" --header "${CMAKE_CURRENT_BINARY_DIR}/release.h"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(rc-build tools/rc-build.c)
target_include_directories(rc-build PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(rc-build PRIVATE shenrc)
set(RC_KERNEL_C "${CMAKE_CURRENT_BINARY_DIR}/rc-kernel.c")
add_custom_command(OUTPUT "${RC_KERNEL_C}"
  COMMAND $<TARGET_FILE:rc-build> --emit-kernel
          "${CMAKE_CURRENT_SOURCE_DIR}" "${RC_KERNEL_C}"
  DEPENDS rc-build ${RC_KERNEL_INPUTS}
  COMMENT "Compiling the complete Shen kernel with the Boehm-free compiler"
  VERBATIM)
# Optional native tier (SHEN_RC_NATIVE): embedded runtime headers and build
# flags, the emitter, and runtime symbols exported to dlopen'd modules.
set(RC_TIER_HEADERS src/rc/rc.h src/rc/primitives.h src/rc/eval.h)
set(RC_TIER_HOST_FLAGS "")
if(SHEN_RC_SANITIZE)
  set(RC_TIER_HOST_FLAGS "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
endif()
if(SHEN_RC_ACYCLIC_ONLY)
  string(APPEND RC_TIER_HOST_FLAGS " -DSHEN_RC_ACYCLIC_ONLY")
endif()
set(RC_TIER_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/tier_config.h")
add_custom_command(OUTPUT "${RC_TIER_CONFIG}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-tier-config.py"
          "${RC_TIER_CONFIG}" "${CMAKE_C_COMPILER}" "${RC_TIER_HOST_FLAGS}" ${RC_TIER_HEADERS}
          -- src/rc/emit.c src/rc/emit.h src/rc/rc.c src/rc/primitives.c src/rc/eval.c src/rc/native_compile.c
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS scripts/embed-tier-config.py ${RC_TIER_HEADERS} src/rc/emit.c src/rc/emit.h src/rc/rc.c src/rc/primitives.c src/rc/eval.c src/rc/native_compile.c
  VERBATIM)
target_sources(shenrc PRIVATE "${RC_TIER_CONFIG}")
add_executable(shen-rc src/rc/main.c src/rc/native_tier.c "${RC_KERNEL_C}" "${RC_TIER_CONFIG}")
target_include_directories(shen-rc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(shen-rc PRIVATE shenrc ${CMAKE_DL_LIBS})
set_target_properties(shen-rc PROPERTIES ENABLE_EXPORTS ON)
if(APPLE)
  # Also keeps LTO from internalizing runtime symbols the kernel never uses.
  target_link_options(shen-rc PRIVATE -Wl,-export_dynamic)
endif()
# Keep the conventional executable name without a second runtime.
add_custom_command(TARGET shen-rc POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:shen-rc>
          "$<TARGET_FILE_DIR:shen-rc>/shen-c"
  VERBATIM)
add_custom_target(shen-c DEPENDS shen-rc)
