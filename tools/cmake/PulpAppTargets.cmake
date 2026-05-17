# PulpAppTargets.cmake — standalone audio app + generic app targets.
#
# Extracted from PulpUtils.cmake in the 2026-05 Phase 3 (R2-9) refactor.
# Both `_pulp_add_standalone` (the standalone audio-host shell used by
# `pulp_add_plugin(... FORMATS standalone)`) and `pulp_add_app`
# (generic non-plugin Pulp apps) live here.
function(_pulp_add_standalone target name bundle_id version)
    if(NOT _PULP_STANDALONE_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): Standalone requested but Pulp::standalone is unavailable")
    endif()

    # Find standalone entry (convention: main.cpp in source dir)
    set(standalone_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
        set(standalone_entry "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    endif()

    if(APPLE)
        add_executable(${target}_Standalone MACOSX_BUNDLE
            ${PULP_${target}_CORE_OBJECTS}
            ${standalone_entry}
        )
    else()
        add_executable(${target}_Standalone
            ${PULP_${target}_CORE_OBJECTS}
            ${standalone_entry}
        )
    endif()
    target_link_libraries(${target}_Standalone PRIVATE
        ${target}_Core
        ${_PULP_STANDALONE_TARGET}
    )
    _pulp_apply_ui_script_definition(${target}_Standalone "${PULP_${target}_UI_SCRIPT}")
    target_include_directories(${target}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target}_Standalone PROPERTIES
        OUTPUT_NAME "${name}"
    )
    if(APPLE)
        set_target_properties(${target}_Standalone PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${version}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${version}"
        )
    endif()
    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_Standalone)
    endif()
    # Linux+GNU-ld link-order fix: libskia.a → fontconfig. Same helper
    # used for pulp-cli (#1986) and pulp-import-design (#2018). Standalone
    # transitively pulls in pulp::view → pulp::canvas → libskia.a, which
    # references Fc* symbols. Re-mention fontconfig AFTER the archive so
    # the linker resolves them. No-op on macOS/Windows/Android.
    #
    # CMAKE_CURRENT_FUNCTION_LIST_DIR (NOT CMAKE_CURRENT_LIST_DIR or
    # CMAKE_SOURCE_DIR) — CMake's CMAKE_CURRENT_LIST_DIR inside a function
    # body resolves to the *caller's* dir, not where the function was
    # defined. CMAKE_CURRENT_FUNCTION_LIST_DIR (CMake 3.17+; Pulp pins
    # 3.24) is the one that consistently resolves to PulpUtils.cmake's
    # own dir — i.e., the in-tree tools/cmake/ during a source build,
    # and ~/.pulp/sdk/<ver>/lib/cmake/Pulp/ after install. The sibling
    # PulpLinkFontconfig.cmake ships alongside via root CMakeLists.txt's
    # install(FILES) block. Pulp #2087 piggyback — pre-fix this broke
    # every Linux/Android consumer since CMAKE_SOURCE_DIR resolved to
    # the consumer's source tree.
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpLinkFontconfig.cmake")
    pulp_link_fontconfig_after_skia(${target}_Standalone)
endfunction()

# ── pulp_add_app ────────────────────────────────────────────────────────
# Usage:
#   pulp_add_app(MyApp
#       APP_NAME "My App"
#       BUNDLE_ID "com.mycompany.myapp"
#       VERSION "1.0.0"
#   )
function(pulp_add_app target)
    cmake_parse_arguments(APP
        ""
        "APP_NAME;BUNDLE_ID;VERSION"
        ""
        ${ARGN}
    )

    if(APPLE)
        add_executable(${target} MACOSX_BUNDLE)
    else()
        add_executable(${target})
    endif()

    if(APP_APP_NAME)
        target_compile_definitions(${target} PRIVATE
            PULP_APP_NAME="${APP_APP_NAME}"
        )
    endif()

    if(APP_BUNDLE_ID)
        target_compile_definitions(${target} PRIVATE
            PULP_BUNDLE_ID="${APP_BUNDLE_ID}"
        )
    endif()

    message(STATUS "Pulp app: ${target}")
endfunction()

# pulp_add_binary_data() lives in PulpEmbedData.cmake so callers (e.g.
# core/canvas — bundled-font registration, #932) can include it before the
# Pulp targets exist. The function definition is unchanged; this PulpUtils
# include() preserves the existing public surface of `include(PulpUtils)`.
include("${CMAKE_CURRENT_LIST_DIR}/PulpEmbedData.cmake")

# pulp_register_font() — public font-registration macro for plugin authors
# (pulp #1150). Lives in its own file so SDK consumers who only want
# pulp_add_binary_data don't pay for the extra includes/parsing, and so the
# install layout can ship the font macro alongside the rest of the Pulp
# CMake helpers.
include("${CMAKE_CURRENT_LIST_DIR}/PulpFonts.cmake")
