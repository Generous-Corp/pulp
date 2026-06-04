# ELYSIUM standalone — imports the committed figma-plugin ELYSIUM .pulp.zip at
# runtime via the native materializer (with the sprite-knob hoist) and renders
# it in a real GPU window. Requires the GPU host (MacGpuWindowHost), so it is
# gated the same way as pulp-design-tool.
if(APPLE AND NOT PULP_IOS AND PULP_ENABLE_GPU)
    if(NOT PULP_HAS_SKIA)
        message(FATAL_ERROR
            "pulp-elysium-standalone requires PULP_HAS_SKIA=TRUE (Skia linked) "
            "for the GPU window host. Build Skia (tools/build-skia.sh) or pass "
            "-DPULP_ENABLE_GPU=OFF to skip this example.")
    endif()

    add_executable(pulp-elysium-standalone MACOSX_BUNDLE main.cpp)

    target_link_libraries(pulp-elysium-standalone PRIVATE
        pulp::view pulp::canvas pulp::state)

    # miniz for reading the .pulp.zip; mirrors the mac-platform-harness.
    target_include_directories(pulp-elysium-standalone PRIVATE
        ${CMAKE_SOURCE_DIR}/external/miniz)

    target_compile_features(pulp-elysium-standalone PRIVATE cxx_std_20)
    target_compile_definitions(pulp-elysium-standalone PRIVATE
        PULP_HAS_SKIA
        PULP_ELYSIUM_DEFAULT_ZIP="${CMAKE_SOURCE_DIR}/planning/fixtures/figma-plugin/real-designs/elysium.pulp.zip")

    set_target_properties(pulp-elysium-standalone PROPERTIES
        OUTPUT_NAME "ElysiumStandalone"
        MACOSX_BUNDLE_BUNDLE_NAME "ELYSIUM (imported native)"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.pulp.elysium-standalone"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0")
endif()
