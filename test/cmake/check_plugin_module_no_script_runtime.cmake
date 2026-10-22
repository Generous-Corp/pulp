# Verifies that a plug-in module whose editor runs no script links none of the
# script runtime. Editor glue reaches a scripted session only through virtual
# calls, so a dead-stripped AutoUi plug-in must not contain the JS engine or
# the session code; one direct call from always-linked glue pulls in all of it.
#
# Inputs:
#   BINARY  the module's executable file
#   CONFIG  the active build configuration
#   MODULE  the module target name, for messages
if(CONFIG STREQUAL "Debug")
    message(STATUS "${MODULE}: Debug configuration is not dead-stripped; skipping")
    return()
endif()
find_program(_pulp_nm nm REQUIRED)
execute_process(COMMAND "${_pulp_nm}" "${BINARY}"
    OUTPUT_VARIABLE _pulp_symbols
    RESULT_VARIABLE _pulp_nm_result
    ERROR_VARIABLE _pulp_nm_error)
if(NOT _pulp_nm_result EQUAL 0)
    message(FATAL_ERROR "${MODULE}: nm failed on ${BINARY}: ${_pulp_nm_error}")
endif()
# Control: the module's own glue must be visible, or the scan proves nothing.
string(FIND "${_pulp_symbols}" "ViewBridge" _pulp_control_at)
if(_pulp_control_at EQUAL -1)
    message(FATAL_ERROR "${MODULE}: no ViewBridge symbols in ${BINARY}; the scan cannot see the editor glue")
endif()
foreach(_pulp_marker
        "N4pulp4view12ScriptEngineC"           # ScriptEngine constructor
        "N4pulp4view17ScriptedUiSession4poll"  # session frame pump
        "N4pulp4view19prewarm_scripted_ui"     # prewarm worker
        "N4choc10javascript7quickjs")          # the JS engine itself
    string(FIND "${_pulp_symbols}" "${_pulp_marker}" _pulp_marker_at)
    if(NOT _pulp_marker_at EQUAL -1)
        message(FATAL_ERROR
            "${MODULE} links the script runtime (found ${_pulp_marker}). Some always-linked "
            "editor code calls ScriptedUiSession or the engine directly; route it through "
            "view::ScriptedEditorSession or view::scripted_ui_prewarm_runner(). "
            "Find the caller with -Wl,-why_load.")
    endif()
endforeach()
message(STATUS "${MODULE}: no script runtime linked")
