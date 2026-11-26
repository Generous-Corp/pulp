# Verify source-install provenance follows the checkout at install time.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()

get_filename_component(PULP_SOURCE_DIR "${PULP_SOURCE_DIR}" ABSOLUTE)
get_filename_component(FIXTURE_DIR "${FIXTURE_DIR}" ABSOLUTE)
file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}/source")
file(WRITE "${FIXTURE_DIR}/source/payload.txt" "initial\n")

execute_process(COMMAND git init --quiet WORKING_DIRECTORY "${FIXTURE_DIR}/source"
    RESULT_VARIABLE _git_init_rc)
execute_process(COMMAND git config user.email test@example.invalid
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_email_rc)
execute_process(COMMAND git config user.name fixture
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_name_rc)
execute_process(COMMAND git add payload.txt
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_add_rc)
execute_process(COMMAND git commit --quiet -m initial
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_commit_rc)
if(_git_init_rc OR _git_email_rc OR _git_name_rc OR _git_add_rc OR _git_commit_rc)
    message(FATAL_ERROR "could not initialize fixture repository")
endif()

set(_template "${PULP_SOURCE_DIR}/tools/cmake/PulpStampSourceInstall.cmake.in")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(source-install-fixture NONE)
set(CMAKE_SUPPRESS_REGENERATION ON)
find_package(Git REQUIRED)
set(PULP_ROOT_DIR "@FIXTURE_SOURCE@")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PULP_ROOT_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE PULP_SOURCE_INSTALL_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(PULP_SOURCE_INSTALL_DIRTY false)
set(PULP_SOURCE_INSTALL_BRIDGE_EXEC false)
set(PULP_SOURCE_INSTALL_OS darwin)
set(PULP_SOURCE_INSTALL_ARCH arm64)
set(PULP_PROFILE dev)
set(PULP_PROFILE_OVERRIDDEN OFF)
configure_file("@STAMP_TEMPLATE@" "${CMAKE_BINARY_DIR}/PulpStampSourceInstall.cmake" @ONLY)
add_custom_target(fixture ALL COMMAND "${CMAKE_COMMAND}" -E copy
    "${PULP_ROOT_DIR}/payload.txt" "${CMAKE_BINARY_DIR}/payload.txt"
    VERBATIM)
install(FILES "${CMAKE_BINARY_DIR}/payload.txt" DESTINATION ".")
install(SCRIPT "${CMAKE_BINARY_DIR}/PulpStampSourceInstall.cmake")
]=])
file(READ "${FIXTURE_DIR}/source/CMakeLists.txt" _fixture_cmake)
string(REPLACE "@FIXTURE_SOURCE@" "${FIXTURE_DIR}/source" _fixture_cmake "${_fixture_cmake}")
string(REPLACE "@STAMP_TEMPLATE@" "${_template}" _fixture_cmake "${_fixture_cmake}")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" "${_fixture_cmake}")

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/source"
    -B "${FIXTURE_DIR}/build" -G Ninja
    RESULT_VARIABLE _configure_rc OUTPUT_VARIABLE _configure_out ERROR_VARIABLE _configure_err)
if(NOT _configure_rc EQUAL 0)
    message(FATAL_ERROR "fixture configure failed:\n${_configure_out}\n${_configure_err}")
endif()

file(WRITE "${FIXTURE_DIR}/source/payload.txt" "advanced\n")
execute_process(COMMAND git add payload.txt
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_add_advanced_rc)
execute_process(COMMAND git commit --quiet -m advanced
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_commit_advanced_rc)
if(_git_add_advanced_rc OR _git_commit_advanced_rc)
    message(FATAL_ERROR "could not advance fixture repository")
endif()
execute_process(COMMAND git -C "${FIXTURE_DIR}/source" rev-parse HEAD
    OUTPUT_VARIABLE _expected_sha OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _head_rc)
if(NOT _head_rc EQUAL 0)
    message(FATAL_ERROR "could not read advanced fixture HEAD")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FIXTURE_DIR}/build"
    RESULT_VARIABLE _build_rc OUTPUT_VARIABLE _build_out ERROR_VARIABLE _build_err)
if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR "fixture build failed:\n${_build_out}\n${_build_err}")
endif()
file(APPEND "${FIXTURE_DIR}/source/payload.txt" "local edit\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${FIXTURE_DIR}/build"
    --prefix "${FIXTURE_DIR}/prefix"
    RESULT_VARIABLE _install_rc OUTPUT_VARIABLE _install_out ERROR_VARIABLE _install_err)
if(NOT _install_rc EQUAL 0)
    message(FATAL_ERROR "fixture install failed:\n${_install_out}\n${_install_err}")
endif()

