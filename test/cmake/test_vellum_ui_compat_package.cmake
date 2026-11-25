# The installed package config defines Vellum-backed Pulp::view, Pulp::canvas,
# Pulp::render, Pulp::view-native and Pulp::design-import for each of those
# names the SDK does not export from Pulp's own code.
#
# Each scenario fabricates an installed Pulp prefix whose PulpConfig.cmake is
# spliced from the real template: the Vellum find block and the compatibility
# block are copied verbatim from tools/cmake/PulpConfig.cmake.in, and the module
# is copied to where the install rules put it. A consumer then runs
# find_package(Pulp CONFIG REQUIRED) against that prefix.
#
#   wiring       the template includes the module after the Skia attach and
#                before the PULP_HAS_* facts, and the install rules ship it.
#   shape        stand-in Vellum targets: all five names are defined with the
#                expected link sets, C++20 and lower-case aliases.
#   shadowed     names Pulp already exports keep Pulp's own target.
#   missing      a name whose Vellum backing target is absent stays undefined.
#   no-bridge    an SDK without the Vellum bridge defines nothing.
#   no-vellum    the bridge is on but Vellum cannot be found: the consumer
#                fails at configure, naming the missing Vellum-backed name.
#   real         a real installed Vellum SDK: the consumer under
#                tools/validation/sdk-smoke/vellum-ui-compat builds one
#                executable per name and runs each. Needs VELLUM_DIR (the
#                directory holding VellumConfig.cmake); prints SKIP without it.
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR OR NOT DEFINED SCENARIO)
    message(FATAL_ERROR "PULP_SOURCE_DIR, FIXTURE_DIR and SCENARIO are required")
endif()

set(_template "${PULP_SOURCE_DIR}/tools/cmake/PulpConfig.cmake.in")
set(_module "${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumUiCompat.cmake")
file(READ "${_template}" _config)

# Returns the block of the template that starts at `first_line` and ends at the
# first line that is exactly "endif()" after it.
function(_template_block first_line out)
    string(FIND "${_config}" "${first_line}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "PulpConfig.cmake.in no longer contains '${first_line}'; update this test")
    endif()
    string(SUBSTRING "${_config}" ${_at} -1 _rest)
    string(FIND "${_rest}" "\nendif()\n" _end)
    if(_end LESS 0)
        message(FATAL_ERROR "no endif() closes the block at '${first_line}'")
    endif()
    math(EXPR _len "${_end} + 9")
    string(SUBSTRING "${_rest}" 0 ${_len} _block)
    set(${out} "${_block}" PARENT_SCOPE)
endfunction()

set(_find_head "set(PULP_SDK_HAS_VELLUM_BRIDGE \"@PULP_SDK_HAS_VELLUM_BRIDGE@\")")
set(_compat_head "set(PULP_SDK_VELLUM_UI_COMPAT_TARGETS \"\")")
_template_block("${_find_head}" _find_block)
_template_block("${_compat_head}" _compat_block)

set(_names "view;canvas;render;view-native;design-import")

# Writes <prefix>/lib/cmake/Pulp/{PulpConfig,PulpVellumUiCompat}.cmake.
# `bridge` substitutes @PULP_SDK_HAS_VELLUM_BRIDGE@; `prelude` stands in for
# PulpTargets.cmake and runs between the Vellum find and the compatibility block.
function(_write_prefix prefix bridge prelude)
    set(_dir "${prefix}/lib/cmake/Pulp")
    file(REMOVE_RECURSE "${prefix}")
    file(MAKE_DIRECTORY "${_dir}")
    file(COPY "${_module}" DESTINATION "${_dir}")
    string(REPLACE "@PULP_SDK_HAS_VELLUM_BRIDGE@" "${bridge}" _find "${_find_block}")
    file(WRITE "${_dir}/PulpConfig.cmake"
        "get_filename_component(PACKAGE_PREFIX_DIR \"\${CMAKE_CURRENT_LIST_DIR}/../../..\" ABSOLUTE)\n"
        "${_find}\n${prelude}\n${_compat_block}\n"
        "set(Pulp_FOUND TRUE)\n")
endfunction()

# Package registries stay out of every consumer, so only the fabricated prefix
# (or, for the real scenario, a checked location) supplies Pulp and Vellum.
set(_isolation
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
set(_clean_env "${CMAKE_COMMAND}" -E env --unset=Vellum_DIR --unset=Pulp_DIR
    --unset=CMAKE_PREFIX_PATH)

# Configures a language-less consumer that prints facts about each name.
function(_inspect name bridge prelude out)
    set(_root "${FIXTURE_DIR}/${name}")
    file(REMOVE_RECURSE "${_root}")
    _write_prefix("${_root}/prefix" "${bridge}" "${prelude}")
    set(_src "${_root}/src")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(VellumUiCompatInspect NONE)\n"
        "find_package(Pulp CONFIG REQUIRED)\n"
        "message(STATUS \"COMPAT=[\${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}]\")\n"
        "message(STATUS \"VELLUM_FOUND=[\${Vellum_FOUND}]\")\n"
        "foreach(n IN ITEMS ${_names})\n"
        "  if(TARGET Pulp::\${n})\n"
        "    get_target_property(l Pulp::\${n} INTERFACE_LINK_LIBRARIES)\n"
        "    get_target_property(f Pulp::\${n} INTERFACE_COMPILE_FEATURES)\n"
        "    set(a NO)\n"
        "    if(TARGET pulp::\${n})\n"
        "      set(a YES)\n"
        "    endif()\n"
        "    message(STATUS \"T[\${n}]=[\${l}] F[\${n}]=[\${f}] A[\${n}]=[\${a}]\")\n"
        "  else()\n"
        "    message(STATUS \"T[\${n}]=[ABSENT]\")\n"
        "  endif()\n"
        "endforeach()\n")
    execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" -S "${_src}" -B "${_root}/build"
            -DPulp_DIR=${_root}/prefix/lib/cmake/Pulp ${_isolation}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name}: inspect configure failed:\n${_out}${_err}")
    endif()
    if(NOT _out MATCHES "COMPAT=\\[")
        message(FATAL_ERROR "${name}: the consumer printed nothing, so the probe reads nothing:\n${_out}")
    endif()
    set(${out} "${_out}" PARENT_SCOPE)
