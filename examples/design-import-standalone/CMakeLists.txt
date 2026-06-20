# Design-import standalone — imports a figma-plugin .pulp.zip at runtime via the
# native materializer (with the sprite-knob hoist) and renders it in a real GPU
# window. Requires the GPU host (MacGpuWindowHost), so it is gated the same way
# as pulp-design-tool.
#
# The DEFAULT fixture is a PRIVATE, dev-only test design that lives in the
# `pulp-planning` submodule (not part of the Pulp distribution) — Pulp ships no
# bundled design here. Point the binary at any `.pulp.zip` to run your own:
#   pulp-design-import-standalone /path/to/your-scene.pulp.zip
# (The private fixture is the ELYSIUM design by Julian Behrens, used only to
# harden the importer — credited in docs/reference/licensing.md, not shipped.)
if(APPLE AND NOT PULP_IOS AND PULP_ENABLE_GPU)
    if(NOT PULP_HAS_SKIA)
        message(FATAL_ERROR
            "pulp-design-import-standalone requires PULP_HAS_SKIA=TRUE (Skia linked) "
            "for the GPU window host. Build Skia (tools/build-skia.sh) or pass "
            "-DPULP_ENABLE_GPU=OFF to skip this example.")
    endif()

    add_executable(pulp-design-import-standalone MACOSX_BUNDLE main.cpp)

    target_link_libraries(pulp-design-import-standalone PRIVATE
        pulp::view pulp::canvas pulp::state)

    # miniz for reading the .pulp.zip; mirrors the mac-platform-harness.
    target_include_directories(pulp-design-import-standalone PRIVATE
        ${CMAKE_SOURCE_DIR}/external/miniz)

    target_compile_features(pulp-design-import-standalone PRIVATE cxx_std_20)
    target_compile_definitions(pulp-design-import-standalone PRIVATE
        PULP_HAS_SKIA
        # Private dev-only test fixture (pulp-planning submodule); absent in the
        # distribution. Pass your own .pulp.zip on the command line otherwise.
        PULP_DESIGN_IMPORT_DEFAULT_ZIP="${CMAKE_SOURCE_DIR}/planning/fixtures/figma-plugin/real-designs/elysium.pulp.zip")

    set_target_properties(pulp-design-import-standalone PROPERTIES
        OUTPUT_NAME "DesignImportStandalone"
        MACOSX_BUNDLE_BUNDLE_NAME "Design Import (standalone)"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.pulp.design-import-standalone"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0")

    # Native-only build hygiene (P5): this standalone uses ONLY the native
    # faithful-vector design-import path (Skia draw_svg + native overlays), so a
    # PULP_ENABLE_JS=OFF build must contain zero JS-engine / view-script symbols.
    # Assert it at build time, mirroring the MacGpuWindowHost nm-check.
    if(NOT PULP_ENABLE_JS)
        add_custom_command(TARGET pulp-design-import-standalone POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:pulp-design-import-standalone>
                -P ${CMAKE_SOURCE_DIR}/tools/cmake/AssertNoJsSymbols.cmake
            VERBATIM
            COMMENT "P5: verifying native-only build has no JS-engine symbols")
    endif()
endif()
