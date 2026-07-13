# Combined-bundle demo — ONE binary per format exposing TWO distinct plugins
# (Bundle Gain + Bundle Width) via pulp_add_plugin_bundle(). Demonstrates the
# multi-plugin packaging surface. See docs/guides/plugin-bundles.md.
#
# v1 covers the internally-enumerating formats (CLAP, VST3); AU bundles need a
# multi-component Info.plist + multi-symbol export and land as a later slice.

set(_bundle_demo_formats "")
if(PULP_HAS_CLAP)
    list(APPEND _bundle_demo_formats CLAP)
endif()
if(PULP_HAS_VST3)
    list(APPEND _bundle_demo_formats VST3)
endif()

if(NOT _bundle_demo_formats)
    message(STATUS "combined-bundle-demo: no bundle-capable format available; skipping")
    return()
endif()

pulp_add_plugin_bundle(CombinedBundleDemo
    BUNDLE_NAME  "CombinedBundleDemo"
    BUNDLE_ID    "com.pulp.bundle-demo"
    VERSION      "1.0.0"
    MANUFACTURER "Pulp"
    FORMATS      ${_bundle_demo_formats}
    # Header-only plugins (bundle_plugins.hpp): the per-format entry TUs
    # instantiate the inline factories, so no SOURCES are required.
)

# Artifact test: dlopen the BUILT .clap and assert the one module entry exposes
# two addressable plugins. Complements test/test_clap_bundle_entry.cpp (macros
# at the API level) by proving the CMake surface produces a working binary.
if(PULP_BUILD_TESTS AND TARGET CombinedBundleDemo_CLAP)
    add_executable(pulp-test-combined-bundle-clap bundle_enum_test.cpp)
    target_link_libraries(pulp-test-combined-bundle-clap PRIVATE clap)
    target_compile_definitions(pulp-test-combined-bundle-clap PRIVATE
        PULP_BUNDLE_CLAP_BINARY="$<TARGET_FILE:CombinedBundleDemo_CLAP>")
    add_dependencies(pulp-test-combined-bundle-clap CombinedBundleDemo_CLAP)
    add_test(NAME combined-bundle-clap-enumeration
             COMMAND pulp-test-combined-bundle-clap)
    set_tests_properties(combined-bundle-clap-enumeration PROPERTIES
        LABELS "validation;clap;bundle" TIMEOUT 30)
endif()
