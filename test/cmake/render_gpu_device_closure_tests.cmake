# GPU device layer closure: pulp-gpu-device-api and pulp-gpu-device link and
# include nothing above themselves, and pulp-gpu-audio reaches the device
# without the 2D renderer.
#
# Three instruments, because each sees a different failure:
#   pulp-gpu-device-api-only-consumer  builds against the API target alone, so
#       a device header that starts naming Dawn, or an inline helper that
#       starts calling into the implementation, fails to compile or link.
#   pulp-gpu-device-only-consumer      links the device target alone and
#       references every device TU, so an undefined symbol only pulp-render
#       could satisfy fails here rather than in a GPU-audio consumer.
#   gpu-device-link-closure            reads the configured link properties and
#       the sources, which is the only place a PRIVATE-vs-PUBLIC link or an
#       upward include is visible: both still build while the renderer is in
#       the final link.
# Its rule set is unit-tested, violation by violation, by
# gpu-device-link-closure-rules.
# Included by render_gpu_tests.cmake under PULP_ENABLE_GPU.

add_executable(pulp-test-gpu-device-api-only-consumer
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gpu_device_link_closure/api_consumer.cpp)
target_link_libraries(pulp-test-gpu-device-api-only-consumer PRIVATE pulp::gpu-device-api)
add_test(NAME pulp-gpu-device-api-only-consumer
    COMMAND pulp-test-gpu-device-api-only-consumer)

add_executable(pulp-test-gpu-device-only-consumer
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gpu_device_link_closure/device_consumer.cpp)
target_link_libraries(pulp-test-gpu-device-only-consumer PRIVATE pulp::gpu-device)
add_test(NAME pulp-gpu-device-only-consumer
    COMMAND pulp-test-gpu-device-only-consumer)
set_tests_properties(
    pulp-gpu-device-api-only-consumer
    pulp-gpu-device-only-consumer
    PROPERTIES LABELS "render;gpu-device;link-closure")

# Raw, unevaluated properties: the checker needs `$<LINK_ONLY:...>` intact to
# tell a PRIVATE static-library link from a PUBLIC one.
set(_pulp_gpu_device_props "")
foreach(_pulp_gpu_entry IN ITEMS
        "api.type;pulp-gpu-device-api;TYPE"
        "api.interface_link;pulp-gpu-device-api;INTERFACE_LINK_LIBRARIES"
        "device.type;pulp-gpu-device;TYPE"
        "device.link;pulp-gpu-device;LINK_LIBRARIES"
        "device.interface_link;pulp-gpu-device;INTERFACE_LINK_LIBRARIES"
        "device.sources;pulp-gpu-device;SOURCES"
        "device.source_dir;pulp-gpu-device;SOURCE_DIR"
        "render.link;pulp-render;LINK_LIBRARIES"
        "render.interface_link;pulp-render;INTERFACE_LINK_LIBRARIES"
        "gpu_audio.link;pulp-gpu-audio;LINK_LIBRARIES"
        "gpu_audio.source_dir;pulp-gpu-audio;SOURCE_DIR"
        "api_consumer.link;pulp-test-gpu-device-api-only-consumer;LINK_LIBRARIES"
        "device_consumer.link;pulp-test-gpu-device-only-consumer;LINK_LIBRARIES")
    list(GET _pulp_gpu_entry 0 _pulp_gpu_key)
    list(GET _pulp_gpu_entry 1 _pulp_gpu_target)
    list(GET _pulp_gpu_entry 2 _pulp_gpu_property)
    get_target_property(_pulp_gpu_value ${_pulp_gpu_target} ${_pulp_gpu_property})
    string(APPEND _pulp_gpu_device_props "${_pulp_gpu_key}\t${_pulp_gpu_value}\n")
endforeach()
# pulp-gpu-audio's GPU path is compiled exactly when the device target exists,
# which is always the case inside this PULP_ENABLE_GPU manifest.
string(APPEND _pulp_gpu_device_props "gpu_audio.expects_device\t1\n")
set(_pulp_gpu_device_props_file
    "${CMAKE_CURRENT_BINARY_DIR}/gpu_device_link_closure.properties")
