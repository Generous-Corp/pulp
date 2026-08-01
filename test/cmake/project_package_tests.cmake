# Durable project-package publication and real helper-process crash recovery.
add_executable(pulp-project-package-publish-helper
    fixtures/project_package_publish_helper.cpp)
target_include_directories(pulp-project-package-publish-helper PRIVATE
    "${CMAKE_SOURCE_DIR}/core/project_package/src")
target_link_libraries(pulp-project-package-publish-helper PRIVATE
    pulp::project-package pulp::runtime)

# Compile the deliberate negative control as a separate test-only copy. The
# production archive has neither the mutation branch nor its control symbol.
add_library(pulp-project-package-test-mutant STATIC
    "${CMAKE_SOURCE_DIR}/core/project_package/src/atomic_publisher.cpp"
    "${CMAKE_SOURCE_DIR}/core/project_package/src/native_io.cpp"
    "${CMAKE_SOURCE_DIR}/core/project_package/src/project_package.cpp"
    "${CMAKE_SOURCE_DIR}/core/project_package/src/project_package_test_access.cpp")
target_compile_features(pulp-project-package-test-mutant PUBLIC cxx_std_20)
target_compile_definitions(pulp-project-package-test-mutant PRIVATE
    PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS=1)
target_include_directories(pulp-project-package-test-mutant
    PUBLIC "${CMAKE_SOURCE_DIR}/core/project_package/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/core/project_package/src")
target_link_libraries(pulp-project-package-test-mutant
    PUBLIC pulp::timeline
    PRIVATE pulp::runtime)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-project-package-test-mutant PRIVATE
        -fno-exceptions -fno-rtti)
endif()

add_executable(pulp-project-package-mutant-helper
    fixtures/project_package_publish_helper.cpp)
target_compile_definitions(pulp-project-package-mutant-helper PRIVATE
    PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS=1
    PULP_PROJECT_PACKAGE_MUTANT_HELPER=1)
target_include_directories(pulp-project-package-mutant-helper PRIVATE
    "${CMAKE_SOURCE_DIR}/core/project_package/src")
target_link_libraries(pulp-project-package-mutant-helper PRIVATE
    pulp-project-package-test-mutant pulp::runtime)

pulp_add_test_suite(pulp-test-project-package
    LIBRARIES pulp::project-package pulp::platform pulp::runtime
    COMPILE_DEFINITIONS
        PULP_PROJECT_PACKAGE_PUBLISH_HELPER="$<TARGET_FILE:pulp-project-package-publish-helper>"
        PULP_PROJECT_PACKAGE_MUTANT_HELPER="$<TARGET_FILE:pulp-project-package-mutant-helper>"
    LABELS timeline crash-recovery
    TIMEOUT 30)
add_dependencies(pulp-test-project-package
    pulp-project-package-publish-helper pulp-project-package-mutant-helper)

if(Python3_Interpreter_FOUND)
    set(_project_package_mutation_runner
        "${CMAKE_CURRENT_BINARY_DIR}/run_project_package_mutation_control.py")
    file(GENERATE OUTPUT "${_project_package_mutation_runner}" CONTENT [=[
import os
import subprocess
import sys

environment = os.environ.copy()
environment["PULP_PROJECT_PACKAGE_RUN_MUTATION_CONTROL"] = "1"
result = subprocess.run(
    [sys.argv[1], "[mutation-control]"],
    env=environment,
    check=False,
    capture_output=True,
    text=True,
)
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
marker = "project-package mutation sentinel: missing-reference generation exposed"
if result.returncode == 42 and marker in result.stdout + result.stderr:
    print("project-package production mutation was caught; sentinel exit 42")
    sys.exit(42)
print(
    f"project-package mutation control returned status {result.returncode} "
    f"with marker_present={marker in result.stdout + result.stderr}"
)
sys.exit(0)
]=])
    add_test(NAME project-package-mutation-control
        COMMAND "${Python3_EXECUTABLE}" "${_project_package_mutation_runner}"
            $<TARGET_FILE:pulp-test-project-package>)
    set_tests_properties(project-package-mutation-control PROPERTIES
        LABELS "timeline;crash-recovery;mutation-control"
        TIMEOUT 30
        WILL_FAIL TRUE)
endif()
