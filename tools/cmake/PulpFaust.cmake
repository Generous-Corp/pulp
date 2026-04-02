# PulpFaust.cmake — FAUST offline codegen integration
#
# Provides:
#   pulp_faust_generate()  — Generate C++ from a .dsp file (requires faust)
#   pulp_add_faust_test()  — Add a test target for a FAUST-generated processor
#
# FAUST is optional: pre-generated C++ files are checked in so builds work
# without a FAUST installation. These functions are for regenerating when
# the .dsp source changes.

find_program(FAUST_COMPILER faust)

# pulp_faust_generate(<output_hpp> <input_dsp> <class_name>)
#
# Generates a C++ header from a FAUST .dsp file using offline codegen.
# The generated file is written to the source tree (not build tree) so it
# can be committed and builds don't require faust.
#
# Example:
#   pulp_faust_generate(
#       ${CMAKE_CURRENT_SOURCE_DIR}/generated_gain.hpp
#       ${CMAKE_CURRENT_SOURCE_DIR}/gain.dsp
#       FaustGainDsp
#   )
function(pulp_faust_generate output_hpp input_dsp class_name)
    if(NOT FAUST_COMPILER)
        message(STATUS "FAUST compiler not found — using pre-generated ${output_hpp}")
        return()
    endif()

    add_custom_command(
        OUTPUT "${output_hpp}"
        COMMAND ${FAUST_COMPILER}
            -lang cpp
            -cn "${class_name}"
            -o "${output_hpp}"
            "${input_dsp}"
        DEPENDS "${input_dsp}"
        COMMENT "FAUST: ${input_dsp} → ${output_hpp}"
        VERBATIM
    )
endfunction()

# pulp_add_faust_test(<target> <test_source> [SOURCES ...])
#
# Convenience wrapper for adding a FAUST example test target.
function(pulp_add_faust_test target test_source)
    cmake_parse_arguments(FTEST "" "" "SOURCES" ${ARGN})

    if(NOT PULP_BUILD_TESTS)
        return()
    endif()

    add_executable(${target} ${test_source} ${FTEST_SOURCES})
    target_link_libraries(${target} PRIVATE pulp::dsl pulp::format Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(${target})
endfunction()

# Convenience target to regenerate all FAUST examples
if(FAUST_COMPILER)
    add_custom_target(faust-regenerate
        COMMENT "Regenerating all FAUST C++ from .dsp sources"
    )
    message(STATUS "FAUST compiler found: ${FAUST_COMPILER}")
else()
    message(STATUS "FAUST compiler not found — using pre-generated C++ files")
endif()
