# PulpFonts.cmake — `pulp_register_font()` for plugin authors who want to
# bundle their own .ttf / .otf into a Pulp plugin and have it resolve through
# `canvas.set_font("My Family", ...)` and `setFontFamily(label, "My Family")`.
#
# Issue: pulp #1150. Before this, `AssetManager::register_font_family` was
# dead plumbing — the family→path map was never consulted by SkFontMgr, so
# every plugin that imported a design with `fontFamily: 'JetBrains Mono'` (or
# any non-Pulp-bundled family) silently fell back to whatever the platform
# font manager picked, or to a typeface-less SkFont with ~zero advance width.
#
# Implementation:
#   1. We embed the font file as a pulp_add_binary_data asset so the bytes
#      are baked into the plugin binary at link time. No filesystem lookup
#      at startup, no path-vs-bundle ambiguity in DAW sandboxes.
#   2. We generate a tiny .cpp that runs `pulp::canvas::register_font(...)`
#      from a `__attribute__((constructor))` / `int dummy = …` static
#      initialiser, so the registration happens before the plugin's own
#      `main` / `prepare` / `create_view` runs. By the time any code asks
#      for the family, it's already in the registry.
#   3. The two pieces are bundled into a STATIC library that the caller
#      links into their plugin / app target. This keeps the registration
#      symbols private to the consumer (they don't leak into the SDK).
#
# Usage:
#   pulp_register_font(my-plugin
#       NAME    fonts/MyBrandDisplay-Regular.ttf
#       FAMILY  "MyBrand Display"   # optional; defaults to the font's name table
#   )
#
#   pulp_register_font(my-plugin
#       NAME    fonts/MyBrandDisplay-Bold.ttf
#       FAMILY  "MyBrand Display"
#   )
#
# Multiple invocations are fine — each generates an independent helper TU
# whose static initialiser registers one face. There is no
# `pulp_unregister_font` and we deliberately don't support late
# (post-startup) registration via this macro: register at link-time, not at
# runtime.

include_guard(GLOBAL)

# Pulled in for `pulp_add_binary_data`. Idempotent thanks to the include_guard.
include("${CMAKE_CURRENT_LIST_DIR}/PulpEmbedData.cmake")

