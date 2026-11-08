# The adapter's tests, added once Catch2 exists (see the root CMakeLists.txt).

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-rmlui-native-surface
        ${PROJECT_SOURCE_DIR}/test/test_rmlui_native_surface.mm)
    target_compile_options(pulp-test-rmlui-native-surface PRIVATE -fobjc-arc)
    target_link_libraries(pulp-test-rmlui-native-surface PRIVATE
        pulp::rmlui-native Catch2::Catch2WithMain "-framework AppKit" "-framework Metal")
    include(Catch)
    catch_discover_tests(pulp-test-rmlui-native-surface
        PROPERTIES LABELS "native-surface;rmlui")
endif()
