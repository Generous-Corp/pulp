cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/rmlui-sdk-consumer")
set(_prefix "${_fixture_root}/prefix")
set(_source "${_fixture_root}/src")
set(_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")
file(MAKE_DIRECTORY "${_source}")

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "RmlUi SDK staging failed (${_install_result})\n${_install_output}\n${_install_error}")
endif()

if(NOT EXISTS "${_prefix}/lib/cmake/RmlUi/RmlUiTargets.cmake")
    message(FATAL_ERROR "installed RmlUi package is missing RmlUiTargets.cmake")
endif()
if(NOT EXISTS "${_prefix}/include/RmlUi/Core/Variant.inl")
    message(FATAL_ERROR "installed RmlUi headers are missing Variant.inl")
endif()

set(_consumer_lua_include "")
set(_consumer_lua_probe "")
if(PULP_ENABLE_RMLUI_LUA)
    set(_consumer_lua_include "#include <RmlUi/Lua/Lua.h>\n")
    set(_consumer_lua_probe "  Rml::Lua::Initialise();\n")
endif()

file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpRmlUiSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Pulp CONFIG REQUIRED)
add_executable(pulp-rmlui-sdk-consumer main.cpp)
target_link_libraries(pulp-rmlui-sdk-consumer PRIVATE Pulp::view-rmlui RmlUi::Core)
]=])
file(WRITE "${_source}/main.cpp"
"${_consumer_lua_include}#include <RmlUi/Core/Variant.h>\n"
"#include <pulp/view/rmlui_backend.hpp>\n"
"int main() {\n"
"${_consumer_lua_probe}"
"    Rml::Variant value;\n"
"    return value.GetType() == Rml::Variant::NONE ? 0 : 0;\n"
"}\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}"
            -G Ninja -DCMAKE_BUILD_TYPE=${_config}
            -DCMAKE_PREFIX_PATH=${_prefix}
            -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
            -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "RmlUi SDK consumer configure failed (${_configure_result})\n${_configure_output}\n${_configure_error}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "RmlUi SDK consumer build failed (${_build_result})\n${_build_output}\n${_build_error}")
endif()
