# PulpWgslShaders.cmake: `pulp_add_wgsl_shaders()`.
#
# Embeds .wgsl files in a target as `std::string_view` constants and, where
# the build can run Dawn on the host, validates each file at build time.
#
#   pulp_add_wgsl_shaders(my_target
#       FILES shaders/glow.wgsl shaders/blur.wgsl
#       [NAMESPACE my::shaders]     # default: <target>_wgsl (C identifier)
#       [NO_VALIDATE])
#
# For each file it generates `<stem>_wgsl.hpp` in
# ${CMAKE_CURRENT_BINARY_DIR}/pulp_wgsl/<target>/, adds that directory to the
# target's PRIVATE include path, and declares
#
#   namespace my::shaders { inline constexpr std::string_view glow_wgsl{...}; }
#
# Validation compiles each file with Dawn's WGSL front end through the
# `pulp-wgsl-validate` host tool, which creates the shader module on Dawn's
# Null adapter (the same compiler a real device runs, with no GPU needed). A
# compile error fails the build with `file:line:column: error: message`.
# Validation is skipped, with a status message, when the tool does not exist:
# a cross-compiled build (iOS, Android, the browser), a build without Dawn,
# or an installed SDK, which does not ship the tool.
#
# Generation happens at build time (add_custom_command), never at configure
# time, and never writes into the source tree.

include_guard(GLOBAL)

set(_PULP_WGSL_SHADERS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(pulp_add_wgsl_shaders target)
    cmake_parse_arguments(WGSL "NO_VALIDATE" "NAMESPACE" "FILES" ${ARGN})
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pulp_add_wgsl_shaders: '${target}' is not a target")
    endif()
    if(NOT WGSL_FILES)
        message(FATAL_ERROR "pulp_add_wgsl_shaders(${target}): FILES is required")
    endif()
    if(WGSL_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_add_wgsl_shaders(${target}): unknown arguments ${WGSL_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT WGSL_NAMESPACE)
        string(MAKE_C_IDENTIFIER "${target}_wgsl" WGSL_NAMESPACE)
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_embed "${_PULP_WGSL_SHADERS_DIR}/scripts/embed_wgsl.py")
    if(NOT EXISTS "${_embed}")
        message(FATAL_ERROR "pulp_add_wgsl_shaders: ${_embed} is missing")
    endif()

    set(_validate FALSE)
    if(NOT WGSL_NO_VALIDATE AND TARGET pulp-wgsl-validate AND NOT CMAKE_CROSSCOMPILING)
        set(_validate TRUE)
    elseif(NOT WGSL_NO_VALIDATE)
        message(STATUS "pulp_add_wgsl_shaders(${target}): WGSL build-time validation "
                       "unavailable in this build; embedding only")
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp_wgsl/${target}")
    set(_outputs "")
    foreach(_file IN LISTS WGSL_FILES)
        get_filename_component(_abs "${_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR "pulp_add_wgsl_shaders(${target}): ${_file} not found")
        endif()
        get_filename_component(_stem "${_abs}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${_stem}_wgsl" _var)
        set(_header "${_out_dir}/${_var}.hpp")
        add_custom_command(
            OUTPUT "${_header}"
            COMMAND "${Python3_EXECUTABLE}" "${_embed}"
                    --src "${_abs}" --out "${_header}"
                    --namespace "${WGSL_NAMESPACE}" --var "${_var}"
            DEPENDS "${_abs}" "${_embed}"
            COMMENT "Embedding WGSL ${_stem}.wgsl into ${target}"
            VERBATIM)
        list(APPEND _outputs "${_header}")

        if(_validate)
            set(_stamp "${_out_dir}/${_var}.validated")
            add_custom_command(
                OUTPUT "${_stamp}"
                COMMAND pulp-wgsl-validate --stamp "${_stamp}" "${_abs}"
                DEPENDS "${_abs}" pulp-wgsl-validate
                COMMENT "Validating WGSL ${_stem}.wgsl"
                VERBATIM)
            list(APPEND _outputs "${_stamp}")
        endif()
    endforeach()

    target_sources(${target} PRIVATE ${_outputs})
    target_include_directories(${target} PRIVATE "$<BUILD_INTERFACE:${_out_dir}>")
endfunction()
