cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}/source")
file(WRITE "${FIXTURE_DIR}/source/main.cpp" "int main() { return 0; }\n")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(IosAuv3ControlShipping LANGUAGES CXX)

include("@PULP_SOURCE_DIR@/tools/cmake/PulpControlShipping.cmake")
include("@PULP_SOURCE_DIR@/tools/cmake/PulpAuv3.cmake")

add_library(pulp-format-fixture INTERFACE)
set(_PULP_FORMAT_TARGET pulp-format-fixture)
set(PULP_IOS TRUE)

function(_pulp_metadata_require_fourcc)
endfunction()

# Keep the public helper under test while replacing its heavyweight iOS target
# implementation with the one operation relevant to this contract.
function(_pulp_add_auv3 target)
    add_executable(${target}_AUv3 main.cpp)
    _pulp_attach_control_shipping(${target} ${target}_AUv3 AUv3Extension)
endfunction()

pulp_add_ios_auv3(
    NAME FixtureAuv3
    BUNDLE_ID com.pulp.test.fixture
    MANUFACTURER Pulp
    MANUFACTURER_CODE Pulp
    SUBTYPE_CODE Fxtr
    VERSION 1.0.0)
]=])

file(READ "${FIXTURE_DIR}/source/CMakeLists.txt" _fixture_cmake)
string(REPLACE "@PULP_SOURCE_DIR@" "${PULP_SOURCE_DIR}"
    _fixture_cmake "${_fixture_cmake}")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" "${_fixture_cmake}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/source"
            -B "${FIXTURE_DIR}/build"
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "iOS AUv3 control-shipping fixture failed to configure: "
        "${_configure_output}${_configure_error}")
endif()

foreach(_required IN ITEMS
        "pulp-inspector-manifests/FixtureAuv3.json"
        "pulp-control-shipping-manifests/FixtureAuv3.AUv3Extension.control-shipping.json")
    if(NOT EXISTS "${FIXTURE_DIR}/build/${_required}")
        message(FATAL_ERROR "iOS AUv3 control-shipping output missing: ${_required}")
    endif()
endforeach()
