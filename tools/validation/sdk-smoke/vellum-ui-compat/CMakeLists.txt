cmake_minimum_required(VERSION 3.24)

# Installed-SDK consumer of the Vellum-backed Pulp UI names.
#
# Builds one executable per compatibility name, each linking only that name
# and including a header through it, then runs every executable. The
# design-import executable parses a DESIGN.md document, so the proof covers
# the implementation behind the name rather than an unreferenced library.
#
# Runs standalone against an installed SDK, or as a subdirectory of the SDK
# smoke when that SDK reports Vellum-backed names.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PulpVellumUiCompatConsumer LANGUAGES CXX)
    find_package(Pulp CONFIG REQUIRED)
endif()

set(PULP_VELLUM_UI_COMPAT_PROBE_NAMES
    "view;canvas;render;design-import;view-audio;view-tooling"
    CACHE STRING "Pulp UI names this consumer must link")

foreach(_name IN LISTS PULP_VELLUM_UI_COMPAT_PROBE_NAMES)
    if(NOT _name IN_LIST PULP_SDK_VELLUM_UI_COMPAT_TARGETS)
        message(FATAL_ERROR
            "Pulp::${_name} is not Vellum-backed in this SDK "
            "(PULP_SDK_VELLUM_UI_COMPAT_TARGETS='${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}')")
    endif()
    string(REPLACE "-" "_" _id "${_name}")
    string(TOUPPER "${_id}" _upper)
    set(_exe "PulpVellumUiCompat_${_id}")
    add_executable(${_exe} "${CMAKE_CURRENT_LIST_DIR}/vellum_ui_compat_probe.cpp")
    target_compile_definitions(${_exe} PRIVATE PULP_VELLUM_UI_COMPAT_PROBE_${_upper}=1)
    target_link_libraries(${_exe} PRIVATE "Pulp::${_name}")
    add_custom_command(TARGET ${_exe} POST_BUILD
        COMMAND "$<TARGET_FILE:${_exe}>"
        COMMENT "Running Vellum-backed Pulp::${_name} probe"
        VERBATIM)
endforeach()
unset(_name)
unset(_id)
unset(_upper)
unset(_exe)
