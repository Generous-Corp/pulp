# DC — constant control-voltage source and the suite's held-value guard.

if(PULP_BUILD_TESTS)
    add_executable(brew-dc-test test_dc.cpp)
    target_link_libraries(brew-dc-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(brew-dc-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-dc-test)
endif()

# No Info.plist.au: the generated template picks the AU component type from the
# descriptor (aufx here — DC takes no MIDI), which is what we want.
pulp_add_plugin(BrewDC
    FORMATS         VST3 AU CLAP
    PLUGIN_NAME     "DC"
    BUNDLE_ID       "com.bitchesbrew.dc"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbDc"
    MANUFACTURER_CODE "BtBr"
)

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewDC
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/DC.clap/Contents/MacOS/DC'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewDC PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
