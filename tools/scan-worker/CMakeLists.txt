# pulp-scan-worker — isolation binary for out-of-process plugin scanning.
#
# The parent PluginScanner launches this binary per plugin bundle; if the
# child crashes (SIGSEGV, abort, etc.) the parent records the bundle in
# its ScanBlacklist (workstream 03 slice 3.3a) and continues with the
# next candidate. The worker itself just links against pulp::host and
# prints the scanned metadata as JSON on stdout.
#
# Workstream 03 slice 3.3b.
if(IOS OR ANDROID)
    # Sandboxed platforms can't fork/exec a helper binary. Skip.
    return()
endif()

add_executable(pulp-scan-worker scan_worker_main.cpp)
target_link_libraries(pulp-scan-worker PRIVATE pulp::host pulp::runtime)
set_target_properties(pulp-scan-worker PROPERTIES OUTPUT_NAME "pulp-scan-worker")

if(PULP_BUILD_TESTS)
    add_executable(pulp-test-scan-worker test_scan_worker.cpp)
    target_link_libraries(pulp-test-scan-worker PRIVATE
        pulp::platform
        Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-scan-worker PRIVATE
        PULP_SCAN_WORKER_PATH="$<TARGET_FILE:pulp-scan-worker>")
    add_dependencies(pulp-test-scan-worker pulp-scan-worker)
    catch_discover_tests(pulp-test-scan-worker)
endif()

install(TARGETS pulp-scan-worker
    RUNTIME DESTINATION bin
)
