# Asserts a built image embeds the UI_SCRIPT path it registers at load time.
#   cmake -DIMAGE=<binary> -DSCRIPT=<path> -P check_image_registers_script.cmake
foreach(_var IMAGE SCRIPT)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()
file(TO_CMAKE_PATH "${SCRIPT}" _script)
file(STRINGS "${IMAGE}" _hits REGEX "main\\.js")
list(FIND _hits "${_script}" _index)
if(_index EQUAL -1)
    message(FATAL_ERROR "${IMAGE} does not register ${_script}; strings mentioning main.js: ${_hits}")
endif()
message(STATUS "${IMAGE} registers ${_script}")
