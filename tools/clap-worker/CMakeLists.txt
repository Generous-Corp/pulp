# The production companion is installed; the hostile fixture is test-only.
if(IOS OR ANDROID)
    return()
endif()

get_target_property(_clap_worker_fingerprint pulp-host PULP_CLAP_WORKER_FINGERPRINT)

add_executable(pulp-clap-worker clap_worker_main.cpp)
target_include_directories(pulp-clap-worker PRIVATE "${PULP_ROOT_DIR}/core/host/src")
target_link_libraries(pulp-clap-worker PRIVATE pulp::host pulp::runtime)
target_compile_definitions(pulp-clap-worker PRIVATE
    PULP_CLAP_WORKER_BUILD="${_clap_worker_fingerprint}")
if(APPLE AND NOT IOS AND PULP_HAS_CLAP AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    target_compile_definitions(pulp-clap-worker PRIVATE PULP_ISOLATED_CLAP_SUPPORTED=1)
endif()
if(PULP_HAS_CLAP)
    target_link_libraries(pulp-clap-worker PRIVATE clap)
endif()
if(APPLE)
    set_target_properties(pulp-clap-worker PROPERTIES INSTALL_RPATH "@executable_path/../lib")
endif()
install(TARGETS pulp-clap-worker RUNTIME DESTINATION bin)

if(PULP_BUILD_TESTS)
    add_executable(pulp-test-isolated-clap-protocol "${PULP_ROOT_DIR}/test/test_isolated_clap_protocol.cpp")
    target_include_directories(pulp-test-isolated-clap-protocol PRIVATE
        "${PULP_ROOT_DIR}/core/host/include" "${PULP_ROOT_DIR}/core/host/src")
    target_link_libraries(pulp-test-isolated-clap-protocol PRIVATE
        pulp::audio pulp::format pulp::midi pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-isolated-clap-protocol)
endif()

if(PULP_BUILD_TESTS AND APPLE AND PULP_HAS_CLAP AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    add_executable(pulp-test-clap-worker-mismatched clap_worker_main.cpp)
    target_include_directories(pulp-test-clap-worker-mismatched PRIVATE "${PULP_ROOT_DIR}/core/host/src")
    target_link_libraries(pulp-test-clap-worker-mismatched PRIVATE pulp::host pulp::runtime clap)
    target_compile_definitions(pulp-test-clap-worker-mismatched PRIVATE
        PULP_ISOLATED_CLAP_SUPPORTED=1
        PULP_CLAP_WORKER_BUILD="0000000000000000000000000000000000000000000000000000000000000000")
    add_executable(pulp-test-isolated-clap-construction
        "${PULP_ROOT_DIR}/test/fixtures/isolated_clap/construction_probe.cpp")
    target_link_libraries(pulp-test-isolated-clap-construction PRIVATE pulp::host)
    add_dependencies(pulp-test-isolated-clap-construction pulp-clap-worker pulp-test-clap-worker-mismatched)
    add_library(pulp-isolated-clap-fixture MODULE "${PULP_ROOT_DIR}/test/fixtures/isolated_clap/plugin.cpp")
    target_link_libraries(pulp-isolated-clap-fixture PRIVATE clap)
    set_target_properties(pulp-isolated-clap-fixture PROPERTIES PREFIX "" SUFFIX ".clap")
    add_executable(pulp-test-isolated-clap-worker "${PULP_ROOT_DIR}/test/test_isolated_clap_worker.cpp")
    target_link_libraries(pulp-test-isolated-clap-worker PRIVATE pulp::host Catch2::Catch2WithMain)
    target_include_directories(pulp-test-isolated-clap-worker PRIVATE "${PULP_ROOT_DIR}/core/host/src")
    target_compile_definitions(pulp-test-isolated-clap-worker PRIVATE
        PULP_ISOLATED_CLAP_FIXTURE="$<TARGET_FILE:pulp-isolated-clap-fixture>"
        PULP_CLAP_WORKER_PATH="$<TARGET_FILE:pulp-clap-worker>"
        PULP_MISMATCHED_CLAP_WORKER_PATH="$<TARGET_FILE:pulp-test-clap-worker-mismatched>")
    add_dependencies(pulp-test-isolated-clap-worker pulp-isolated-clap-fixture pulp-clap-worker pulp-test-clap-worker-mismatched)
    target_sources(pulp-test-isolated-clap-worker PRIVATE
        "${PULP_ROOT_DIR}/test/native_components/rt_intercept_test_support.cpp"
        "${PULP_ROOT_DIR}/test/fixtures/isolated_clap/callback_syscall_traps.cpp")
    target_compile_definitions(pulp-test-isolated-clap-worker PRIVATE
        PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1)
    catch_discover_tests(pulp-test-isolated-clap-worker PROPERTIES TIMEOUT 60)
    add_test(NAME isolated-clap-construction-recovery
        COMMAND pulp-test-isolated-clap-construction
            "$<TARGET_FILE:pulp-test-clap-worker-mismatched>"
            "$<TARGET_FILE:pulp-clap-worker>"
            "$<TARGET_FILE:pulp-isolated-clap-fixture>" complete)
    set_tests_properties(isolated-clap-construction-recovery PROPERTIES TIMEOUT 20 LABELS "host")
    add_test(NAME cmake-g9-isolated-clap-sdk-consumer
        COMMAND "${CMAKE_COMMAND}"
            "-DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DPULP_SOURCE_DIR=${PULP_ROOT_DIR}"
            "-DPULP_FIXTURE=$<TARGET_FILE:pulp-isolated-clap-fixture>"
            -P "${PULP_ROOT_DIR}/test/cmake/test_g9_isolated_clap_sdk_consumer.cmake")
    set_tests_properties(cmake-g9-isolated-clap-sdk-consumer PROPERTIES TIMEOUT 600 LABELS "installed-sdk;host")
endif()
