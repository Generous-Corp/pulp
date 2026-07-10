# Trigger — MIDI notes and input voltages become gates, pulses, and envelopes.
#
# ACCEPTS_MIDI, because that is the whole plug-in. It types the AU bundle `aumf`
# (kAudioUnitType_MusicEffect) — the only AU type a host routes MIDI to — and
# au_v2_entry.cpp must use PULP_AU_MIDI_PLUGIN to match.

if(PULP_BUILD_TESTS)
    # trigger_view.cpp defines create_view(); without it the Processor's vtable is
    # incomplete and the test does not link.
    add_executable(brew-trigger-test test_trigger.cpp trigger_view.cpp)
    target_link_libraries(brew-trigger-test
        PRIVATE pulp::format brew-core brew-ui Catch2::Catch2WithMain)
    target_include_directories(brew-trigger-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-trigger-test)
endif()

pulp_add_plugin(BrewTrigger
    FORMATS         VST3 AU CLAP
    SOURCES         trigger_view.cpp
    PLUGIN_NAME     "Trigger"
    BUNDLE_ID       "com.bitchesbrew.trigger"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    ACCEPTS_MIDI
    PLUGIN_CODE     "BbTg"
    MANUFACTURER_CODE "BtBr"
)

if(TARGET BrewTrigger_Core)
    target_include_directories(BrewTrigger_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BrewTrigger_Core PUBLIC brew-core brew-ui)
endif()
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewTrigger_${fmt})
        target_link_libraries(BrewTrigger_${fmt} PRIVATE brew-core brew-ui)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewTrigger
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Trigger.clap/Contents/MacOS/Trigger'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewTrigger PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
