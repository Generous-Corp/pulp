# Neutral adapter timing facts (format::ProcessTiming) and their projection onto
# the host event timeline. The CLAP traces drive the real adapter, so the target
# matches pulp-format's PULP_CLAP_GUI layout of PulpClapPlugin.
if(TARGET pulp-host AND TARGET clap)
    set(_process_timing_libs pulp::host clap)
    if(TARGET vst3-sdk)
        list(APPEND _process_timing_libs vst3-sdk)
    endif()
    pulp_add_test_suite(pulp-test-process-timing
        SOURCES test_process_timing.cpp test_clap_process_timing.cpp
        LIBRARIES ${_process_timing_libs}
        COMPILE_DEFINITIONS PULP_CLAP_GUI=1)
endif()

# The same traces through the real VST3 adapter. Compiles vst3_plug_view.cpp for
# the GUI symbols PulpVst3Processor::createView() references, like the other
# adapter-driving VST3 tests.
if(PULP_HAS_VST3 AND TARGET pulp-host)
    add_executable(pulp-test-vst3-process-timing
        test_vst3_process_timing.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp)
    target_link_libraries(pulp-test-vst3-process-timing
        PRIVATE pulp::host Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-vst3-process-timing PRIVATE
        PULP_VST3=1
        PULP_VST3_GUI=1)
    catch_discover_tests(pulp-test-vst3-process-timing)
endif()

# The installed-SDK timing consumer, built in-tree from the same source so the
# required gate runs it on every change to the facts or the projection.
if(TARGET pulp-host)
    add_executable(pulp-test-process-timing-consumer
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/process-timing-consumer/main.cpp")
    target_link_libraries(pulp-test-process-timing-consumer PRIVATE pulp::host)
    target_compile_features(pulp-test-process-timing-consumer PRIVATE cxx_std_20)
    add_test(NAME process-timing-public-consumer COMMAND pulp-test-process-timing-consumer)
    set_tests_properties(process-timing-public-consumer PROPERTIES
        LABELS "graph;events;unit" TIMEOUT 10
        PASS_REGULAR_EXPRESSION "process-timing-consumer PASS checks=[0-9]+"
        FAIL_REGULAR_EXPRESSION "process-timing-consumer FAIL")
endif()

# The same facts through the real AU v2 effect, instrument and MIDI-processor
# adapters. Links ausdk and compiles at C++23 like the other AudioUnitSDK
# suites, because the adapter headers include AudioUnitSDK headers.
if(APPLE AND NOT PULP_IOS AND PULP_HAS_AUSDK AND TARGET pulp-host)
    pulp_add_test_suite(pulp-test-au-v2-process-timing
        LIBRARIES pulp::host pulp::format pulp::midi ausdk
                  "-framework AudioToolbox"
                  "-framework CoreFoundation"
                  "-framework CoreMIDI")
    set_target_properties(pulp-test-au-v2-process-timing PROPERTIES CXX_STANDARD 23)
endif()
