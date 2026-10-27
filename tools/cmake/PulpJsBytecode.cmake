# PulpJsBytecode.cmake — build-time QuickJS bytecode for scripted UIs.
#
# pulp_add_js_bytecode(<target>
#     SOURCES <script.js>...
#     [NAMESPACE <ns>]                # default: <target>
#     [WIDGET_BRIDGE_SCRIPT])         # compile as WidgetBridge::load_script() evaluates
#
# Compiles each script to a bytecode blob during the build and embeds it in a
# static library <target>. The generated header <target>_data.hpp declares,
# per script `runtime.js`:
#
#     namespace <ns> {
#         extern const unsigned char runtime_js_pjsbc[];
#         extern const std::size_t   runtime_js_pjsbc_size;
#     }
#
# Hand each blob to pulp::view::register_embedded_script_bytecode() before the
# editor opens (e.g. from the processor's constructor). A blob is used only for
# the exact source it was compiled from and only by the engine build that
# compiled it; otherwise the script compiles from source as before.
#
# The generator is built from the same Pulp libraries the product links, so
# its output always matches the product's engine. When cross-compiling, the
# generator cannot run on the build machine: the library is still created but
# holds placeholder blobs, which register_embedded_script_bytecode() rejects.

include_guard(GLOBAL)

function(_pulp_js_bytecode_generator out_var)
    if(TARGET pulp-js-bytecode)
        set(${out_var} pulp-js-bytecode PARENT_SCOPE)
        return()
    endif()
    set(_pulp_js_bytecode_source
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/pulp_js_bytecode_main.cpp")
    if(NOT EXISTS "${_pulp_js_bytecode_source}")
        message(FATAL_ERROR
            "pulp_add_js_bytecode: generator source not found at "
            "${_pulp_js_bytecode_source}. Re-install Pulp: the SDK ships it under "
            "lib/cmake/Pulp/scripts/.")
    endif()
    if(TARGET Pulp::view)
        set(_pulp_js_bytecode_view Pulp::view)
    elseif(TARGET pulp-view)
        set(_pulp_js_bytecode_view pulp-view)
    else()
        message(FATAL_ERROR "pulp_add_js_bytecode: the Pulp view library is not available")
    endif()
    add_executable(pulp-js-bytecode "${_pulp_js_bytecode_source}")
    target_link_libraries(pulp-js-bytecode PRIVATE ${_pulp_js_bytecode_view})
    target_compile_features(pulp-js-bytecode PRIVATE cxx_std_20)
    set(${out_var} pulp-js-bytecode PARENT_SCOPE)
endfunction()

function(pulp_add_js_bytecode target)
    cmake_parse_arguments(JSBC "WIDGET_BRIDGE_SCRIPT" "NAMESPACE" "SOURCES" ${ARGN})
    if(NOT JSBC_SOURCES)
        message(FATAL_ERROR "pulp_add_js_bytecode(${target}): SOURCES is required.")
    endif()
    if(NOT JSBC_NAMESPACE)
        set(JSBC_NAMESPACE "${target}")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_encoder "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py")
    if(NOT EXISTS "${_encoder}")
        message(FATAL_ERROR "pulp_add_js_bytecode: encode_binary_data.py not found at ${_encoder}")
    endif()

    set(_generator "")
    if(NOT CMAKE_CROSSCOMPILING)
        _pulp_js_bytecode_generator(_generator)
    else()
        message(STATUS "pulp_add_js_bytecode(${target}): cross-compiling; scripts "
                       "will compile from source at run time")
    endif()

    set(_flags "")
    if(JSBC_WIDGET_BRIDGE_SCRIPT)
        set(_flags --widget-bridge-script)
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_pjsbc")
    file(MAKE_DIRECTORY "${_out_dir}")
    set(_hpp "#pragma once\n#include <cstddef>\n\nnamespace ${JSBC_NAMESPACE} {\n\n")
    set(_cpps "")
    foreach(_script IN LISTS JSBC_SOURCES)
        get_filename_component(_abs "${_script}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(_name "${_script}" NAME)
        string(MAKE_C_IDENTIFIER "${_name}_pjsbc" _var)
        set(_blob "${_out_dir}/${_name}.pjsbc")
        set(_cpp "${_out_dir}/${_var}.cpp")
        if(_generator)
            add_custom_command(
                OUTPUT "${_blob}"
                COMMAND $<TARGET_FILE:${_generator}> ${_flags} "${_abs}" "${_blob}"
                DEPENDS "${_abs}" ${_generator}
                COMMENT "Compiling ${_name} to QuickJS bytecode"
                VERBATIM)
        else()
            # One byte, not empty: a zero-length array does not compile everywhere.
            file(WRITE "${_out_dir}/unavailable.pjsbc" "-")
            add_custom_command(
                OUTPUT "${_blob}"
                COMMAND "${CMAKE_COMMAND}" -E copy "${_out_dir}/unavailable.pjsbc" "${_blob}"
                DEPENDS "${_abs}"
                VERBATIM)
        endif()
        add_custom_command(
            OUTPUT "${_cpp}"
            COMMAND "${Python3_EXECUTABLE}" "${_encoder}"
                    --src "${_blob}" --out "${_cpp}"
                    --namespace "${JSBC_NAMESPACE}" --var "${_var}"
            DEPENDS "${_blob}" "${_encoder}"
            COMMENT "Embedding ${_name} bytecode into ${target}"
            VERBATIM)
        list(APPEND _cpps "${_cpp}")
        string(APPEND _hpp "extern const unsigned char ${_var}[];\n")
        string(APPEND _hpp "extern const std::size_t ${_var}_size;\n\n")
    endforeach()
    string(APPEND _hpp "}  // namespace ${JSBC_NAMESPACE}\n")

    set(_hpp_path "${CMAKE_CURRENT_BINARY_DIR}/${target}_data.hpp")
    set(_existing "")
    if(EXISTS "${_hpp_path}")
        file(READ "${_hpp_path}" _existing)
    endif()
    if(NOT "${_existing}" STREQUAL "${_hpp}")
        file(WRITE "${_hpp_path}" "${_hpp}")
    endif()

    add_library(${target} STATIC ${_cpps})
    target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
endfunction()
