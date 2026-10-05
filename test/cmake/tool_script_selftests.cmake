# Self-tests of the Python tools under tools/ (dependency audit, the web-compat
# harness, design-import validation, package tooling, the REAPER smoke
# helpers). Each is self-contained: it reads only the source tree and
# fixtures, so it runs on any configured build.
if(Python3_Interpreter_FOUND)
    # glitch_trace.py: planted click/dropout found at their samples, a clean
    # tone found clean (negative control), and a sample joined to the trace
    # block that rendered it (end to end when trace_processor is installed).
    add_test(NAME audio-glitch-trace-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/audio/test_glitch_trace.py")
    set_tests_properties(audio-glitch-trace-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    # macos_merge_group_bootstrap.sh: the merge-group `macos` verdict. A failed
    # or missing dependency fails closed; a cancelled one cancels the run so the
    # queue re-batches instead of ejecting the PR.
    add_test(NAME macos-merge-group-bootstrap-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/test_macos_merge_group_bootstrap.py")
    set_tests_properties(macos-merge-group-bootstrap-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME deps-audit-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/deps/test_audit.py")
    set_tests_properties(deps-audit-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME deps-audit-extra-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/deps/test_audit_extra.py")
    set_tests_properties(deps-audit-extra-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-adapters-base-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_adapters_base.py")
    set_tests_properties(harness-adapters-base-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-auto-discover-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_auto_discover.py")
    set_tests_properties(harness-auto-discover-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-canvas2d-adapter-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_canvas2d_adapter.py")
    set_tests_properties(harness-canvas2d-adapter-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-canvas2d-adapter-shim-files-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_canvas2d_adapter_shim_files.py")
    set_tests_properties(harness-canvas2d-adapter-shim-files-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-css-adapter-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_css_adapter.py")
    set_tests_properties(harness-css-adapter-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-differential-contract-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_differential_contract.py")
    set_tests_properties(harness-differential-contract-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120 ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}")
    add_test(NAME harness-evidence-check-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_evidence_check.py")
    set_tests_properties(harness-evidence-check-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-html-adapter-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_html_adapter.py")
    set_tests_properties(harness-html-adapter-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-html-adapter-element-events-union-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_html_adapter_element_events_union.py")
    set_tests_properties(harness-html-adapter-element-events-union-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-rn-adapter-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_rn_adapter.py")
    set_tests_properties(harness-rn-adapter-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-status-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_status.py")
    set_tests_properties(harness-status-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-validate-test-ref-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_validate_test_ref.py")
    set_tests_properties(harness-validate-test-ref-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-verifier-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_verifier.py")
    set_tests_properties(harness-verifier-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-verify-report-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_verify_report.py")
    set_tests_properties(harness-verify-report-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME harness-yoga-adapter-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/harness/tests/test_yoga_adapter.py")
    set_tests_properties(harness-yoga-adapter-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME import-validation-diff-against-reference-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import-validation/test_diff_against_reference.py")
    set_tests_properties(import-validation-diff-against-reference-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME import-validation-diff-against-reference-regions-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import-validation/test_diff_against_reference_regions.py")
    set_tests_properties(import-validation-diff-against-reference-regions-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME import-validation-score-native-panel-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import-validation/test_score_native_panel.py")
    set_tests_properties(import-validation-score-native-panel-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME import-validation-source-contract-schema-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import-validation/test_source_contract_schema.py")
    set_tests_properties(import-validation-source-contract-schema-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME import-project-import-ir-schema-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import/test_project_import_ir_schema.py")
    set_tests_properties(import-project-import-ir-schema-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME packages-freshness-check-extra-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/packages/test_freshness_check_extra.py")
    set_tests_properties(packages-freshness-check-extra-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME packages-package-validation-tools-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/packages/test_package_validation_tools.py")
    set_tests_properties(packages-package-validation-tools-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME daw-smoke-reaper-smoke-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/testing/daw-smoke/test_reaper_smoke.py")
    set_tests_properties(daw-smoke-reaper-smoke-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME daw-smoke-sample-region-native-reaper-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/testing/daw-smoke/test_sample_region_native_reaper.py")
    set_tests_properties(daw-smoke-sample-region-native-reaper-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
    add_test(NAME daw-smoke-sample-region-native-smoke-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/testing/daw-smoke/test_sample_region_native_smoke.py")
    set_tests_properties(daw-smoke-sample-region-native-smoke-selftest PROPERTIES LABELS "tools;selftest" TIMEOUT 120)
endif()