endfunction()

function(_expect out pattern what)
    string(REGEX MATCH "${pattern}" _m "${out}")
    if(NOT _m)
        message(FATAL_ERROR "${SCENARIO}: expected ${what} (/${pattern}/) in:\n${out}")
    endif()
endfunction()

# Stand-ins for Vellum's mirror targets and Pulp's own bridge target. INTERFACE
# targets need no files, so these scenarios run on every host.
set(_vellum_standins "set(Vellum_FOUND TRUE)\n")
foreach(_t MirrorCanvas MirrorRender_2d MirrorView_core MirrorDesign_import MirrorView_script)
    string(APPEND _vellum_standins "add_library(Vellum::${_t} INTERFACE IMPORTED)\n")
endforeach()
string(APPEND _vellum_standins "add_library(Pulp::vellum INTERFACE IMPORTED)\n")

if(SCENARIO STREQUAL "wiring")
    # Template order: Skia attach < module include < derived facts.
    string(FIND "${_config}" "pulp_attach_fontconfig_to_canvas(Pulp::canvas)" _skia_at)
    string(FIND "${_config}" "include(\"\${CMAKE_CURRENT_LIST_DIR}/PulpTargets.cmake\")" _targets_at)
    string(FIND "${_config}" "include(\"\${CMAKE_CURRENT_LIST_DIR}/PulpVellumUiCompat.cmake\")" _include_at)
    string(FIND "${_config}" "pulp_define_vellum_ui_compat_targets()" _call_at)
    string(FIND "${_config}" "set(PULP_ENABLE_GPU FALSE)" _gpu_at)
    string(FIND "${_config}" "foreach(_pulp_component IN LISTS Pulp_FIND_COMPONENTS)" _components_at)
    foreach(_v _skia_at _targets_at _include_at _call_at _gpu_at _components_at)
        if(${_v} LESS 0)
            message(FATAL_ERROR "wiring: PulpConfig.cmake.in lacks the anchor ${_v}; update this test")
        endif()
    endforeach()
    if(NOT (_targets_at LESS _skia_at AND _skia_at LESS _include_at
            AND _include_at LESS _call_at AND _call_at LESS _gpu_at
            AND _gpu_at LESS _components_at))
        message(FATAL_ERROR "wiring: the compatibility module must load after PulpTargets.cmake "
            "and the Skia attach, and before PULP_ENABLE_GPU and the component check")
    endif()
    if(NOT _compat_block MATCHES "if\\(PULP_SDK_HAS_VELLUM_BRIDGE AND Vellum_FOUND\\)")
        message(FATAL_ERROR "wiring: the compatibility block is not gated on the bridge and Vellum:\n${_compat_block}")
    endif()
    file(READ "${PULP_SOURCE_DIR}/tools/cmake/PulpInstallRules.cmake" _rules)
    string(FIND "${_rules}" "integrations/pulp-vellum/cmake/PulpVellumUiCompat.cmake" _installed)
    if(_installed LESS 0)
        message(FATAL_ERROR "wiring: PulpInstallRules.cmake does not install PulpVellumUiCompat.cmake")
    endif()

