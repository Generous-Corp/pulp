# Python bindings for Pulp HeadlessHost via pybind11

find_package(Python3 COMPONENTS Interpreter Development QUIET)

if(NOT Python3_FOUND)
    message(STATUS "Pulp: Python3 not found — Python bindings disabled")
    return()
endif()

include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(pybind11)

pybind11_add_module(pulp_python bindings.cpp)

target_link_libraries(pulp_python PRIVATE
    pulp::format
    pulp::audio
    pulp::midi
    pulp::state
    pulp::signal
)

# Install to Python site-packages
install(TARGETS pulp_python
    LIBRARY DESTINATION ${Python3_SITEARCH}
)

set_target_properties(pulp_python PROPERTIES
    OUTPUT_NAME "pulp"
)

if(PULP_BUILD_TESTS)
    add_test(NAME python-bindings-smoke
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/test/test_python_bindings.py"
                "$<TARGET_FILE:pulp_python>")
    set_tests_properties(python-bindings-smoke PROPERTIES
        LABELS "python;bindings"
        TIMEOUT 30)

    # Run the bindings init path inside a normal executable so llvm-cov
    # records bindings.cpp hits even when shared-module profiling is
    # flaky on a given platform/toolchain.
    add_executable(pulp-test-python-bindings-embedded
        ${CMAKE_SOURCE_DIR}/test/test_python_bindings_embedded.cpp
        bindings.cpp)
    target_link_libraries(pulp-test-python-bindings-embedded PRIVATE
        Python3::Python
        pybind11::headers
        pulp::format
        pulp::audio
        pulp::midi
        pulp::state
        pulp::signal)
    add_test(NAME python-bindings-embedded-smoke
        COMMAND pulp-test-python-bindings-embedded)
    set_tests_properties(python-bindings-embedded-smoke PROPERTIES
        LABELS "python;bindings"
        TIMEOUT 30)
endif()
