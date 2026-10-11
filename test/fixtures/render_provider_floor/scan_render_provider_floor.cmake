# Asserts each plug-in image carries Skia's Dawn and neither loads nor stages
# libwgpu_native. IMAGES is a "|"-separated list of plug-in binaries;
# WGPU_POSITIVE_CONTROL, when given, is an image that really links wgpu-native.
if(NOT IMAGES)
    message(FATAL_ERROR "render provider floor: no plug-in images given")
endif()
string(REPLACE "|" ";" _images "${IMAGES}")

function(_tool out)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE _status OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "render provider floor: ${ARGN} failed: ${_error}")
    endif()
    set(${out} "${_output}" PARENT_SCOPE)
endfunction()

if(WGPU_POSITIVE_CONTROL)
    _tool(_control_deps /usr/bin/otool -L "${WGPU_POSITIVE_CONTROL}")
    if(NOT _control_deps MATCHES "libwgpu_native")
        message(FATAL_ERROR
            "positive control does not load libwgpu_native; the absence check cannot see one:\n${_control_deps}")
    endif()
endif()

foreach(_image IN LISTS _images)
    if(NOT EXISTS "${_image}")
        message(FATAL_ERROR "render provider floor: image does not exist: ${_image}")
    endif()
    _tool(_symbols /usr/bin/nm -U "${_image}")
    # An image with no Dawn would pass the absence check for the wrong reason.
    if(NOT _symbols MATCHES "__ZN4dawn6native")
        message(FATAL_ERROR "plug-in image does not carry Skia's Dawn (dawn::native): ${_image}")
    endif()
    _tool(_deps /usr/bin/otool -L "${_image}")
    if(_deps MATCHES "libwgpu_native")
        message(FATAL_ERROR
            "plug-in image loads libwgpu_native beside Skia's Dawn: ${_image}\n${_deps}")
    endif()
    get_filename_component(_dir "${_image}" DIRECTORY)
    file(GLOB _staged "${_dir}/*wgpu_native*")
    if(_staged)
        message(FATAL_ERROR "plug-in bundle stages libwgpu_native: ${_staged}")
    endif()
endforeach()
list(LENGTH _images _count)
message(STATUS "render provider floor: ${_count} plug-in image(s) carry Dawn and load no wgpu-native")
