# Engine-side reference-clock sync soak (TL-18), no hardware required.
#
# `timeline-sync-hardware-soak` (in timeline_tests.cmake) covers the RIG half and
# correctly SKIPs with 77 until someone captures a trace on physical gear. That
# left the engine half — does Pulp's chase accumulate error — untested by
# anything, because the only path to it went through hardware nobody had wired
# up.
#
# It does not need to. How far two physical crystals drift apart is a property
# of the oscillators, not of Pulp; measuring that grades the interface. What is
# Pulp's is whether decode and conversion track a reference WITHOUT adding error
# of their own, and that is deterministic.
#
# So this drives the real `MtcChaser` with synthetic MTC whose drift is INJECTED
# at a known rate, and asserts the result against the same fixed tolerance spec
# the rig run uses. Being able to dial the exact drift makes it stricter than a
# rig run, not weaker — a real oscillator gives you whatever it happens to do
# that afternoon, and cannot be reproduced.
#
# Both directions are asserted, because a soak that can only pass proves
# nothing: a clean chase must PASS and an over-budget drift must FAIL.
if(TARGET pulp-sync-soak-capture AND Python3_Interpreter_FOUND)
    set(_sync_soak_spec "${CMAKE_CURRENT_BINARY_DIR}/sync-soak-spec.json")
    # The spec lives in the private planning repo, which an external clone does
    # not have. Materialise the same numbers here so the gate runs everywhere;
    # planning remains the human-facing record of WHY they are these numbers.
    file(WRITE "${_sync_soak_spec}"
"{\n"
"  \"schema\": \"pulp.timeline-sync-soak-spec.v1\",\n"
"  \"fixed_at\": \"2026-07-27T00:00:00Z\",\n"
"  \"min_duration_seconds\": 1800,\n"
"  \"max_abs_offset_samples\": 64,\n"
"  \"max_drift_ppm\": 25,\n"
"  \"min_points_per_stream\": 120\n"
"}\n")

    add_test(NAME timeline-sync-soak-engine
        COMMAND ${CMAKE_COMMAND}
            -DCAPTURE=$<TARGET_FILE:pulp-sync-soak-capture>
            -DVERIFIER=${CMAKE_SOURCE_DIR}/tools/scripts/verify_timeline_sync_soak.py
            -DPYTHON=${Python3_EXECUTABLE}
            -DSPEC=${_sync_soak_spec}
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/sync-soak
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_sync_soak_engine.cmake)
    set_tests_properties(timeline-sync-soak-engine PROPERTIES
        LABELS "timeline;sync;validation"
        TIMEOUT 120)
endif()
