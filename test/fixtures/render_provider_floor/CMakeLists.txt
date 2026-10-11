# Render provider floor: a Pulp plug-in image built with Skia's static Dawn
# carries exactly that Dawn and loads no second WebGPU implementation. In
# particular it must not load libwgpu_native: dyld shares one copy of it per
# process by install name, so two Pulp plug-ins in one host would otherwise run
# on whichever copy loaded first. The plug-in images are the always-built
# bundle-name-quoting fixture's formats, which reach Dawn through the format
# and view layers.
if(NOT APPLE OR PULP_IOS OR NOT PULP_HAS_SKIA
   OR DAWN_LIBRARY STREQUAL "DAWN_LIBRARY-NOTFOUND")
    return()
endif()

set(_provider_floor_images "")
foreach(_fmt CLAP VST3 AU)
    if(TARGET PulpBundleNameQuoting_${_fmt})
        list(APPEND _provider_floor_images "$<TARGET_FILE:PulpBundleNameQuoting_${_fmt}>")
    endif()
endforeach()
if(NOT _provider_floor_images)
    return()
endif()

# Positive control: an image that really links wgpu-native, so the absence
# assertion is shown to see a libwgpu_native load command when one exists.
# Without a wgpu-native runtime in the build there is nothing that could load.
set(_provider_floor_control_args "")
if(TARGET webgpu)
    add_executable(pulp-render-provider-floor-wgpu-control wgpu_control.cpp)
    target_link_libraries(pulp-render-provider-floor-wgpu-control PRIVATE webgpu)
    set(_provider_floor_control_args
        "-DWGPU_POSITIVE_CONTROL=$<TARGET_FILE:pulp-render-provider-floor-wgpu-control>")
endif()

string(JOIN "|" _provider_floor_image_list ${_provider_floor_images})
add_test(NAME render-provider-floor-plugin-images
    COMMAND "${CMAKE_COMMAND}"
        "-DIMAGES=${_provider_floor_image_list}"
        ${_provider_floor_control_args}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scan_render_provider_floor.cmake")
set_tests_properties(render-provider-floor-plugin-images PROPERTIES
    LABELS "render;link-contract;artifact" TIMEOUT 60)
