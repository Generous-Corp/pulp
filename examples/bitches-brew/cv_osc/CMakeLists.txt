# CV To OSC — a pass-through insert that reports its input over the loopback.
# The only plug-in in the suite that opens a socket, and the only one that is off
# by default for that reason.

if(PULP_BUILD_TESTS)
    add_executable(brew-cv-osc-test test_cv_osc.cpp cv_osc_view.cpp)
    target_link_libraries(brew-cv-osc-test
        PRIVATE pulp::format pulp::osc pulp::events brew-core brew-ui
                Catch2::Catch2WithMain)
    target_include_directories(brew-cv-osc-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-cv-osc-test)
endif()

pulp_add_plugin(BrewCvOsc
    FORMATS         VST3 AU CLAP
    SOURCES         cv_osc_view.cpp
    PLUGIN_NAME     "CV To OSC"
    BUNDLE_ID       "com.bitchesbrew.cvosc"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbOs"
    MANUFACTURER_CODE "BtBr"
)

if(TARGET BrewCvOsc_Core)
    target_include_directories(BrewCvOsc_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BrewCvOsc_Core PUBLIC brew-core brew-ui pulp::osc pulp::events)
endif()
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewCvOsc_${fmt})
        target_link_libraries(BrewCvOsc_${fmt} PRIVATE brew-core brew-ui pulp::osc pulp::events)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewCvOsc
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/CV To OSC.clap/Contents/MacOS/CV To OSC'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewCvOsc PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
