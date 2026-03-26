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
