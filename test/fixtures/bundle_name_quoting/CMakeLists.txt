# A plug-in whose PLUGIN_NAME carries spaces and parentheses. Bundle paths are
# derived from that name, and every POST_BUILD step pulp_add_plugin() attaches
# (PkgInfo, moduleinfo.json, runtime manifest, build info, runtime staging) is
# appended to the link command the generator runs through a shell. A step that
# lets the shell see an unquoted "(" fails the link, so building this fixture at
# all is the first half of the proof; the artifact check below is the second.
if(PULP_IOS OR ANDROID OR EMSCRIPTEN)
    return()
endif()

set(_quoting_formats Standalone)
if(PULP_HAS_VST3)
    list(APPEND _quoting_formats VST3)
endif()
if(PULP_HAS_CLAP)
    list(APPEND _quoting_formats CLAP)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND _quoting_formats AU)
endif()

set(_quoting_name "Pulp Test (dev) Plugin")
pulp_add_plugin(PulpBundleNameQuoting
    FORMATS           ${_quoting_formats}
    PLUGIN_NAME       "${_quoting_name}"
    BUNDLE_ID         "dev.pulp.test.bundle-name-quoting"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PTqn"
    MANUFACTURER_CODE "PlTq"
    SOURCES           tail_edge_probe.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_tail_edge_probe
)

set(_quoting_args
    "-DPLUGIN_NAME=${_quoting_name}"
    "-DEXPECT_PKGINFO=$<BOOL:${APPLE}>"
    "-DSTANDALONE_FILE=$<TARGET_FILE:PulpBundleNameQuoting_Standalone>")
foreach(_fmt VST3 CLAP AU)
    if(TARGET PulpBundleNameQuoting_${_fmt})
        list(APPEND _quoting_args
            "-D${_fmt}_FILE=$<TARGET_FILE:PulpBundleNameQuoting_${_fmt}>")
        if(APPLE)
            list(APPEND _quoting_args
                "-D${_fmt}_BUNDLE=$<TARGET_BUNDLE_DIR:PulpBundleNameQuoting_${_fmt}>")
        endif()
    endif()
endforeach()

add_test(NAME cmake-bundle-name-quoting
    COMMAND "${CMAKE_COMMAND}" ${_quoting_args}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_bundle_name_quoting.cmake")
set_tests_properties(cmake-bundle-name-quoting PROPERTIES
    LABELS "cmake;format;artifact" TIMEOUT 30)

# Real-host proof, advisory: pluginval automates the Freeze toggle, whose tail
# edges queue a VST3 kReloadComponent that must never be delivered from inside
# the host's own deactivation. Labeled `validation`, so it stays off the
# required gate; it runs wherever pluginval is installed.
if(TARGET PulpBundleNameQuoting_VST3)
    find_program(PULP_PLUGINVAL_EXECUTABLE pluginval
        HINTS /Applications/pluginval.app/Contents/MacOS)
    if(PULP_PLUGINVAL_EXECUTABLE)
        add_test(NAME pluginval-bundle-name-quoting-VST3
            COMMAND "${PULP_PLUGINVAL_EXECUTABLE}" --strictness-level 5
                --validate-in-process --timeout-ms 60000
                --validate "$<TARGET_BUNDLE_DIR:PulpBundleNameQuoting_VST3>")
        set_tests_properties(pluginval-bundle-name-quoting-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 300
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()
endif()
