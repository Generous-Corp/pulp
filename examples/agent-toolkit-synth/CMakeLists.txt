cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AgentToolkitSynth LANGUAGES C CXX)
    find_package(Pulp CONFIG REQUIRED)
else()
    include("${PULP_ROOT_DIR}/tools/cmake/PulpUtils.cmake")
endif()

# The tool kit's declaration for this vendor id. Every group is bound below,
# so nothing is excluded and every tier is declared.
pulp_control_agent_toolkit_declaration(_agent_toolkit_operations VENDOR com.example)

pulp_add_plugin(AgentToolkitSynth
    FORMATS Standalone
    SOURCES agent_toolkit_synth.cpp
    PLUGIN_NAME "Agent Toolkit Synth"
    BUNDLE_ID "com.example.agent-toolkit-synth"
    MANUFACTURER "Pulp Examples"
    VERSION "1.0.0"
    CATEGORY Instrument
    ACCEPTS_MIDI
    NO_EDITOR
    PLUGIN_CODE "AgTk"
    MANUFACTURER_CODE "PulE"
    PROCESSOR_FACTORY create_agent_toolkit_synth
    CONTROL_PROFILE developer-local
    CONTROL_CAPABILITIES
        dev.pulp.instance/read@1
        dev.pulp.session/control@1
        dev.pulp.vendor/read@1
        dev.pulp.vendor/gesture@1
        dev.pulp.vendor/render@1
        dev.pulp.vendor/destructive@1
    CONTROL_VENDOR_OPERATIONS "${_agent_toolkit_operations}"
    CONTROL_VENDOR_SOURCES agent_operations.cpp)
