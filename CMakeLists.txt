cmake_minimum_required(VERSION 3.16)
project(shen-c LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(BDWGC REQUIRED IMPORTED_TARGET bdw-gc)

set(_bdwgc_paths "${BDWGC_INCLUDE_DIRS} ${BDWGC_CFLAGS} ${BDWGC_LIBRARY_DIRS} ${BDWGC_LDFLAGS}")
if(_bdwgc_paths MATCHES "Homebrew" OR _bdwgc_paths MATCHES "/opt/homebrew")
  message(FATAL_ERROR "Homebrew libgc paths are not allowed; use Nix pkg-config bdw-gc")
endif()
message(STATUS "bdw-gc ${BDWGC_VERSION} include=${BDWGC_INCLUDE_DIRS}")

option(SHEN_C_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_compile_options(-fno-optimize-sibling-calls -fsigned-char)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-O3)
endif()

if(SHEN_C_SANITIZE)
  if(APPLE)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

file(GLOB SHEN_C_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/c/*.c")
list(REMOVE_ITEM SHEN_C_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/c/main.c")

add_library(shenc STATIC ${SHEN_C_SOURCES})
# -iquote, not -I: src/c/string.h must not shadow libc <string.h>.
target_compile_options(shenc PUBLIC "-iquote${CMAKE_CURRENT_SOURCE_DIR}/src/c")
target_link_libraries(shenc PUBLIC PkgConfig::BDWGC)
set_target_properties(shenc PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
  OUTPUT_NAME shenc
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

add_executable(kernel-aot-build tools/kernel-aot-build.c)
target_link_libraries(kernel-aot-build PRIVATE shenc)
set_target_properties(kernel-aot-build PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

set(KERNEL_AOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/obj/kernel_aot")
set(KERNEL_AOT_MODULES
  sys writer core reader declarations toplevel macros load
  prolog sequent track t_star yacc types)
set(KERNEL_AOT_SRCS "${KERNEL_AOT_DIR}/install_all.c")
foreach(_mod ${KERNEL_AOT_MODULES})
  list(APPEND KERNEL_AOT_SRCS "${KERNEL_AOT_DIR}/${_mod}.c")
endforeach()
file(GLOB KERNEL_KL CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shen/src/kl/*.kl")
add_custom_command(
  OUTPUT ${KERNEL_AOT_SRCS} "${KERNEL_AOT_DIR}/.stamp"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${KERNEL_AOT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E env SHEN_C_HOME=${CMAKE_CURRENT_SOURCE_DIR}
          $<TARGET_FILE:kernel-aot-build>
          "${CMAKE_CURRENT_SOURCE_DIR}/shen/src/kl" "${KERNEL_AOT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E touch "${KERNEL_AOT_DIR}/.stamp"
  DEPENDS kernel-aot-build ${KERNEL_KL}
  COMMENT "Always-AOT kernel NativeFunctions (install_all)"
)

add_executable(shen-c src/c/main.c ${KERNEL_AOT_SRCS})
target_compile_options(shen-c PRIVATE "-iquote${CMAKE_CURRENT_SOURCE_DIR}/src/c")
target_link_libraries(shen-c PRIVATE shenc)
set_target_properties(shen-c PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
  OUTPUT_NAME shen-c
)

add_executable(test_foundation test/c/test_foundation.c)
target_compile_options(test_foundation PRIVATE "-iquote${CMAKE_CURRENT_SOURCE_DIR}/src/c")
target_link_libraries(test_foundation PRIVATE shenc)
set_target_properties(test_foundation PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

add_executable(test_abi test/c/test_abi.c)
target_link_libraries(test_abi PRIVATE shenc)
set_target_properties(test_abi PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

add_executable(test_emit test/c/test_emit.c)
target_link_libraries(test_emit PRIVATE shenc)
set_target_properties(test_emit PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

add_executable(yggdrasil-build tools/yggdrasil-build.c)
target_link_libraries(yggdrasil-build PRIVATE shenc)
set_target_properties(yggdrasil-build PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

enable_testing()
add_test(NAME foundation COMMAND test_foundation)
add_test(NAME abi COMMAND test_abi)
add_test(NAME emit COMMAND test_emit)
