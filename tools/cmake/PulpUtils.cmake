# PulpUtils.cmake — Build utilities for Pulp projects
#
# Provides:
#   pulp_add_plugin()  — Create a plugin target with format adapters
#   pulp_add_app()     — Create a standalone application target

# ── pulp_add_plugin ─────────────────────────────────────────────────────────
# Usage:
#   pulp_add_plugin(MySynth
#       FORMATS VST3 AU AUv3 CLAP Standalone
#       PLUGIN_NAME "My Synth"
#       BUNDLE_ID "com.mycompany.mysynth"
#       VERSION "1.0.0"
#   )
function(pulp_add_plugin target)
    cmake_parse_arguments(PLUGIN
        ""
        "PLUGIN_NAME;BUNDLE_ID;VERSION"
        "FORMATS"
        ${ARGN}
    )

    # Phase 1: stub — creates a static library target
    # Phase 5: will generate per-format shared library targets
    add_library(${target} STATIC)

    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(PLUGIN_PLUGIN_NAME)
        target_compile_definitions(${target} PRIVATE
            PULP_PLUGIN_NAME="${PLUGIN_PLUGIN_NAME}"
        )
    endif()

    if(PLUGIN_BUNDLE_ID)
        target_compile_definitions(${target} PRIVATE
            PULP_BUNDLE_ID="${PLUGIN_BUNDLE_ID}"
        )
    endif()

    if(PLUGIN_VERSION)
        target_compile_definitions(${target} PRIVATE
            PULP_PLUGIN_VERSION="${PLUGIN_VERSION}"
        )
    endif()

    message(STATUS "Pulp plugin: ${target} (formats: ${PLUGIN_FORMATS})")
endfunction()

# ── pulp_add_app ────────────────────────────────────────────────────────────
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

    # Phase 1: stub — creates an executable target
    add_executable(${target})

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

# ── pulp_add_binary_data ────────────────────────────────────────────────────
# Embed binary assets as C++ arrays (images, fonts, etc.)
# Phase 3: will implement actual binary data embedding
function(pulp_add_binary_data target)
    cmake_parse_arguments(DATA "" "" "SOURCES" ${ARGN})
    message(STATUS "Pulp binary data: ${target} (${DATA_SOURCES})")
endfunction()
