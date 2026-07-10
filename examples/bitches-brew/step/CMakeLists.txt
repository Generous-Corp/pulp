# Step LFO — an eight-step pattern derived from the host's position, with a
# deterministic hash standing in for a random generator.

if(PULP_BUILD_TESTS)
    add_executable(brew-step-test test_step.cpp step_view.cpp)
    target_link_libraries(brew-step-test
        PRIVATE pulp::format brew-core brew-ui Catch2::Catch2WithMain)
    target_include_directories(brew-step-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-step-test)
endif()

pulp_add_plugin(BrewStep
    FORMATS         VST3 AU CLAP
    SOURCES         step_view.cpp
    PLUGIN_NAME     "Step LFO"
    BUNDLE_ID       "com.bitchesbrew.step"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbSt"
    MANUFACTURER_CODE "BtBr"
)

if(TARGET BrewStep_Core)
    target_include_directories(BrewStep_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BrewStep_Core PUBLIC brew-core brew-ui)
endif()
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewStep_${fmt})
        target_link_libraries(BrewStep_${fmt} PRIVATE brew-core brew-ui)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewStep
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Step LFO.clap/Contents/MacOS/Step LFO'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewStep PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
