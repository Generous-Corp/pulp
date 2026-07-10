# Function — math on an incoming control voltage.

if(PULP_BUILD_TESTS)
    # function_view.cpp defines create_view(); without it the Processor's vtable
    # is incomplete and the test does not link.
    add_executable(brew-function-test test_function.cpp function_view.cpp)
    target_link_libraries(brew-function-test
        PRIVATE pulp::format brew-core brew-ui Catch2::Catch2WithMain)
    target_include_directories(brew-function-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-function-test)
endif()

pulp_add_plugin(BrewFunction
    FORMATS         VST3 AU CLAP
    SOURCES         function_view.cpp
    PLUGIN_NAME     "Function"
    BUNDLE_ID       "com.bitchesbrew.function"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbFn"
    MANUFACTURER_CODE "BtBr"
)

if(TARGET BrewFunction_Core)
    target_include_directories(BrewFunction_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BrewFunction_Core PUBLIC brew-core brew-ui)
endif()
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewFunction_${fmt})
        target_link_libraries(BrewFunction_${fmt} PRIVATE brew-core brew-ui)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewFunction
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Function.clap/Contents/MacOS/Function'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewFunction PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
