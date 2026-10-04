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
