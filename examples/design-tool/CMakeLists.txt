if(APPLE AND NOT PULP_IOS AND PULP_ENABLE_GPU)
    # pulp-internal #70 — pulp-design-tool requires the GPU host. The
    # design viewport pin + aspect-locked uniform scale used to fit
    # runtime-imported JSX into a window are implemented ONLY in
    # MacGpuWindowHost (window_host_mac.mm, gated by PULP_HAS_SKIA).
    # Without Skia linked, WindowHost::create() returns MacWindowHost,
    # where set_design_viewport() and set_fixed_aspect_ratio() are
    # no-op base-class methods. The example still builds and runs but
    # the resize behavior is silently broken: content stays at native
    # size as the window grows, with the dark fill exposed past the
    # design surface. Hard-fail at configure so a missing Skia tarball
    # surfaces immediately instead of as a baffling runtime UX bug.
    # Burned ~3h of session debugging this in 2026-05.
    if(NOT PULP_HAS_SKIA)
        message(FATAL_ERROR
            "pulp-design-tool requires PULP_HAS_SKIA=TRUE (Skia + Dawn linked). "
            "Build with -DSKIA_DIR=<path-to-populated-skia-build> or run "
            "tools/build-skia.sh, or pass -DPULP_ENABLE_GPU=OFF to skip this "
            "example. See planning/2026-05-19-gpu-host-fallback.md.")
    endif()
    add_executable(pulp-design-tool main.cpp)
    # WYSIWYG T6 — pulp::inspect supplies the in-canvas InspectorOverlay
    # (move/resize/reflow manipulation layer) wired via install_inspector_hooks,
    # the same overlay ui-preview uses. pulp::inspect itself requires the
    # GPU/Skia stack, which this branch already guarantees
    # (APPLE AND PULP_ENABLE_GPU AND PULP_HAS_SKIA above).
    target_link_libraries(pulp-design-tool PRIVATE
        pulp::view pulp::canvas pulp::state pulp::inspect)
    target_compile_features(pulp-design-tool PRIVATE cxx_std_20)
    # Propagate PULP_HAS_SKIA so the example can do a belt-and-suspenders
    # runtime check in main() (defense-in-depth — CMake gate could be
    # disabled by mistake; the runtime check is the last line).
    target_compile_definitions(pulp-design-tool PRIVATE PULP_HAS_SKIA)
endif()
