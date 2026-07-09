# Sync — host-transport clock and run/stop gate.

if(PULP_BUILD_TESTS)
    add_executable(brew-sync-test test_sync.cpp)
    target_link_libraries(brew-sync-test PRIVATE pulp::format brew-core Catch2::Catch2WithMain)
    target_include_directories(brew-sync-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-sync-test)
endif()

# No Info.plist.au: Sync reads no MIDI, so the descriptor yields `aufx`, and the
# generated template picks it up. Deliberate — see the plan's classification note.
pulp_add_plugin(BrewSync
    FORMATS         VST3 AU CLAP
    PLUGIN_NAME     "Sync"
    BUNDLE_ID       "com.bitchesbrew.sync"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbSy"
    MANUFACTURER_CODE "BtBr"
)
# The entry translation units include sync_processor.hpp, which pulls in
# brew-core's headers. No SOURCES were given, so there is no BrewSync_Core to
# hang them off: link the header-only target into each format module directly.
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewSync_${fmt})
        target_link_libraries(BrewSync_${fmt} PRIVATE brew-core)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewSync
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Sync.clap/Contents/MacOS/Sync'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewSync PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