elseif(SCENARIO STREQUAL "shape")
    _inspect(shape ON "${_vellum_standins}" _out)
    _expect("${_out}" "COMPAT=\\[canvas;render;design-import;view-native;view\\]" "all five names")
    _expect("${_out}" "T\\[canvas\\]=\\[Vellum::MirrorCanvas\\] F\\[canvas\\]=\\[cxx_std_20\\] A\\[canvas\\]=\\[YES\\]" "canvas")
    _expect("${_out}" "T\\[render\\]=\\[Vellum::MirrorRender_2d\\] F\\[render\\]=\\[cxx_std_20\\] A\\[render\\]=\\[YES\\]" "render")
    _expect("${_out}" "T\\[design-import\\]=\\[Vellum::MirrorDesign_import\\] F\\[design-import\\]=\\[cxx_std_20\\] A\\[design-import\\]=\\[YES\\]" "design-import")
    _expect("${_out}" "T\\[view-native\\]=\\[Vellum::MirrorView_core;Pulp::vellum\\] F\\[view-native\\]=\\[cxx_std_20\\] A\\[view-native\\]=\\[YES\\]" "view-native")
    _expect("${_out}" "T\\[view\\]=\\[Vellum::MirrorView_core;Vellum::MirrorDesign_import;Vellum::MirrorView_script;Pulp::vellum\\] F\\[view\\]=\\[cxx_std_20\\] A\\[view\\]=\\[YES\\]" "view")

elseif(SCENARIO STREQUAL "shadowed")
    _inspect(shadowed ON
        "${_vellum_standins}add_library(Pulp::canvas INTERFACE IMPORTED)\nset_property(TARGET Pulp::canvas PROPERTY INTERFACE_LINK_LIBRARIES pulp_own_canvas)\nadd_library(Pulp::view INTERFACE IMPORTED)\nset_property(TARGET Pulp::view PROPERTY INTERFACE_LINK_LIBRARIES pulp_own_view)\n"
        _out)
    _expect("${_out}" "COMPAT=\\[render;design-import;view-native\\]" "only the names Pulp does not export")
    _expect("${_out}" "T\\[canvas\\]=\\[pulp_own_canvas\\]" "Pulp's own canvas untouched")
    _expect("${_out}" "T\\[view\\]=\\[pulp_own_view\\]" "Pulp's own view untouched")

elseif(SCENARIO STREQUAL "missing")
    string(REPLACE "add_library(Vellum::MirrorCanvas INTERFACE IMPORTED)\n" "" _partial "${_vellum_standins}")
    _inspect(missing ON "${_partial}" _out)
    _expect("${_out}" "COMPAT=\\[render;design-import;view-native;view\\]" "canvas left out")
    _expect("${_out}" "T\\[canvas\\]=\\[ABSENT\\]" "no empty canvas target")

elseif(SCENARIO STREQUAL "no-bridge")
    _inspect(no-bridge OFF "" _out)
    _expect("${_out}" "COMPAT=\\[\\]" "no names")
    _expect("${_out}" "VELLUM_FOUND=\\[\\]" "Vellum never searched")
    _expect("${_out}" "T\\[view\\]=\\[ABSENT\\]" "no view target")

