# PulpEmbedData.cmake — `pulp_add_binary_data()` extracted into a standalone
# include so it can run *before* the Pulp targets exist (e.g. inside
# core/canvas/CMakeLists.txt for bundled fonts).
#
# `PulpUtils.cmake` itself fails fast unless `pulp::format` already exists,
# because most of its other helpers (pulp_add_plugin / pulp_add_app) wire
# format adapters into a target. `pulp_add_binary_data` has no such
# requirement — it just generates a static C++ array library — so it lives
# in its own file with no preconditions. PulpUtils.cmake `include()`s this
# file as well, so the public surface is unchanged: callers continue to use
# `pulp_add_binary_data(...)` and the function definition is identical.
#
# Implementation notes for binary-data embedding:
#   * The legacy implementation walked a hex-encoded file inside a CMake
#     `while()` loop, building the C-array initializer with repeated
#     `string(APPEND)`. CMake string append on a growing string is O(n²);
#     a 1 MB asset took 10–22 minutes per `cmake -S/-B` reconfigure.
#   * The encoding work is now delegated to
#     `tools/cmake/scripts/encode_binary_data.py` and driven by
#     `add_custom_command(OUTPUT … DEPENDS …)`, so the .cpp is regenerated
#     during `cmake --build` (not configure) and only when the input asset
#     or the encoder script changes. A 1 MB asset encodes in well under a
#     second.
#   * The emitted symbols match the legacy ABI exactly:
#         namespace ${NS} {
#             const unsigned char ${var}[]   = { … };
#             const std::size_t   ${var}_size = N;
#         }
#     so existing consumers do not see a behavior change.

include_guard(GLOBAL)

# ── pulp_add_binary_data ────────────────────────────────────────────────
# Embed binary assets as C++ arrays.
# Usage:
#   pulp_add_binary_data(MyResources
#       SOURCES logo.png preset.json font.ttf
#       NAMESPACE myresources
#   )
# Generates a header with:
#   namespace myresources {
#       extern const unsigned char logo_png[];
#       extern const std::size_t   logo_png_size;
#   }
function(pulp_add_binary_data target)
    cmake_parse_arguments(DATA "" "NAMESPACE" "SOURCES" ${ARGN})

    if(NOT DATA_NAMESPACE)
        set(DATA_NAMESPACE "${target}")
    endif()

    if(NOT DATA_SOURCES)
        message(FATAL_ERROR
            "pulp_add_binary_data(${target}): SOURCES is required.")
    endif()

    # Python is already a hard build dependency for pulp (tools/cli,
    # bindings/python, tools/deps/audit.py). `xxd -i` is faster on Linux/macOS
    # but is shipped via Vim on Windows, so we do not rely on it.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_pulp_encoder
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py")
    if(NOT EXISTS "${_pulp_encoder}")
        message(FATAL_ERROR
            "pulp_add_binary_data: encode_binary_data.py not found at "
            "${_pulp_encoder}.\n"
            "If you're consuming Pulp via find_package(Pulp), the SDK install "
            "should have placed the encoder alongside PulpUtils.cmake under "
            "lib/cmake/Pulp/scripts/. Re-install Pulp or report a packaging "
            "regression (issue-905).")
    endif()

    set(generated_hpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_data.hpp")

    # The header carries declarations only; emitting it at configure time is
    # cheap and keeps `#include "${target}_data.hpp"` resolvable for
    # downstream targets without building anything yet.
    set(hpp_content "#pragma once\n#include <cstddef>\n\nnamespace ${DATA_NAMESPACE} {\n\n")

    set(_generated_cpps "")

    foreach(source_file ${DATA_SOURCES})
        get_filename_component(file_name "${source_file}" NAME)
        string(MAKE_C_IDENTIFIER "${file_name}" var_name)
        get_filename_component(abs_path "${source_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

        if(NOT EXISTS "${abs_path}")
            message(FATAL_ERROR
                "pulp_add_binary_data(${target}): source file not found: "
                "${source_file} (resolved to ${abs_path}).")
        endif()

        set(_per_source_cpp
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_data_${var_name}.cpp")

        add_custom_command(
            OUTPUT  "${_per_source_cpp}"
            COMMAND "${Python3_EXECUTABLE}" "${_pulp_encoder}"
                    --src       "${abs_path}"
                    --out       "${_per_source_cpp}"
                    --namespace "${DATA_NAMESPACE}"
                    --var       "${var_name}"
            DEPENDS "${abs_path}" "${_pulp_encoder}"
            COMMENT "Encoding binary asset ${file_name} into ${target}"
            VERBATIM)

        list(APPEND _generated_cpps "${_per_source_cpp}")

        string(APPEND hpp_content "extern const unsigned char ${var_name}[];\n")
        string(APPEND hpp_content "extern const std::size_t ${var_name}_size;\n\n")
    endforeach()

    string(APPEND hpp_content "}  // namespace ${DATA_NAMESPACE}\n")

    # Avoid touching the header (and triggering needless downstream
    # recompiles) when the declarations haven't changed.
    set(_existing_hpp "")
    if(EXISTS "${generated_hpp}")
        file(READ "${generated_hpp}" _existing_hpp)
    endif()
    if(NOT "${_existing_hpp}" STREQUAL "${hpp_content}")
        file(WRITE "${generated_hpp}" "${hpp_content}")
    endif()

    add_library(${target} STATIC ${_generated_cpps})
    # Wrap the build-tree include in BUILD_INTERFACE so that targets which
    # link this lib privately can be added to install(EXPORT …). CMake
    # rejects exported targets whose INTERFACE_INCLUDE_DIRECTORIES contain
    # raw paths inside the build tree.
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)

    message(STATUS "Pulp binary data: ${target} (${DATA_SOURCES})")
endfunction()
