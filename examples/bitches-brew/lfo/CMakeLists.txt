# LFO — tempo-locked modulation source with a quadrature partner.

if(PULP_BUILD_TESTS)
    # lfo_view.cpp defines create_view(); without it the Processor's vtable is
    # incomplete and the test does not link.
    add_executable(brew-lfo-test test_lfo.cpp lfo_view.cpp)
    target_link_libraries(brew-lfo-test
        PRIVATE pulp::format brew-core brew-ui Catch2::Catch2WithMain)
    target_include_directories(brew-lfo-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-lfo-test)
endif()

pulp_add_plugin(BrewLFO
    FORMATS         VST3 AU CLAP
    SOURCES         lfo_view.cpp
    PLUGIN_NAME     "LFO"
    BUNDLE_ID       "com.bitchesbrew.lfo"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbLf"
    MANUFACTURER_CODE "BtBr"
)

if(TARGET BrewLFO_Core)
    target_include_directories(BrewLFO_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BrewLFO_Core PUBLIC brew-core brew-ui)
endif()
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewLFO_${fmt})
        target_link_libraries(BrewLFO_${fmt} PRIVATE brew-core brew-ui)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewLFO
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/LFO.clap/Contents/MacOS/LFO'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewLFO PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
