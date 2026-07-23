set(PULP_SEQUENCE_FORMATS VST3 CLAP)
if(APPLE)
    list(APPEND PULP_SEQUENCE_FORMATS AUv3)
endif()

pulp_add_plugin(PulpSequence
    FORMATS         ${PULP_SEQUENCE_FORMATS}
    SOURCES         sequence_demo_processor.cpp
    PLUGIN_NAME     "Pulp Sequence"
    BUNDLE_ID       "com.pulp.sequence-demo"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PSeq"
    MANUFACTURER_CODE "Pulp"
)
target_link_libraries(PulpSequence_Core PUBLIC pulp::sequence)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpSequence
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Pulp Sequence.clap/Contents/MacOS/Pulp Sequence'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpSequence PROPERTIES
        LABELS "validation;clap;timeline" TIMEOUT 10)
endif()
