if(APPLE AND NOT PULP_IOS)
    # The test-default window policy (PULP_TEST_OFFSCREEN): windows are ordered
    # in and present, but transparent, click-through and never activating.
    pulp_add_test_suite(pulp-test-offscreen-windows
        SOURCES test_offscreen_windows.mm
        LIBRARIES pulp::view)
    target_link_libraries(pulp-test-offscreen-windows PRIVATE "-framework Cocoa" "-framework CoreGraphics")
    target_link_options(pulp-test-offscreen-windows PRIVATE
        "LINKER:-u,_OBJC_CLASS_$_PulpView")
endif()