# ── pulp_register_font ──────────────────────────────────────────────────────
# Register a bundled font with `pulp::canvas` so it resolves at runtime.
#
# Arguments:
#   target  — the Pulp plugin/app target. Must already exist (created via
#             pulp_add_plugin / pulp_add_app / add_executable).
#   NAME    — path to the .ttf / .otf, relative to CMAKE_CURRENT_SOURCE_DIR
#             or absolute. Required.
#   FAMILY  — optional family-name override. When omitted, the font's own
#             name table is used (same behaviour as
#             `pulp::canvas::register_font` with an empty `family_override`).
function(pulp_register_font target)
    cmake_parse_arguments(PRF "" "NAME;FAMILY" "" ${ARGN})

    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "pulp_register_font(${target}, …): target '${target}' does not "
            "exist. Create it (pulp_add_plugin / pulp_add_app / "
            "add_executable / add_library) before calling pulp_register_font.")
    endif()

    if(NOT PRF_NAME)
        message(FATAL_ERROR
            "pulp_register_font(${target}, …): NAME <path-to-font> is required.")
    endif()

    get_filename_component(_pulp_font_abs "${PRF_NAME}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_pulp_font_abs}")
        message(FATAL_ERROR
            "pulp_register_font(${target}, …): font file not found: "
            "${PRF_NAME} (resolved to ${_pulp_font_abs}).")
    endif()

    # Each call must produce its own pulp_add_binary_data target. We derive a
    # unique-per-(target,font) name so that registering several fonts on the
    # same plugin doesn't collide. MAKE_C_IDENTIFIER also gives us the
    # symbol name pulp_add_binary_data emits inside the generated namespace.
    get_filename_component(_pulp_font_basename "${PRF_NAME}" NAME)
    string(MAKE_C_IDENTIFIER "${_pulp_font_basename}" _pulp_font_var)
    string(MAKE_C_IDENTIFIER "${target}_${_pulp_font_var}" _pulp_font_slug)

    set(_pulp_font_data_target "pulp_font_data_${_pulp_font_slug}")
    set(_pulp_font_namespace "pulp_font_data_${_pulp_font_slug}")
    set(_pulp_font_register_target "pulp_font_register_${_pulp_font_slug}")
    set(_pulp_font_register_cpp
        "${CMAKE_CURRENT_BINARY_DIR}/${_pulp_font_register_target}.cpp")

    # 1. Embed the .ttf bytes as a static C array.
    pulp_add_binary_data(${_pulp_font_data_target}
        SOURCES "${_pulp_font_abs}"
        NAMESPACE ${_pulp_font_namespace})

    # 2. Generate the registration TU. The static initialiser forces the
    #    helper to run before any plugin code that asks for the family. We
    #    deliberately keep the call inside an anonymous namespace so the
    #    symbol cannot collide across multiple pulp_register_font() calls
    #    that happened to land in the same translation unit (they don't,
    #    but defence in depth is cheap).
    set(_pulp_font_family_literal "")
    if(PRF_FAMILY)
        # Escape backslashes and quotes for a C++ string literal. CMake's
        # configure_file would also work, but we'd need a template file —
        # this keeps the macro self-contained in a single .cmake file.
        string(REPLACE "\\" "\\\\" _pulp_font_family_literal "${PRF_FAMILY}")
        string(REPLACE "\"" "\\\"" _pulp_font_family_literal
            "${_pulp_font_family_literal}")
    endif()

    # Avoid touching the .cpp when nothing changed — keeps incremental
    # rebuilds tight.
    set(_pulp_font_register_cpp_body
"// Auto-generated by pulp_register_font(${target}, NAME ${PRF_NAME}).
// Registers the embedded font bytes with pulp::canvas at static init time
// so the family resolves through canvas.set_font() / setFontFamily().
//
// Do not edit; regenerate via cmake reconfigure (or just re-run the build —
// the .ttf is a DEPENDS of the embed step).

#include <pulp/canvas/bundled_fonts.hpp>
#include <cstddef>

namespace ${_pulp_font_namespace} {
extern const unsigned char ${_pulp_font_var}[];
extern const std::size_t   ${_pulp_font_var}_size;
}  // namespace ${_pulp_font_namespace}

namespace {
struct PulpFontRegistrar_${_pulp_font_slug} {
    PulpFontRegistrar_${_pulp_font_slug}() {
        ::pulp::canvas::register_font(
            ${_pulp_font_namespace}::${_pulp_font_var},
            ${_pulp_font_namespace}::${_pulp_font_var}_size,
            \"${_pulp_font_family_literal}\");
    }
};
// Static initialiser. The trailing `(void)&_instance;` ODR-uses the symbol
// so a stripping linker (LTO + dead-code elimination) cannot drop the
// constructor — without that, plugin builds with `-Wl,--gc-sections` were
// silently dropping the registration on Linux.
const PulpFontRegistrar_${_pulp_font_slug} _instance{};
[[maybe_unused]] auto* const _instance_anchor = &_instance;
}  // namespace
")

    set(_pulp_font_existing "")
    if(EXISTS "${_pulp_font_register_cpp}")
        file(READ "${_pulp_font_register_cpp}" _pulp_font_existing)
    endif()
    if(NOT "${_pulp_font_existing}" STREQUAL "${_pulp_font_register_cpp_body}")
        file(WRITE "${_pulp_font_register_cpp}"
            "${_pulp_font_register_cpp_body}")
    endif()

    # 3. Wrap the generated .cpp + the binary-data lib into a small static
    #    library and link it into the caller's target. We use a static lib
    #    (rather than target_sources(${target} PRIVATE …)) so the helper
    #    survives across nested call paths — pulp_register_font is allowed
    #    on any target, including ones built via add_subdirectory().
    add_library(${_pulp_font_register_target} STATIC
        "${_pulp_font_register_cpp}")
    target_link_libraries(${_pulp_font_register_target} PUBLIC
        pulp::canvas
        ${_pulp_font_data_target})
    set_target_properties(${_pulp_font_register_target} PROPERTIES
        LINKER_LANGUAGE CXX)

    # `--whole-archive` semantics: the static initialiser inside the helper
    # archive will be discarded by the linker if no symbol from the archive
    # is referenced by ${target}. Force the linker to keep every object by
    # marking the helper as `OBJECT`-style on platforms where that matters,
    # and on ELF/Mach-O fall back to wrapping with `LINK_OPTIONS`.
    if(MSVC)
        target_link_libraries(${target} PRIVATE
            ${_pulp_font_register_target})
        target_link_options(${target} PRIVATE
            "/WHOLEARCHIVE:${_pulp_font_register_target}")
    elseif(APPLE)
        target_link_libraries(${target} PRIVATE
            "-Wl,-force_load"
            ${_pulp_font_register_target})
    elseif(UNIX)
        target_link_libraries(${target} PRIVATE
            "-Wl,--whole-archive"
            ${_pulp_font_register_target}
            "-Wl,--no-whole-archive")
    else()
        target_link_libraries(${target} PRIVATE
            ${_pulp_font_register_target})
    endif()

    message(STATUS "Pulp font: ${target} ← ${PRF_NAME}"
        "${PRF_FAMILY}")
endfunction()
