# PulpGainHeadless: PulpGain without an editor (pulp_add_plugin NO_EDITOR).
# Its VST3, CLAP and AU modules link only the audio side of the format layer,
# so they carry no view, canvas, Skia, Dawn or JavaScript code. The footprint
# test below holds that link-graph boundary and the size it buys.

set(PULP_GAIN_HEADLESS_FORMATS VST3 CLAP)
if(APPLE)
    list(APPEND PULP_GAIN_HEADLESS_FORMATS AU)
endif()

pulp_add_plugin(PulpGainHeadless
    FORMATS         ${PULP_GAIN_HEADLESS_FORMATS}
    NO_EDITOR
    PLUGIN_NAME     "PulpGainHeadless"
    BUNDLE_ID       "com.pulp.gain-headless"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PGhl"
    MANUFACTURER_CODE "Pulp"
)

if(APPLE AND Python3_EXECUTABLE)
    set(_pulp_gain_headless_binaries "")
    foreach(_fmt VST3 CLAP AU)
        if(TARGET PulpGainHeadless_${_fmt})
            list(APPEND _pulp_gain_headless_binaries "$<TARGET_FILE:PulpGainHeadless_${_fmt}>")
        endif()
    endforeach()
    add_test(NAME plugin-footprint-PulpGainHeadless
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/plugin_footprint_check.py"
            --profile no-editor ${_pulp_gain_headless_binaries})
    set_tests_properties(plugin-footprint-PulpGainHeadless PROPERTIES
        LABELS "packaging;footprint"
        TIMEOUT 60
        SKIP_RETURN_CODE 77)
endif()

# CLAP dlopen test
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpGainHeadless
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpGainHeadless.clap/Contents/MacOS/PulpGainHeadless'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpGainHeadless PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# auval (AU v2)
if(APPLE AND PULP_HAS_AUSDK)
    add_test(
        NAME auval-PulpGainHeadless
        COMMAND ${CMAKE_COMMAND} -E env
            bash "${CMAKE_SOURCE_DIR}/tools/ci/run-auval-component.sh"
                "${CMAKE_BINARY_DIR}/AU/PulpGainHeadless.component" aufx PGhl Pulp
    )
    set_tests_properties(auval-PulpGainHeadless PROPERTIES
        LABELS "validation;au"
        TIMEOUT 90
        RESOURCE_LOCK pulp_au_discovery
        SKIP_RETURN_CODE 77)
endif()

# pluginval (VST3)
if(PULP_HAS_VST3)
    _pulp_find_pluginval(PLUGINVAL)
    if(PLUGINVAL)
        add_test(
            NAME pluginval-PulpGainHeadless-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpGainHeadless.vst3 --strictness-level 5 --timeout-ms 10000
        )
        set_tests_properties(pluginval-PulpGainHeadless-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 30)
    endif()
endif()