file(WRITE "${_pulp_gpu_device_props_file}" "${_pulp_gpu_device_props}")

if(Python3_Interpreter_FOUND)
    add_test(NAME gpu-device-link-closure
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/scripts/gpu_device_link_closure_check.py
                --properties "${_pulp_gpu_device_props_file}"
                --render-include "${CMAKE_SOURCE_DIR}/core/render/include")
    add_test(NAME gpu-device-link-closure-rules
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/scripts/test_gpu_device_link_closure_check.py)
    set_tests_properties(gpu-device-link-closure gpu-device-link-closure-rules
        PROPERTIES LABELS "render;gpu-device;link-closure" TIMEOUT 60)
endif()

# The device-layer mode is one fact for every consumer: PULP_GPU_RUNTIME sits
# on pulp::platform and pulp::gpu-device-api exactly when this build forwards to
# gpu-runtime, and a PULP_USE_GPU_RUNTIME=OFF build leaks it nowhere.
get_target_property(_pulp_platform_defs pulp-platform INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_pulp_device_api_defs pulp-gpu-device-api INTERFACE_COMPILE_DEFINITIONS)
foreach(_pulp_defs_var _pulp_platform_defs _pulp_device_api_defs)
    if(NOT ${_pulp_defs_var})
        set(${_pulp_defs_var} "")
    endif()
    string(REPLACE ";" "|" ${_pulp_defs_var} "${${_pulp_defs_var}}")
endforeach()
add_test(NAME gpu-runtime-mode-consistency
    COMMAND ${CMAKE_COMMAND}
        -DHAS_GPU_RUNTIME=${PULP_HAS_GPU_RUNTIME}
        "-DPLATFORM_DEFS=${_pulp_platform_defs}"
        "-DAPI_DEFS=${_pulp_device_api_defs}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_gpu_runtime_mode.cmake)
set_tests_properties(gpu-runtime-mode-consistency
    PROPERTIES LABELS "render;gpu-device;link-closure" TIMEOUT 20)
# One Dawn proc-table installer per image: the device-only consumer always, and
# the GPU-audio provider probe (render plus gpu_audio) where it is configured.
if(APPLE AND Python3_Interpreter_FOUND)
    if(PULP_HAS_GPU_RUNTIME)
        set(_pulp_gpu_topology_mode gpu-runtime)
    else()
        set(_pulp_gpu_topology_mode pulp)
    endif()
    set(_pulp_gpu_topology_binaries "$<TARGET_FILE:pulp-test-gpu-device-only-consumer>")
    if(TARGET pulp-gpu-dawn-shared-io-provider-probe)
        list(APPEND _pulp_gpu_topology_binaries --provider
            "$<TARGET_FILE:pulp-gpu-dawn-shared-io-provider-probe>")
    endif()
    add_test(NAME gpu-runtime-installer-topology
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_gpu_runtime_topology.py
            --mode ${_pulp_gpu_topology_mode} ${_pulp_gpu_topology_binaries})
    set_tests_properties(gpu-runtime-installer-topology
        PROPERTIES LABELS "render;gpu-device;link-closure" TIMEOUT 30)
    unset(_pulp_gpu_topology_mode)
    unset(_pulp_gpu_topology_binaries)
endif()

add_test(NAME gpu-runtime-prefix-record
    COMMAND ${CMAKE_COMMAND}
        -DMODULE=${CMAKE_SOURCE_DIR}/tools/cmake/PulpGpuRuntimeRecord.cmake
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/gpu-runtime-prefix-record
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_gpu_runtime_record.cmake)
set_tests_properties(gpu-runtime-prefix-record PROPERTIES LABELS "render;gpu-device" TIMEOUT 20)
unset(_pulp_platform_defs)
unset(_pulp_device_api_defs)

unset(_pulp_gpu_device_props)
unset(_pulp_gpu_device_props_file)
unset(_pulp_gpu_entry)
unset(_pulp_gpu_key)
unset(_pulp_gpu_target)
unset(_pulp_gpu_property)
unset(_pulp_gpu_value)
