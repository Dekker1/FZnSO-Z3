cmake_minimum_required(VERSION 3.20)
project(fznso-z3 CXX)

# The FZnSO headers live in the enclosing checkout; nothing is vendored, because
# this solver ships beside them.
get_filename_component(FZNSO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Z3, linked in rather than dlopened. An installed one is used if there is one;
# otherwise it is fetched and built here, so a checkout needs nothing beyond a
# compiler and a network. Building Z3 from source takes a while, so an installed
# one is worth having (`brew install z3`).
set(Z3_VERSION "5.1.0" CACHE STRING "Z3 version to fetch when none is installed")
find_package(Z3 ${Z3_VERSION} CONFIG QUIET)
if(NOT Z3_FOUND)
  include(FetchContent)
  set(Z3_BUILD_LIBZ3_SHARED OFF)
  set(Z3_BUILD_TEST_EXECUTABLES OFF)
  set(Z3_BUILD_EXECUTABLE OFF)
  set(Z3_ENABLE_EXAMPLE_TARGETS OFF)
  FetchContent_Declare(z3
    GIT_REPOSITORY https://github.com/Z3Prover/z3.git
    GIT_TAG "z3-${Z3_VERSION}"
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(z3)
endif()

# The solver itself. One target: unlike `fznso-mip`, there is no second backend
# to share a core with, so a separate object library would be scaffolding for a
# reuse that is not coming.
#
# The file's base name is the FZnSO solver name, which prefixes the thirteen
# entry points, so `OUTPUT_NAME` is not cosmetic; `lib/fznso/` keeps it apart
# from the library it wraps, whose leaf name is the same.
add_library(fznso-z3 SHARED
  src/declarations.cpp
  src/translate.cpp
  src/solver.cpp
)
target_include_directories(fznso-z3 PRIVATE
  "${FZNSO_ROOT}/c" "${FZNSO_ROOT}/cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fznso-z3 PRIVATE z3::libz3)
target_compile_options(fznso-z3 PRIVATE -Wall -Wextra)
set_target_properties(fznso-z3 PROPERTIES
  OUTPUT_NAME z3
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib/fznso")

# `z3++.h` is not warning-clean under `-Wall -Wextra`, and it is not ours to fix.
get_target_property(Z3_INCLUDES z3::libz3 INTERFACE_INCLUDE_DIRECTORIES)
if(Z3_INCLUDES)
  target_include_directories(fznso-z3 SYSTEM PRIVATE ${Z3_INCLUDES})
endif()

# The self-test loads a built library the way a consumer does, so it exercises
# the entry-point naming and the export path as well as the behaviour. It also
# reads `share/minizinc/fznso_constraints/` to check the declared list against
# the constraints that have no decomposition, so it needs to know where the
# MiniZinc library lives.
enable_testing()
set(FZNSO_MZN_STDLIB "${FZNSO_ROOT}/minizinc_impl/libminizinc/share/minizinc"
  CACHE PATH "MiniZinc library directory, for the declaration-coverage check")
add_executable(fznso-z3-test src/test/fznso_z3_test.cpp)
target_include_directories(fznso-z3-test PRIVATE "${FZNSO_ROOT}/c" "${FZNSO_ROOT}/cpp")
target_compile_options(fznso-z3-test PRIVATE -Wall -Wextra)
if(NOT WIN32)
  target_link_libraries(fznso-z3-test PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(fznso-z3-test fznso-z3)
add_test(NAME fznso-z3 COMMAND fznso-z3-test $<TARGET_FILE:fznso-z3> "${FZNSO_MZN_STDLIB}")
