add_library(pulp-interchange
    src/census.cpp
    src/export_plan.cpp
    src/loss.cpp
)
add_library(pulp::interchange ALIAS pulp-interchange)

target_compile_features(pulp-interchange PUBLIC cxx_std_20)

target_include_directories(pulp-interchange PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# Format-neutral by construction: this target knows about documents and about
# what formats declare, never about any format's bytes. Readers and writers live
# in their own targets above it. The dependency floor is CI-enforced.
target_link_libraries(pulp-interchange
    PUBLIC
        pulp::timeline
        pulp::timebase
    PRIVATE
        pulp::runtime
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-interchange PRIVATE -fno-exceptions -fno-rtti)
endif()

# Portable conformance runner over the timeline fixture corpus. Free of Catch2
# and of any desktop dependency, so the same binary runs under desktop ctest, on
# the Android emulator via `adb push`, and compiled to WASM; it takes its corpus
# path on argv rather than through a compile-time host path. It is owned here
# rather than by the test suite for two reasons: the test directory is not
# configured on mobile, which is where the binary most needs to exist, and this
# module's CI-enforced dependency floor is what keeps the portability claim
# honest — an outside-floor include added to the runner fails the gate.
#
# This declares the DESKTOP build. The wasm build of the same source is a
# separate Emscripten-only root (wasm/CMakeLists.txt) because the top-level
# project does not configure under that toolchain; see the header there.
add_executable(pulp-fixture-runner tools/fixture_runner_main.cpp)
target_link_libraries(pulp-fixture-runner PRIVATE pulp::timeline pulp::interchange)
