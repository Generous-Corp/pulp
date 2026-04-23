if(NOT DEFINED INPUT_PNG OR NOT DEFINED OUTPUT_ICNS)
    message(FATAL_ERROR "PulpGenerateMacIcns.cmake requires INPUT_PNG and OUTPUT_ICNS")
endif()
if(NOT EXISTS "${INPUT_PNG}")
    message(FATAL_ERROR "Input icon PNG does not exist: ${INPUT_PNG}")
endif()
if(NOT DEFINED SIPS_EXECUTABLE OR NOT EXISTS "${SIPS_EXECUTABLE}")
    message(FATAL_ERROR "Missing SIPS_EXECUTABLE for PulpGenerateMacIcns.cmake")
endif()
if(NOT DEFINED ICONUTIL_EXECUTABLE OR NOT EXISTS "${ICONUTIL_EXECUTABLE}")
    message(FATAL_ERROR "Missing ICONUTIL_EXECUTABLE for PulpGenerateMacIcns.cmake")
endif()

get_filename_component(_out_dir "${OUTPUT_ICNS}" DIRECTORY)
set(_iconset_dir "${_out_dir}/AppIcon.iconset")

file(REMOVE_RECURSE "${_iconset_dir}")
file(MAKE_DIRECTORY "${_iconset_dir}")
file(MAKE_DIRECTORY "${_out_dir}")

function(_pulp_make_icon size filename)
    execute_process(
        COMMAND "${SIPS_EXECUTABLE}" -s format png -z "${size}" "${size}"
            "${INPUT_PNG}" --out "${_iconset_dir}/${filename}"
        RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "sips failed while generating ${filename}")
    endif()
endfunction()

_pulp_make_icon(16   "icon_16x16.png")
_pulp_make_icon(32   "icon_16x16@2x.png")
_pulp_make_icon(32   "icon_32x32.png")
_pulp_make_icon(64   "icon_32x32@2x.png")
_pulp_make_icon(128  "icon_128x128.png")
_pulp_make_icon(256  "icon_128x128@2x.png")
_pulp_make_icon(256  "icon_256x256.png")
_pulp_make_icon(512  "icon_256x256@2x.png")
_pulp_make_icon(512  "icon_512x512.png")
_pulp_make_icon(1024 "icon_512x512@2x.png")

execute_process(
    COMMAND "${ICONUTIL_EXECUTABLE}" -c icns "${_iconset_dir}" -o "${OUTPUT_ICNS}"
    RESULT_VARIABLE _iconutil_result
)
if(NOT _iconutil_result EQUAL 0)
    message(FATAL_ERROR "iconutil failed while generating ${OUTPUT_ICNS}")
endif()
