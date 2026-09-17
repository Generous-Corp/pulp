if(NOT DEFINED PROBE OR PROBE STREQUAL "")
    message(FATAL_ERROR "PROBE is required")
endif()

foreach(mismatch IN ITEMS expected header proc native)
    execute_process(
        COMMAND "${PROBE}" --verify-provider-identity-negative-control "${mismatch}"
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_error)

    if(NOT probe_result EQUAL 1)
        message(FATAL_ERROR
            "${mismatch} mismatch control must exit 1, got ${probe_result}: "
            "${probe_output}${probe_error}")
    endif()

    string(STRIP "${probe_output}" probe_receipt)
    string(JSON probe_status ERROR_VARIABLE json_error GET "${probe_receipt}" status)
    if(json_error OR NOT probe_status STREQUAL "failed")
        message(FATAL_ERROR
            "${mismatch} mismatch control lacks failed JSON status: ${probe_receipt}")
    endif()
    string(JSON probe_reason GET "${probe_receipt}" reason)
    if(NOT probe_reason STREQUAL "provider_identity_mismatch")
        message(FATAL_ERROR
            "${mismatch} mismatch control failed for the wrong reason: ${probe_receipt}")
    endif()
    string(JSON identity_status GET "${probe_receipt}" provider_identity_status)
    string(JSON runtime_status GET "${probe_receipt}" runtime_version_status)
    string(JSON proc_table_install_attempted GET
        "${probe_receipt}" proc_table_install_attempted)
    string(JSON dispatches GET "${probe_receipt}" dispatches)
    if(NOT identity_status STREQUAL "failed" OR proc_table_install_attempted OR dispatches)
        message(FATAL_ERROR
            "${mismatch} mismatch escaped the pre-setter gate: ${probe_receipt}")
    endif()
    if(mismatch STREQUAL "expected")
        if(NOT runtime_status STREQUAL "passed")
            message(FATAL_ERROR
                "expected mismatch mislabeled runtime agreement: ${probe_receipt}")
        endif()
    elseif(NOT runtime_status STREQUAL "failed")
        message(FATAL_ERROR
            "${mismatch} runtime mismatch was not detected: ${probe_receipt}")
    endif()
endforeach()

# Prove the receipt observer itself detects an early proc-table installation.
# The four controls above are meaningful only if this planted ordering failure
# flips the field they require to remain false.
execute_process(
    COMMAND "${PROBE}" --verify-provider-setter-order-negative-control
    RESULT_VARIABLE setter_result
    OUTPUT_VARIABLE setter_output
    ERROR_VARIABLE setter_error)
if(NOT setter_result EQUAL 1)
    message(FATAL_ERROR
        "setter-order control must exit 1, got ${setter_result}: "
        "${setter_output}${setter_error}")
endif()
string(STRIP "${setter_output}" setter_receipt)
string(JSON setter_reason GET "${setter_receipt}" reason)
string(JSON setter_attempted GET "${setter_receipt}" proc_table_install_attempted)
string(JSON setter_dispatches GET "${setter_receipt}" dispatches)
if(NOT setter_reason STREQUAL "provider_identity_mismatch" OR
        NOT setter_attempted OR setter_dispatches)
    message(FATAL_ERROR
        "setter-order observer did not detect planted early installation: ${setter_receipt}")
endif()
