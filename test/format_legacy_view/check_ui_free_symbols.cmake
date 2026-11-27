# PAIRS is a list of "<ui-free binary>|<legacy binary>". The ui-free binary must
# define Processor's reserved create_view slot and not the legacy one; the
# legacy binary, built from the same test with PULP_FORMAT_LEGACY_VIEW=1, is
# the control and must show the opposite, which proves the probe reads the
# symbol that tells the two modes apart.
set(_reserved "Processor33reserved_legacy_view_create_view_Ev")
set(_legacy "Processor11create_viewEv")
foreach(_pair IN LISTS PAIRS)
    string(REPLACE "|" ";" _pair "${_pair}")
    list(GET _pair 0 _ui_free)
    list(GET _pair 1 _legacy_bin)
    foreach(_bin IN ITEMS "${_ui_free}" "${_legacy_bin}")
        if(NOT EXISTS "${_bin}")
            message(FATAL_ERROR "binary not found: ${_bin}")
        endif()
        execute_process(COMMAND "${NM}" "${_bin}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _syms
            ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "nm failed on ${_bin}: ${_err}")
        endif()
        string(FIND "${_syms}" "${_reserved}" _has_reserved)
        string(FIND "${_syms}" "${_legacy}" _has_legacy)
        if(_bin STREQUAL _ui_free)
            if(_has_reserved LESS 0 OR NOT _has_legacy LESS 0)
                message(FATAL_ERROR "${_bin} is not a PULP_FORMAT_LEGACY_VIEW=0 build: "
                    "reserved slot at ${_has_reserved}, legacy create_view at ${_has_legacy}")
            endif()
        elseif(NOT _has_reserved LESS 0 OR _has_legacy LESS 0)
            message(FATAL_ERROR "control ${_bin} does not read as a legacy build: "
                "reserved slot at ${_has_reserved}, legacy create_view at ${_has_legacy}")
        endif()
    endforeach()
    message(STATUS "ui-free ${_ui_free}; legacy control ${_legacy_bin}")
endforeach()
