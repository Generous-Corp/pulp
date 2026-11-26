cmake_minimum_required(VERSION 3.24)
project(PulpVellumInstalledConsumer LANGUAGES CXX)

# Consumer of an installed Pulp SDK's Vellum integration packages, configured
# from scratch against the install prefix alone (plus the Vellum SDK, when the
# prefix does not stage one). It links Pulp::vellum and Pulp::vellum-audio,
# calls one compiled function from each, and runs the result, so a missing
# archive, header, include directory or Vellum link is a build failure here
# rather than in a downstream plug-in.
find_package(Pulp CONFIG REQUIRED)

foreach(_target IN ITEMS Pulp::vellum Pulp::vellum-audio)
    if(NOT TARGET "${_target}")
        message(FATAL_ERROR "the installed SDK does not export ${_target}")
    endif()
endforeach()

# Vellum must resolve to a real package, not to an unreferenced name.
if(NOT Vellum_FOUND)
    message(FATAL_ERROR "the installed SDK's Vellum package was not found")
endif()

# One GPU device layer per binary: the audio widgets must not reach Pulp's
# static gpu_runtime, or every Vellum-backed consumer of them would be refused.
# Walk the installed link closure of both packages.
function(_closure_names root out)
    set(_queue "${root}")
    set(_seen "")
    while(_queue)
        list(POP_FRONT _queue _item)
        string(REGEX MATCHALL "[A-Za-z0-9_]+::[A-Za-z0-9_+-]+" _names "${_item}")
        foreach(_name IN LISTS _names)
            if(_name IN_LIST _seen OR NOT TARGET "${_name}")
                continue()
            endif()
            list(APPEND _seen "${_name}")
            get_target_property(_real "${_name}" ALIASED_TARGET)
            if(NOT _real)
                set(_real "${_name}")
            endif()
            get_target_property(_links "${_real}" INTERFACE_LINK_LIBRARIES)
            if(_links)
                list(APPEND _queue ${_links})
            endif()
        endforeach()
    endwhile()
    set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

_closure_names("Pulp::vellum-audio;Pulp::vellum" _closure)
message(STATUS "installed closure: ${_closure}")
foreach(_gpu IN ITEMS Pulp::gpu-device Pulp::gpu-audio Pulp::host VellumGpuRuntime::Device)
    if(_gpu IN_LIST _closure)
        message(FATAL_ERROR "the Vellum integration packages reach ${_gpu}, a second GPU device layer")
    endif()
endforeach()
# Control: the same walk over Pulp's GPU device target must reach the static
# device layer, so an empty result above means the packages are clean rather
# than that the walk cannot see it.
if(TARGET Pulp::gpu-device)
    _closure_names("Pulp::gpu-device" _gpu_closure)
    if(NOT "VellumGpuRuntime::Device" IN_LIST _gpu_closure)
        message(FATAL_ERROR "control: the closure walk does not reach VellumGpuRuntime::Device from Pulp::gpu-device: ${_gpu_closure}")
    endif()
    message(STATUS "control: Pulp::gpu-device reaches VellumGpuRuntime::Device")
else()
    message(STATUS "control skipped: this SDK exports no Pulp::gpu-device")
endif()
if(NOT "Vellum::MirrorView_core" IN_LIST _closure)
    message(FATAL_ERROR "the closure never reaches Vellum::MirrorView_core: ${_closure}")
endif()

add_executable(pulp_vellum_installed_consumer main.cpp)
target_link_libraries(pulp_vellum_installed_consumer PRIVATE Pulp::vellum-audio Pulp::vellum)
add_custom_command(TARGET pulp_vellum_installed_consumer POST_BUILD
    COMMAND "$<TARGET_FILE:pulp_vellum_installed_consumer>"
    COMMENT "Running the installed-SDK Vellum consumer"
    VERBATIM)