file(READ "${FIXTURE_DIR}/prefix/sdk-provenance.json" _provenance)
string(JSON _installed_sha ERROR_VARIABLE _sha_error GET "${_provenance}" source_git_sha)
if(_sha_error OR NOT _installed_sha STREQUAL _expected_sha)
    message(FATAL_ERROR
        "install-time source SHA did not follow advanced HEAD: expected=${_expected_sha} "
        "installed=${_installed_sha}\n${_provenance}")
endif()
string(JSON _installed_dirty GET "${_provenance}" source_git_dirty)
if(NOT _installed_dirty)
    message(FATAL_ERROR "source install did not record the tracked edit made after configure")
endif()
file(READ "${FIXTURE_DIR}/prefix/payload.txt" _installed_payload)
if(NOT _installed_payload STREQUAL "advanced\n")
    message(FATAL_ERROR "fixture did not install the payload built from the advanced checkout")
endif()

# A later incremental build must refresh the source-install marker in the same
# prefix rather than copying new payloads and then failing on the old marker.
file(WRITE "${FIXTURE_DIR}/source/payload.txt" "second\n")
execute_process(COMMAND git add payload.txt
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_add_second_rc)
execute_process(COMMAND git commit --quiet -m second
    WORKING_DIRECTORY "${FIXTURE_DIR}/source" RESULT_VARIABLE _git_commit_second_rc)
if(_git_add_second_rc OR _git_commit_second_rc)
    message(FATAL_ERROR "could not advance fixture repository a second time")
endif()
execute_process(COMMAND git -C "${FIXTURE_DIR}/source" rev-parse HEAD
    OUTPUT_VARIABLE _second_expected_sha OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _second_head_rc)
if(NOT _second_head_rc EQUAL 0)
    message(FATAL_ERROR "could not read second fixture HEAD")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${FIXTURE_DIR}/build"
    RESULT_VARIABLE _second_build_rc OUTPUT_VARIABLE _second_build_out
    ERROR_VARIABLE _second_build_err)
if(NOT _second_build_rc EQUAL 0)
    message(FATAL_ERROR "second fixture build failed:\n${_second_build_out}\n${_second_build_err}")
endif()
file(APPEND "${FIXTURE_DIR}/source/payload.txt" "second local edit\n")
# Force the second install to copy the newly built payload even on filesystems
# whose timestamp resolution makes the source and destination look equal.
file(REMOVE "${FIXTURE_DIR}/prefix/payload.txt")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${FIXTURE_DIR}/build"
    --prefix "${FIXTURE_DIR}/prefix"
    RESULT_VARIABLE _second_install_rc OUTPUT_VARIABLE _second_install_out
    ERROR_VARIABLE _second_install_err)
if(NOT _second_install_rc EQUAL 0)
    message(FATAL_ERROR
        "repeated fixture install failed:\n${_second_install_out}\n${_second_install_err}")
endif()
file(READ "${FIXTURE_DIR}/prefix/sdk-provenance.json" _second_provenance)
string(JSON _second_installed_sha ERROR_VARIABLE _second_sha_error GET
    "${_second_provenance}" source_git_sha)
if(_second_sha_error OR NOT _second_installed_sha STREQUAL _second_expected_sha)
    message(FATAL_ERROR
        "repeated install did not refresh source SHA: expected=${_second_expected_sha} "
        "installed=${_second_installed_sha}\n${_second_provenance}")
endif()
string(JSON _second_installed_dirty GET "${_second_provenance}" source_git_dirty)
if(NOT _second_installed_dirty)
    message(FATAL_ERROR "repeated install did not record the tracked edit")
endif()
file(READ "${FIXTURE_DIR}/prefix/payload.txt" _second_installed_payload)
if(NOT _second_installed_payload STREQUAL "second\n")
    message(FATAL_ERROR "repeated install did not install the second payload")
endif()

# A release marker is immutable and must still reject a source-install refresh.
set(_release_prefix "${FIXTURE_DIR}/release-prefix")
file(MAKE_DIRECTORY "${_release_prefix}")
set(_release_marker "{\n  \"schema\": \"pulp.sdk-provenance.v1\",\n  \"kind\": \"release\",\n  \"distribution_eligible\": true,\n  \"source_git_sha\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n}\n")
file(WRITE "${_release_prefix}/sdk-provenance.json" "${_release_marker}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${FIXTURE_DIR}/build"
    --prefix "${_release_prefix}"
    RESULT_VARIABLE _release_install_rc OUTPUT_VARIABLE _release_install_out
    ERROR_VARIABLE _release_install_err)
if(_release_install_rc EQUAL 0)
    message(FATAL_ERROR "source install replaced an existing release marker")
endif()
file(READ "${_release_prefix}/sdk-provenance.json" _retained_release_marker)
if(NOT _retained_release_marker STREQUAL _release_marker)
    message(FATAL_ERROR "source install modified the existing release marker")
endif()
message(STATUS "PASS: source install SHA follows repeated install-time HEAD (${_second_expected_sha})")
