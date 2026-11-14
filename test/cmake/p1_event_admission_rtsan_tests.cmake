# Realtime proof of the P1 event-admission consumer's combined audio callback.
# Built in-tree from the installed-SDK consumer source only when the tree is
# configured with PULP_SANITIZER=realtime, so the required gate never sees it.
# The positive run must exit clean; each planted control (malloc, mutex,
# sleep at the [[clang::nonblocking]] boundary, and malloc inside the graph's
# custom-node callback and the voice render) must produce an RTSan
# unsafe-library-call report naming the planted call.
if(TARGET pulp-host AND PULP_SANITIZER STREQUAL "realtime")
    add_executable(pulp-test-p1-event-admission-rtsan
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-event-admission-consumer/main.cpp")
    target_link_libraries(pulp-test-p1-event-admission-rtsan PRIVATE pulp::host)
    target_compile_features(pulp-test-p1-event-admission-rtsan PRIVATE cxx_std_20)

    add_test(NAME p1-event-admission-rtsan-positive
        COMMAND pulp-test-p1-event-admission-rtsan)
    set_tests_properties(p1-event-admission-rtsan-positive PROPERTIES
        LABELS "graph;rtsan" TIMEOUT 120
        PASS_REGULAR_EXPRESSION "realtime_boundary=nonblocking realtime_boundary_blocks=[1-9]"
        FAIL_REGULAR_EXPRESSION "RealtimeSanitizer")

    set(_p1_rtsan_controls
        "malloc|`malloc`"
        "mutex|`pthread_mutex_lock`"
        "sleep|`nanosleep`"
        "node-malloc|`malloc`"
        "voice-malloc|`malloc`")
    foreach(_entry IN LISTS _p1_rtsan_controls)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _control)
        list(GET _parts 1 _symbol)
        add_test(NAME p1-event-admission-rtsan-control-${_control}
            COMMAND pulp-test-p1-event-admission-rtsan)
        set_tests_properties(p1-event-admission-rtsan-control-${_control} PROPERTIES
            LABELS "graph;rtsan" TIMEOUT 120
            ENVIRONMENT "P1_RTSAN_CONTROL=${_control};RTSAN_OPTIONS=abort_on_error=0"
            PASS_REGULAR_EXPRESSION
                "RealtimeSanitizer: unsafe-library-call\nIntercepted call to real-time unsafe function ${_symbol} in real-time context")
    endforeach()
endif()
