# The audio-display registration entry point is linked into IMAGE and absent
# from NEGATIVE_CONTROL, a binary built without the audio provider. The control
# proves nm and the match can tell the two apart.
foreach(_input NM IMAGE NEGATIVE_CONTROL)
    if(NOT DEFINED ${_input})
        message(FATAL_ERROR "missing input ${_input}")
    endif()
endforeach()

function(has_registration binary OUT_VAR)
    execute_process(COMMAND "${NM}" "${binary}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _symbols ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0 OR _symbols STREQUAL "")
        message(FATAL_ERROR "nm could not read ${binary} (rc=${_rc}): ${_err}")
    endif()
    if(_symbols MATCHES "4pulp4view22register_audio_widgetsEv")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    else()
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    endif()
endfunction()

has_registration("${NEGATIVE_CONTROL}" _control)
if(_control)
    message(FATAL_ERROR "negative control ${NEGATIVE_CONTROL} reports register_audio_widgets")
endif()
has_registration("${IMAGE}" _image)
if(NOT _image)
    message(FATAL_ERROR "register_audio_widgets is not linked into ${IMAGE}")
endif()
message(STATUS "register_audio_widgets linked into ${IMAGE}; absent from the control")
