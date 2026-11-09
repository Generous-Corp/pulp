# The adapter's tests, added once Catch2 exists (see the root CMakeLists.txt).

if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-rmlui-native-surface
        ${PROJECT_SOURCE_DIR}/test/test_rmlui_native_surface.mm)
    target_compile_options(pulp-test-rmlui-native-surface PRIVATE -fobjc-arc)
    target_link_libraries(pulp-test-rmlui-native-surface PRIVATE
        pulp::rmlui-native Catch2::Catch2WithMain "-framework AppKit" "-framework Metal")
    include(Catch)
    # One label: catch_discover_tests splits a quoted list into separate
    # property arguments, so "a;b" would set LABELS to "a" alone.
    catch_discover_tests(pulp-test-rmlui-native-surface
        PROPERTIES LABELS rmlui)
endif()

# Vellum's Metal renderer on the surface, checked against Vellum's
# SoftRenderer. The references are written in a separate process (Vellum's
# Core module carries its own RmlUi), joined to the test by a ctest fixture.
# Imported targets are scoped to the directory that found them, so this
# directory finds the package's Core targets (SoftRenderer among them) itself.
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain AND TARGET pulp-rmlui-native-metal)
    find_package(VellumRmlUi CONFIG REQUIRED COMPONENTS Core SoftRenderer)
    set(_pulp_rmlui_refs "${CMAKE_CURRENT_BINARY_DIR}/rmlui-soft-references")
    set(_pulp_rmlui_font "${PROJECT_SOURCE_DIR}/external/fonts/Inter-Regular.ttf")

    add_executable(pulp-rmlui-soft-reference
        ${PROJECT_SOURCE_DIR}/test/rmlui_metal_soft_reference.cpp)
    target_include_directories(pulp-rmlui-soft-reference PRIVATE ${PROJECT_SOURCE_DIR}/test)
    target_compile_features(pulp-rmlui-soft-reference PRIVATE cxx_std_20)
    target_compile_definitions(pulp-rmlui-soft-reference PRIVATE
        PULP_RMLUI_GOLDEN_FONT="${_pulp_rmlui_font}")
    target_link_libraries(pulp-rmlui-soft-reference PRIVATE
        VellumRmlUi::Core VellumRmlUi::SoftRenderer)
    add_test(NAME rmlui-soft-references
        COMMAND pulp-rmlui-soft-reference ${_pulp_rmlui_refs})
    set_tests_properties(rmlui-soft-references PROPERTIES
        FIXTURES_SETUP rmlui_soft_references
        LABELS "native-surface;rmlui")

    add_executable(pulp-test-rmlui-native-metal
        ${PROJECT_SOURCE_DIR}/test/test_rmlui_native_metal.mm)
    target_compile_options(pulp-test-rmlui-native-metal PRIVATE -fobjc-arc)
    target_include_directories(pulp-test-rmlui-native-metal PRIVATE ${PROJECT_SOURCE_DIR}/test)
    target_compile_definitions(pulp-test-rmlui-native-metal PRIVATE
        PULP_RMLUI_GOLDEN_FONT="${_pulp_rmlui_font}"
        PULP_RMLUI_SOFT_REFERENCES="${_pulp_rmlui_refs}")
    target_link_libraries(pulp-test-rmlui-native-metal PRIVATE
        pulp::rmlui-native-metal Catch2::Catch2WithMain "-framework AppKit" "-framework Metal")
    catch_discover_tests(pulp-test-rmlui-native-metal
        PROPERTIES LABELS "native-surface;rmlui" FIXTURES_REQUIRED rmlui_soft_references)
endif()
