# PulpPluck — Karplus-Strong plucked-string synthesizer
# Same Processor runs as AU, VST3, CLAP (native) and WASM (browser)

pulp_add_plugin(PulpPluck
    FORMATS         VST3 AU CLAP
    PLUGIN_NAME     "PulpPluck"
    BUNDLE_ID       "com.pulp.pluck"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PPlk"
    MANUFACTURER_CODE "Pulp"
)

# CLAP dlopen test
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpPluck
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpPluck.clap/Contents/MacOS/PulpPluck'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpPluck PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# auval (AU v2)
if(APPLE AND PULP_HAS_AUSDK)
    add_test(
        NAME auval-PulpPluck
        COMMAND ${CMAKE_COMMAND} -E env
            bash -c "cp -R ${CMAKE_BINARY_DIR}/AU/PulpPluck.component ~/Library/Audio/Plug-Ins/Components/ && auval -v aumu PPlk Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'"
    )
    set_tests_properties(auval-PulpPluck PROPERTIES LABELS "validation;au" TIMEOUT 30)
endif()
