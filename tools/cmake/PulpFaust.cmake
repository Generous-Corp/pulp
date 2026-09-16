# PulpFaust.cmake — optional external Faust codegen integration
#
# Provides:
#   pulp_faust_generate()  — Generate C++ from a .dsp file (requires faust)
#   pulp_add_faust_test()  — Add a test target for a FAUST-generated processor
#
# Faust is optional. Pulp's checked-in examples use Pulp-owned reference DSP
# implementations; this helper produces a separate developer-selected artifact.

find_program(FAUST_COMPILER faust)

# pulp_faust_generate(<output_hpp> <input_dsp> <class_name>)
#
# Generates a C++ header from a FAUST .dsp file using offline codegen.
# Pulp does not claim this output is equivalent to any checked-in reference DSP.
# Pin the external compiler and verify the artifact before making such a claim.
#
# Example:
#   pulp_faust_generate(
#       ${CMAKE_CURRENT_BINARY_DIR}/my_gain_faust.hpp
#       ${CMAKE_CURRENT_SOURCE_DIR}/gain.dsp
#       FaustGainDsp
#   )
function(pulp_faust_generate output_hpp input_dsp class_name)
    if(NOT FAUST_COMPILER)
        message(STATUS "Faust compiler not found — optional codegen target omitted")
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

    # Give the compatibility target a real dependency for every registered
    # output instead of leaving it as a status-only target.
    # Relative output names are scoped to the caller's binary directory. Hash
    # that resolved path so two subdirectories can both register, for example,
    # `generated.hpp` without collapsing into one aggregate dependency.
    get_filename_component(output_absolute "${output_hpp}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    string(SHA256 output_id "${output_absolute}")
    set(output_target "pulp-faust-output-${output_id}")
    if(NOT TARGET ${output_target})
        add_custom_target(${output_target} DEPENDS "${output_hpp}")
        add_dependencies(faust-regenerate ${output_target})
    endif()
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

# Convenience target for projects that register Faust-generated outputs.
if(FAUST_COMPILER)
    add_custom_target(faust-regenerate
        COMMENT "Generate registered outputs with the external Faust compiler"
    )
    message(STATUS "FAUST compiler found: ${FAUST_COMPILER}")
else()
    message(STATUS "Faust compiler not found — optional codegen helpers remain inactive")
endif()
