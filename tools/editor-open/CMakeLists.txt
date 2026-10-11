# pulp-editor-open-oop-probe — what a host window shows while an AU editor
# opens out of process (AUHostingService, as Logic hosts AU v2). macOS only.
if(APPLE AND NOT IOS)
    add_executable(pulp-editor-open-oop-probe editor_open_oop_probe.mm)
    set_source_files_properties(editor_open_oop_probe.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_features(pulp-editor-open-oop-probe PRIVATE cxx_std_20)
    target_link_libraries(pulp-editor-open-oop-probe PRIVATE
        "-framework AppKit" "-framework AudioToolbox" "-framework CoreAudioKit"
        "-framework CoreGraphics" "-framework ImageIO" "-framework QuartzCore")
endif()

# pulp-host-lane-probe — load built plug-in bundles into one host process and
# record load, audio with the editor closed/open, and the editor's open /
# resize / close / reopen read-back. Data for a paired host-lane comparison.
if(APPLE AND NOT IOS AND TARGET pulp::host)
    add_executable(pulp-host-lane-probe host_lane_probe.mm)
    set_source_files_properties(host_lane_probe.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_features(pulp-host-lane-probe PRIVATE cxx_std_20)
    target_link_libraries(pulp-host-lane-probe PRIVATE pulp::host
        "-framework AppKit" "-framework AudioToolbox" "-framework CoreGraphics" "-framework ImageIO")
    # The probe must not load a GPU runtime of its own: a host that already
    # holds a dylib with a plug-in's install name makes dyld bind the plug-in
    # to the host's copy, which is the very behavior the probe observes.
    target_link_options(pulp-host-lane-probe PRIVATE "-Wl,-dead_strip_dylibs")
endif()
