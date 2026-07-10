# Quantizer — snap a control voltage to discrete steps. Manual mode only;
# calibrated (musical-scale) mode needs a measured full-scale voltage.

if(PULP_BUILD_TESTS)
    add_executable(brew-quantizer-test test_quantizer.cpp quantizer_view.cpp)
    target_link_libraries(brew-quantizer-test
        PRIVATE pulp::format brew-core brew-ui Catch2::Catch2WithMain)
    target_include_directories(brew-quantizer-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(brew-quantizer-test)
endif()

pulp_add_plugin(BrewQuantizer
    FORMATS         VST3 AU CLAP
    SOURCES         quantizer_view.cpp
    PLUGIN_NAME     "Quantizer"
    BUNDLE_ID       "com.bitchesbrew.quantizer"
    MANUFACTURER    "Bitches Brew"
    VERSION         "0.1.0"
    CATEGORY        Effect
    PLUGIN_CODE     "BbQz"
    MANUFACTURER_CODE "BtBr"
)

if(TARGET BrewQuantizer_Core)
    target_include_directories(BrewQuantizer_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(BrewQuantizer_Core PUBLIC brew-core brew-ui)
endif()
foreach(fmt VST3 AU CLAP)
    if(TARGET BrewQuantizer_${fmt})
        target_link_libraries(BrewQuantizer_${fmt} PRIVATE brew-core brew-ui)
    endif()
endforeach()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-BrewQuantizer
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Quantizer.clap/Contents/MacOS/Quantizer'); print('OK')\"")
    set_tests_properties(clap-dlopen-BrewQuantizer PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
