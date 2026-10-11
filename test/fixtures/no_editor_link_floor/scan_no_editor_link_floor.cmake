# Artifact half of the NO_EDITOR link floor. For every editor-less module:
# no GPU-runtime load dependency, and no defined symbol from the view, canvas,
# render, Skia, Dawn or wgpu layers. The control module (same processor, default
# link path) must show each family, or the scanner proves nothing.
if(NOT CONTROL)
    message(FATAL_ERROR "no-editor link floor: CONTROL artifact path is required")
endif()

if(APPLE)
    set(_deps_command /usr/bin/otool -L)
else()
    find_program(_readelf readelf REQUIRED)
    set(_deps_command "${_readelf}" -d)
endif()
find_program(_nm NAMES nm llvm-nm REQUIRED)

# Families of UI/GPU code, matched against `nm -C` (demangled) output by the
# symbol's own qualified name: "<type> pulp::view::...". A processor method that
# merely takes a view type as a parameter (Processor::on_view_opened(View&))
# names pulp::view only after its own name and is not view code.
set(_families
    "view=[A-Za-z] pulp::view::"
    "canvas=[A-Za-z] pulp::canvas::"
    "render=[A-Za-z] pulp::render::"
    "skia=[A-Za-z] (Sk[A-Z][A-Za-z0-9_]*::|skgpu::)"
    "dawn=[A-Za-z] dawn::"
    "wgpu=[A-Za-z] _?wgpu[A-Z]")
include("${CMAKE_CURRENT_LIST_DIR}/no_editor_runtime_deps.cmake")
if(APPLE)
    pulp_no_editor_runtime_deps(_runtime_deps apple)
else()
    pulp_no_editor_runtime_deps(_runtime_deps elf)
endif()

function(_scan artifact out_deps out_symbols)
    if(NOT EXISTS "${artifact}")
        message(FATAL_ERROR "no-editor link floor: artifact does not exist: ${artifact}")
    endif()
    execute_process(COMMAND ${_deps_command} "${artifact}"
        RESULT_VARIABLE _status OUTPUT_VARIABLE _deps ERROR_VARIABLE _error)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "dependency listing failed for ${artifact}: ${_error}")
    endif()
    execute_process(COMMAND "${_nm}" -C "${artifact}"
        RESULT_VARIABLE _status OUTPUT_VARIABLE _symbols ERROR_VARIABLE _error)
    if(NOT _status EQUAL 0 OR _symbols STREQUAL "")
        message(FATAL_ERROR "nm produced no symbols for ${artifact}: ${_error}")
    endif()
    set(${out_deps} "${_deps}" PARENT_SCOPE)
    set(${out_symbols} "${_symbols}" PARENT_SCOPE)
endfunction()

function(_count out symbols regex)
    string(REGEX MATCHALL "${regex}" _hits "${symbols}")
    list(LENGTH _hits _n)
    set(${out} ${_n} PARENT_SCOPE)
endfunction()

# Positive control first. The build says which families its editor stack has
# (CONTROL_FAMILIES) and whether that stack carries a GPU runtime
# (CONTROL_GPU_RUNTIME): a build with PULP_ENABLE_GPU=OFF links view and canvas
# but no render, Skia, Dawn or wgpu code, so its control cannot show those. Every
# family the build has must be seen in the control; the editor-less modules are
# still checked against every family and the runtime pattern below.
if(NOT DEFINED CONTROL_FAMILIES OR NOT DEFINED CONTROL_GPU_RUNTIME)
    message(FATAL_ERROR "no-editor link floor: CONTROL_FAMILIES and CONTROL_GPU_RUNTIME are required")
endif()
string(REPLACE "|" ";" CONTROL_FAMILIES "${CONTROL_FAMILIES}")
_scan("${CONTROL}" _control_deps _control_symbols)
if(CONTROL_GPU_RUNTIME AND NOT _control_deps MATCHES "${_runtime_deps}")
    message(FATAL_ERROR
        "control module has no GPU-runtime dependency; the dependency check is blind:\n${_control_deps}")
endif()
if(NOT CONTROL_GPU_RUNTIME)
    # No GPU runtime to look for, but the dependency listing must still be
    # proven to work: every module loads the platform C/C++ runtime.
    if(NOT _control_deps MATCHES "libSystem|libc\\.so|libc\\+\\+|libstdc\\+\\+")
        message(FATAL_ERROR
            "control module lists no C/C++ runtime dependency; the dependency scan is blind:\n${_control_deps}")
    endif()
    message(STATUS "control GPU runtime: not built (PULP_ENABLE_GPU=OFF); dependency listing proven by the C/C++ runtime")
endif()
foreach(_family IN LISTS _families)
    string(REPLACE "=" ";" _pair "${_family}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _regex)
    if(NOT _name IN_LIST CONTROL_FAMILIES)
        message(STATUS "control ${_name}: not built in this configuration")
        continue()
    endif()
    _count(_n "${_control_symbols}" "${_regex}")
    if(_n EQUAL 0)
        message(FATAL_ERROR
            "control module shows no '${_name}' symbols; the symbol check is blind for that family")
    endif()
    message(STATUS "control ${_name}: ${_n}")
endforeach()

set(_checked 0)
foreach(_fmt VST3 CLAP AU LV2)
    if(NOT ${_fmt})
        continue()
    endif()
    _scan("${${_fmt}}" _deps _symbols)
    if(_deps MATCHES "${_runtime_deps}")
        message(FATAL_ERROR "NO_EDITOR ${_fmt} module links a GPU runtime: ${${_fmt}}\n${_deps}")
    endif()
    foreach(_family IN LISTS _families)
        string(REPLACE "=" ";" _pair "${_family}")
        list(GET _pair 0 _name)
        list(GET _pair 1 _regex)
        _count(_n "${_symbols}" "${_regex}")
        if(NOT _n EQUAL 0)
            message(FATAL_ERROR
                "NO_EDITOR ${_fmt} module contains ${_n} '${_name}' symbols: ${${_fmt}}")
        endif()
    endforeach()
    # Runtimes are staged beside the module. That directory belongs to this
    # module only inside a bundle (<name>.clap/Contents/MacOS); a flat output
    # directory such as build/VST3 on Linux is shared with every other plug-in,
    # whose staged runtime says nothing about this one. There the dependency
    # scan above is the check.
    get_filename_component(_dir "${${_fmt}}" DIRECTORY)
    if(_dir MATCHES "\\.(vst3|clap|component|lv2)/Contents/")
        file(GLOB _staged "${_dir}/libwgpu_native*")
        if(_staged)
            message(FATAL_ERROR "NO_EDITOR ${_fmt} module staged a GPU runtime: ${_staged}")
        endif()
    endif()
    file(SIZE "${${_fmt}}" _size)
    message(STATUS "NO_EDITOR ${_fmt}: clean (${_size} bytes)")
    math(EXPR _checked "${_checked} + 1")
endforeach()
if(_checked EQUAL 0)
    message(FATAL_ERROR "no NO_EDITOR module was passed to the scanner")
endif()
