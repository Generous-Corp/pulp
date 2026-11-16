cmake_minimum_required(VERSION 3.24)
project(control_scalar_consumer LANGUAGES CXX)

# Compiles against an installed include tree only (PULP_INCLUDE_DIR); the Pulp
# source tree is never on the include path.
if(NOT PULP_INCLUDE_DIR OR NOT EXISTS "${PULP_INCLUDE_DIR}/pulp/signal/control_math.hpp")
    message(FATAL_ERROR "PULP_INCLUDE_DIR must name an installed include tree")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(_deprecation_error /we4996)
else()
    set(_deprecation_error -Werror=deprecated-declarations)
endif()

# Canonical names: must build with deprecation promoted to an error, which
# also proves the headers' own non-deprecated code never calls a deprecated
# spelling.
# script-inputs: not-gated standalone consumer project that the repository configure never adds
add_executable(control_scalar_canonical canonical.cpp)
target_include_directories(control_scalar_canonical PRIVATE "${PULP_INCLUDE_DIR}")
target_compile_options(control_scalar_canonical PRIVATE ${_deprecation_error})

# Legacy spellings: must still build with default diagnostics.
# script-inputs: not-gated standalone consumer project that the repository configure never adds
add_executable(control_scalar_legacy legacy.cpp)
target_include_directories(control_scalar_legacy PRIVATE "${PULP_INCLUDE_DIR}")

# Negative control: the same legacy source with deprecation promoted to an
# error must fail to build, proving the deprecation reaches consumers.
if(CONTROL_SCALAR_LEGACY_STRICT)
    # script-inputs: not-gated standalone consumer project that the repository configure never adds
    add_executable(control_scalar_legacy_strict legacy.cpp)
    target_include_directories(control_scalar_legacy_strict PRIVATE "${PULP_INCLUDE_DIR}")
    target_compile_options(control_scalar_legacy_strict PRIVATE ${_deprecation_error})
endif()