elseif(SCENARIO STREQUAL "no-vellum" OR SCENARIO STREQUAL "real")
    set(_root "${FIXTURE_DIR}/${SCENARIO}")
    file(REMOVE_RECURSE "${_root}")
    _write_prefix("${_root}/prefix" ON "")
    if(SCENARIO STREQUAL "real")
        if(NOT VELLUM_DIR OR NOT EXISTS "${VELLUM_DIR}/VellumConfig.cmake")
            message(STATUS "SKIP: no installed Vellum SDK (pass -DVELLUM_DIR=<dir with VellumConfig.cmake>)")
            return()
        endif()
        # Stage Vellum where a release stages it, so the spliced find block's
        # own hint resolves it.
        get_filename_component(_vellum_sdk "${VELLUM_DIR}/../../.." ABSOLUTE)
        file(MAKE_DIRECTORY "${_root}/prefix/share/pulp/vellum-sdk/0.0.0")
        file(CREATE_LINK "${_vellum_sdk}" "${_root}/prefix/share/pulp/vellum-sdk/0.0.0/sdk" SYMBOLIC)
    endif()
    set(_generator)
    find_program(_ninja ninja)
    if(_ninja)
        set(_generator -G Ninja)
    endif()
    set(_no_vellum)
    if(SCENARIO STREQUAL "no-vellum")
        # The consumer's environment cannot provide Vellum at all.
        set(_no_vellum -DCMAKE_DISABLE_FIND_PACKAGE_Vellum=ON)
    endif()
    execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" ${_generator}
            -S "${PULP_SOURCE_DIR}/tools/validation/sdk-smoke/vellum-ui-compat"
            -B "${_root}/build" -DCMAKE_BUILD_TYPE=Release
            -DPulp_DIR=${_root}/prefix/lib/cmake/Pulp ${_isolation} ${_no_vellum}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(SCENARIO STREQUAL "no-vellum")
        if(_rc EQUAL 0)
            message(FATAL_ERROR "no-vellum: the consumer configured without Vellum:\n${_out}${_err}")
        endif()
        _expect("${_out}${_err}" "Pulp::view is not Vellum-backed in this SDK" "a configure error naming the first name")
        message(STATUS "no-vellum: configure refused as expected")
        return()
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: consumer configure failed:\n${_out}${_err}")
    endif()
    # A Vellum found anywhere but the staged copy would make this a test of
    # some other install.
    file(STRINGS "${_root}/build/CMakeCache.txt" _vellum_dir_line REGEX "^Vellum_DIR:")
    if(NOT _vellum_dir_line MATCHES "^Vellum_DIR:PATH=${_root}/prefix/share/pulp/vellum-sdk/")
        message(FATAL_ERROR "real: Vellum resolved outside the staged SDK: '${_vellum_dir_line}'")
    endif()
    # A small literal share unless the caller's lease granted one.
    set(_jobs 4)
    if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
        set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" -j ${_jobs}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: consumer build failed:\n${_out}${_err}")
    endif()
    # The POST_BUILD steps ran each probe; their lines prove they executed.
    string(REGEX MATCHALL "Pulp::[a-z-]+ -> [^\n]*" _ran "${_out}")
    foreach(_line IN LISTS _ran)
        message(STATUS "real: ${_line}")
    endforeach()
    _expect("${_out}" "Pulp::canvas -> vellum::canvas::Color r=1.00" "the canvas probe ran")
    _expect("${_out}" "Pulp::render -> vellum::render::AtlasPacker::allocate ok" "the render probe ran")
    _expect("${_out}" "Pulp::view-native -> vellum::view::PluginViewHost::create linked" "the native probe ran")
    _expect("${_out}" "Pulp::design-import -> vellum::view::parse_designmd_yaml primary=#336699" "the importer ran")
    _expect("${_out}" "Pulp::view -> vellum::view::parse_designmd_yaml primary=#336699" "the umbrella ran")
    message(STATUS "real: five Vellum-backed names built and ran")

else()
    message(FATAL_ERROR "unknown SCENARIO '${SCENARIO}'")
endif()
message(STATUS "${SCENARIO}: ok")
