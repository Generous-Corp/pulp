# The installed package config defines Vellum-backed Pulp::view, Pulp::canvas,
# Pulp::render, Pulp::view-native and Pulp::design-import in an SDK built with
# PULP_SDK_UI_FROM_VELLUM, and defines nothing in an SDK that builds Pulp's own
# UI, fully or partly.
#
# Each scenario fabricates an installed Pulp prefix whose PulpConfig.cmake is
# spliced from the real template: the Vellum find block and the compatibility
# block are copied verbatim from tools/cmake/PulpConfig.cmake.in with their
# @VAR@ values substituted, and the module is copied to where the install rules
# put it. A consumer then runs find_package(Pulp CONFIG REQUIRED).
#
#   wiring        the template includes the module after the Skia attach and
#                 before the PULP_HAS_* facts, and the module is installed by
#                 the unconditional install(FILES) that ships PulpConfig.cmake.
#   shape         stand-in Vellum targets: all five names are defined with the
#                 expected link sets, C++20 and lower-case aliases.
#   pulp-ui       an SDK that exports its own UI defines nothing.
#   partial-ui    a bridge SDK built with GPU and design import off (no
#                 Pulp::render, no Pulp::design-import) defines nothing.
#   view-core     PULP_SDK_UI_FROM_VELLUM on while Pulp::view-core is exported
#                 still defines nothing.
#   missing       a name whose Vellum backing target is absent stays undefined.
#   twice         a second find_package(Pulp) reports the same list.
#   no-bridge     an SDK without the Vellum bridge defines nothing.
#   no-vellum     Vellum cannot be found: the config warns and names the
#                 missing targets, and the consumer refuses to configure.
#   real          a real installed Vellum SDK: the consumer under
#                 tools/validation/sdk-smoke/vellum-ui-compat builds one
#                 executable per name and runs each.
#   wrong-version a real Vellum whose version is not the one recorded: it is
#                 not found, the config warns, and the consumer refuses.
#   gpu-guard     stand-ins: a binary that links a Vellum-backed name and
#                 Pulp's static gpu_runtime (Pulp::gpu-device, Pulp::gpu-audio,
#                 Pulp::host, VellumGpuRuntime::Device), directly, through a
#                 static library's private link, through a subdirectory or
#                 through an alias, is refused at configure; either side alone,
#                 a static library alone and an SDK that builds Pulp's own UI
#                 configure.
#   gpu-owner-real a real Vellum SDK and a real VellumGpuRuntime package: the
#                 Vellum-backed consumer's executable defines no gpu_runtime::
#                 or wgpu symbol (the provider owns them), a control that links
#                 the provider and the static device directly, bypassing the
#                 Pulp names, defines both (two owners, which nm can see), and
#                 the same pair through the Pulp names is refused.
#
# real and wrong-version need VELLUM_DIR (the directory holding
# VellumConfig.cmake). Without it they print SKIP, unless REQUIRE_VELLUM is on,
# when they fail: the lane that registers them with a Vellum SDK must run them.
# gpu-owner-real also needs GPU_RUNTIME_DIR (the directory holding
# VellumGpuRuntimeConfig.cmake) and GPU_RUNTIME_SKIA_DIR (the locked Skia/Dawn
# build it checks Dawn against), and skips without them.
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
set(_compat_head "set(PULP_SDK_UI_FROM_VELLUM \"@PULP_SDK_UI_FROM_VELLUM@\")")
_template_block("${_find_head}" _find_block)
_template_block("${_compat_head}" _compat_block)

set(_names "view;canvas;render;view-native;design-import")

# Writes <prefix>/lib/cmake/Pulp/{PulpConfig,PulpVellumUiCompat}.cmake.
# `bridge`, `ui` and `version` substitute @PULP_SDK_HAS_VELLUM_BRIDGE@,
# @PULP_SDK_UI_FROM_VELLUM@ and @PULP_SDK_VELLUM_VERSION@; `prelude` stands in
# for PulpTargets.cmake and runs between the Vellum find and the compatibility
# block.
function(_write_prefix prefix bridge ui version prelude)
    set(_dir "${prefix}/lib/cmake/Pulp")
    file(REMOVE_RECURSE "${prefix}")
    file(MAKE_DIRECTORY "${_dir}")
    file(COPY "${_module}" DESTINATION "${_dir}")
    set(_body "${_find_block}\n${prelude}\n${_compat_block}")
    string(REPLACE "@PULP_SDK_HAS_VELLUM_BRIDGE@" "${bridge}" _body "${_body}")
    string(REPLACE "@PULP_SDK_UI_FROM_VELLUM@" "${ui}" _body "${_body}")
    string(REPLACE "@PULP_SDK_VELLUM_VERSION@" "${version}" _body "${_body}")
    if(_body MATCHES "@[A-Z_]+@")
        message(FATAL_ERROR "an @VAR@ in the spliced blocks is not substituted: ${CMAKE_MATCH_0}")
    endif()
    file(WRITE "${_dir}/PulpConfig.cmake"
        "get_filename_component(PACKAGE_PREFIX_DIR \"\${CMAKE_CURRENT_LIST_DIR}/../../..\" ABSOLUTE)\n"
        "${_body}\n"
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
# `finds` is how many times the consumer calls find_package(Pulp).
function(_inspect name bridge ui prelude finds out)
    set(_root "${FIXTURE_DIR}/${name}")
    file(REMOVE_RECURSE "${_root}")
    _write_prefix("${_root}/prefix" "${bridge}" "${ui}" "" "${prelude}")
    set(_src "${_root}/src")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(VellumUiCompatInspect NONE)\n"
        "foreach(i RANGE 1 ${finds})\n"
        "  find_package(Pulp CONFIG REQUIRED)\n"
        "  message(STATUS \"FIND\${i} COMPAT=[\${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}]\")\n"
        "endforeach()\n"
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
# targets need no files, so these scenarios run on every host. Each is created
# once, so a second find_package(Pulp) can load the prelude again.
function(_standins out)
    set(_text "set(Vellum_FOUND TRUE)\n")
    foreach(_t IN LISTS ARGN)
        string(APPEND _text "if(NOT TARGET ${_t})\n  add_library(${_t} INTERFACE IMPORTED)\nendif()\n")
    endforeach()
    set(${out} "${_text}" PARENT_SCOPE)
endfunction()
set(_vellum_targets Vellum::MirrorCanvas Vellum::MirrorRender_2d Vellum::MirrorView_core
    Vellum::MirrorDesign_import Vellum::MirrorView_script Pulp::vellum)
_standins(_vellum_standins ${_vellum_targets})
# Pulp's own UI targets, each linking a marker so the scenario can tell them apart.
function(_pulp_own out)
    set(_text "")
    foreach(_n IN LISTS ARGN)
        string(APPEND _text "if(NOT TARGET Pulp::${_n})\n  add_library(Pulp::${_n} INTERFACE IMPORTED)\n"
            "  set_property(TARGET Pulp::${_n} PROPERTY INTERFACE_LINK_LIBRARIES pulp_own_${_n})\nendif()\n")
    endforeach()
    set(${out} "${_text}" PARENT_SCOPE)
endfunction()

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
    if(NOT _compat_block MATCHES "\nif\\(PULP_SDK_UI_FROM_VELLUM AND PULP_SDK_HAS_VELLUM_BRIDGE\\)\n    if\\(Vellum_FOUND\\)\n        include\\(")
        message(FATAL_ERROR "wiring: the module is not gated on PULP_SDK_UI_FROM_VELLUM, the bridge and Vellum_FOUND:\n${_compat_block}")
    endif()
    if(NOT _find_block MATCHES "CONFIG QUIET HINTS")
        message(FATAL_ERROR "wiring: the Vellum find must use HINTS so the staged copy wins:\n${_find_block}")
    endif()

    # The module rides in the install(FILES) that ships PulpConfig.cmake, and
    # that command is unconditional: every SDK that installs the config
    # installs the module, on every platform.
    file(READ "${PULP_SOURCE_DIR}/tools/cmake/PulpInstallRules.cmake" _rules)
    set(_config_install_head "\ninstall(FILES\n    \"\${CMAKE_BINARY_DIR}/PulpConfig.cmake\"\n")
    string(FIND "${_rules}" "${_config_install_head}" _install_at)
    if(_install_at LESS 0)
        message(FATAL_ERROR "wiring: no top-level install(FILES) starting with PulpConfig.cmake in PulpInstallRules.cmake; update this test")
    endif()
    string(SUBSTRING "${_rules}" ${_install_at} -1 _install)
    string(FIND "${_install}" "\n)" _install_end)
    string(SUBSTRING "${_install}" 0 ${_install_end} _install)
    string(REGEX REPLACE "#[^\n]*" "" _install "${_install}")
    string(FIND "${_install}" "\"\${CMAKE_CURRENT_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumUiCompat.cmake\"" _module_at)
    if(_module_at LESS 0)
        message(FATAL_ERROR "wiring: PulpVellumUiCompat.cmake is not installed by the install(FILES) that ships PulpConfig.cmake")
    endif()
    string(SUBSTRING "${_rules}" 0 ${_install_at} _before)
    string(REGEX REPLACE "#[^\n]*" "" _before "${_before}")
    string(REGEX MATCHALL "\n[ \t]*(if|foreach|while|function|macro|block)[ \t]*\\(" _opens "${_before}")
    string(REGEX MATCHALL "\n[ \t]*end(if|foreach|while|function|macro|block)[ \t]*\\(" _closes "${_before}")
    list(LENGTH _opens _n_open)
    list(LENGTH _closes _n_close)
    if(NOT _n_open EQUAL _n_close)
        message(FATAL_ERROR "wiring: the install(FILES) that ships PulpConfig.cmake sits inside a block "
            "(${_n_open} opened, ${_n_close} closed before it); it must be unconditional")
    endif()

elseif(SCENARIO STREQUAL "shape")
    _inspect(shape ON ON "${_vellum_standins}" 1 _out)
    _expect("${_out}" "COMPAT=\\[canvas;render;design-import;view-native;view\\]" "all five names")
    _expect("${_out}" "T\\[canvas\\]=\\[Vellum::MirrorCanvas\\] F\\[canvas\\]=\\[cxx_std_20\\] A\\[canvas\\]=\\[YES\\]" "canvas")
    _expect("${_out}" "T\\[render\\]=\\[Vellum::MirrorRender_2d\\] F\\[render\\]=\\[cxx_std_20\\] A\\[render\\]=\\[YES\\]" "render")
    _expect("${_out}" "T\\[design-import\\]=\\[Vellum::MirrorDesign_import\\] F\\[design-import\\]=\\[cxx_std_20\\] A\\[design-import\\]=\\[YES\\]" "design-import")
    _expect("${_out}" "T\\[view-native\\]=\\[Vellum::MirrorView_core;Pulp::vellum\\] F\\[view-native\\]=\\[cxx_std_20\\] A\\[view-native\\]=\\[YES\\]" "view-native")
    _expect("${_out}" "T\\[view\\]=\\[Vellum::MirrorView_core;Vellum::MirrorDesign_import;Vellum::MirrorView_script;Pulp::vellum\\] F\\[view\\]=\\[cxx_std_20\\] A\\[view\\]=\\[YES\\]" "view")

elseif(SCENARIO STREQUAL "pulp-ui")
    # Today's SDK: the flag is off and Pulp exports its own UI.
    _pulp_own(_own view-core view canvas render view-native design-import)
    _inspect(pulp-ui ON OFF "${_vellum_standins}${_own}" 1 _out)
    _expect("${_out}" "COMPAT=\\[\\]" "no Vellum-backed names")
    foreach(_n view canvas render view-native design-import)
        _expect("${_out}" "T\\[${_n}\\]=\\[pulp_own_${_n}\\]" "Pulp's own ${_n} untouched")
    endforeach()

elseif(SCENARIO STREQUAL "partial-ui")
    # A bridge SDK built with PULP_ENABLE_GPU=OFF and design import off: the
    # view core exists, Pulp::render and Pulp::design-import do not.
    _pulp_own(_own view-core view canvas view-native)
    _inspect(partial-ui ON OFF "${_vellum_standins}${_own}" 1 _out)
    _expect("${_out}" "COMPAT=\\[\\]" "no Vellum-backed names")
    _expect("${_out}" "T\\[render\\]=\\[ABSENT\\]" "no Vellum renderer standing in")
    _expect("${_out}" "T\\[design-import\\]=\\[ABSENT\\]" "no Vellum importer standing in")

elseif(SCENARIO STREQUAL "view-core")
    # Even with the flag on, an exported Pulp view core means Pulp's UI is
    # present and nothing is defined.
    _pulp_own(_own view-core view canvas)
    _inspect(view-core ON ON "${_vellum_standins}${_own}" 1 _out)
    _expect("${_out}" "COMPAT=\\[\\]" "no Vellum-backed names")
    _expect("${_out}" "T\\[render\\]=\\[ABSENT\\]" "no Vellum renderer beside Pulp's view core")

elseif(SCENARIO STREQUAL "missing")
    list(REMOVE_ITEM _vellum_targets Vellum::MirrorCanvas)
    _standins(_partial ${_vellum_targets})
    _inspect(missing ON ON "${_partial}" 1 _out)
    _expect("${_out}" "COMPAT=\\[render;design-import;view-native;view\\]" "canvas left out")
    _expect("${_out}" "T\\[canvas\\]=\\[ABSENT\\]" "no empty canvas target")

elseif(SCENARIO STREQUAL "twice")
    _inspect(twice ON ON "${_vellum_standins}" 2 _out)
    _expect("${_out}" "FIND1 COMPAT=\\[canvas;render;design-import;view-native;view\\]" "the first find's list")
    _expect("${_out}" "FIND2 COMPAT=\\[canvas;render;design-import;view-native;view\\]" "the same list from the second find")

elseif(SCENARIO STREQUAL "no-bridge")
    _inspect(no-bridge OFF ON "" 1 _out)
    _expect("${_out}" "COMPAT=\\[\\]" "no names")
    _expect("${_out}" "VELLUM_FOUND=\\[\\]" "Vellum never searched")
    _expect("${_out}" "T\\[view\\]=\\[ABSENT\\]" "no view target")

elseif(SCENARIO STREQUAL "gpu-guard")
    # Shaped like the installed SDK: Pulp::gpu-audio reaches the device through
    # $<LINK_ONLY:...> as well as through Pulp::gpu-device, and Pulp::host
    # through Pulp::gpu-audio.
    set(_gpu
        "if(NOT TARGET VellumGpuRuntime::Device)\n"
        "  add_library(VellumGpuRuntime::Device INTERFACE IMPORTED)\n"
        "  add_library(Pulp::gpu-device INTERFACE IMPORTED)\n"
        "  set_property(TARGET Pulp::gpu-device PROPERTY INTERFACE_LINK_LIBRARIES VellumGpuRuntime::Device)\n"
        "  add_library(Pulp::gpu-audio INTERFACE IMPORTED)\n"
        "  set_property(TARGET Pulp::gpu-audio PROPERTY INTERFACE_LINK_LIBRARIES \"$<LINK_ONLY:VellumGpuRuntime::Device>\" Pulp::gpu-device)\n"
        "  add_library(Pulp::host INTERFACE IMPORTED)\n"
        "  set_property(TARGET Pulp::host PROPERTY INTERFACE_LINK_LIBRARIES Pulp::gpu-audio)\n"
        "endif()\n")
    string(JOIN "" _gpu ${_gpu})
    _pulp_own(_own view-core view canvas render view-native design-import)
    set(_refusal "links two GPU device layers")
    # name|ui flag|consumer body|expected (pass, or the "through" pair)
    foreach(_case
            "direct|ON|add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE Pulp::view Pulp::gpu-device)|from Pulp::gpu-device, and .* from Pulp::view\\."
            "private-static|ON|add_library(engine STATIC main.cpp)\ntarget_link_libraries(engine PRIVATE Pulp::gpu-device)\nadd_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE engine Pulp::render)|from Pulp::gpu-device \\(through engine\\), and .* from Pulp::render\\."
            "host-module|ON|add_library(plug MODULE main.cpp)\ntarget_link_libraries(plug PRIVATE Pulp::host Pulp::canvas)|from Pulp::gpu-audio \\(through Pulp::host\\), and .* from Pulp::canvas\\."
            "device|ON|add_library(lib SHARED main.cpp)\ntarget_link_libraries(lib PRIVATE VellumGpuRuntime::Device Vellum::MirrorView_core)|from VellumGpuRuntime::Device, and .* from Vellum::MirrorView_core\\."
            "alias|ON|add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE pulp::design-import Pulp::gpu-audio)|from Pulp::gpu-audio, and .* from pulp::design-import\\."
            "subdirectory|ON|add_subdirectory(sub)|from Pulp::gpu-device, and .* from Pulp::view-native\\."
            "view-only|ON|add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE Pulp::view Pulp::render)|pass"
            "gpu-only|ON|add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE Pulp::gpu-device Pulp::host)|pass"
            "static-library|ON|add_library(engine STATIC main.cpp)\ntarget_link_libraries(engine PUBLIC Pulp::view Pulp::gpu-device)|pass"
            "pulp-ui|OFF|add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE Pulp::view Pulp::gpu-device)|pass")
        string(REPLACE "|" ";" _parts "${_case}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _ui)
        list(GET _parts 2 _body)
        list(GET _parts 3 _want)
        set(_root "${FIXTURE_DIR}/gpu-guard/${_name}")
        set(_prelude "${_vellum_standins}${_gpu}")
        if(_ui STREQUAL "OFF")
            set(_prelude "${_vellum_standins}${_gpu}${_own}")
        endif()
        _write_prefix("${_root}/prefix" ON ${_ui} "" "${_prelude}")
        file(MAKE_DIRECTORY "${_root}/src/sub")
        file(WRITE "${_root}/src/main.cpp" "int main() { return 0; }\n")
        file(WRITE "${_root}/src/sub/CMakeLists.txt"
            "add_executable(nested ../main.cpp)\ntarget_link_libraries(nested PRIVATE Pulp::view-native Pulp::gpu-device)\n")
        file(WRITE "${_root}/src/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(VellumGpuGuard CXX)\n"
            "find_package(Pulp CONFIG REQUIRED)\n"
            "message(STATUS \"COMPAT=[\${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}]\")\n"
            "${_body}\n")
        execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" -S "${_root}/src" -B "${_root}/build"
                -DPulp_DIR=${_root}/prefix/lib/cmake/Pulp ${_isolation}
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        string(REGEX REPLACE "[ \n]+" " " _flat "${_out}${_err}")
        if(_ui STREQUAL "ON")
            _expect("${_flat}" "COMPAT=\\[canvas;render;design-import;view-native;view\\]" "${_name}: Vellum-backed names")
        else()
            _expect("${_flat}" "COMPAT=\\[\\]" "${_name}: Pulp's own UI")
        endif()
        if(_want STREQUAL "pass")
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "gpu-guard ${_name}: configure failed:\n${_out}${_err}")
            endif()
        else()
            if(_rc EQUAL 0)
                message(FATAL_ERROR "gpu-guard ${_name}: configured with two GPU device layers:\n${_out}${_err}")
            endif()
            _expect("${_flat}" "${_refusal}: Pulp's static gpu_runtime \\(VellumGpuRuntime::Device and its Dawn\\) ${_want}" "${_name}: the refusal naming both sides")
        endif()
        message(STATUS "gpu-guard ${_name}: ${_want}")
    endforeach()

elseif(SCENARIO STREQUAL "gpu-owner-real")
    foreach(_input VELLUM_DIR GPU_RUNTIME_DIR GPU_RUNTIME_SKIA_DIR)
        if(NOT ${_input} OR NOT EXISTS "${${_input}}")
            message(STATUS "SKIP: gpu-owner-real needs ${_input} (VELLUM_DIR, GPU_RUNTIME_DIR, GPU_RUNTIME_SKIA_DIR)")
            return()
        endif()
    endforeach()
    find_program(_nm nm)
    if(NOT _nm)
        message(STATUS "SKIP: gpu-owner-real needs nm")
        return()
    endif()
    file(STRINGS "${VELLUM_DIR}/VellumConfigVersion.cmake" _version_line
        REGEX "^set\\(PACKAGE_VERSION \"[^\"]+\"\\)")
    if(NOT _version_line MATCHES "\"([^\"]+)\"")
        message(FATAL_ERROR "gpu-owner-real: cannot read the version from ${VELLUM_DIR}")
    endif()
    set(_version "${CMAKE_MATCH_1}")
    set(_root "${FIXTURE_DIR}/gpu-owner-real")
    file(REMOVE_RECURSE "${_root}")
    # The prelude loads the real gpu-runtime package and shapes Pulp::gpu-device
    # as the installed SDK exports it.
    set(_prelude
        "set(GPU_RUNTIME_SKIA_DIR \"${GPU_RUNTIME_SKIA_DIR}\")\n"
        "find_package(VellumGpuRuntime CONFIG REQUIRED PATHS \"${GPU_RUNTIME_DIR}\" NO_DEFAULT_PATH)\n"
        "if(NOT TARGET Pulp::gpu-device)\n"
        "  add_library(Pulp::gpu-device INTERFACE IMPORTED)\n"
        "  set_property(TARGET Pulp::gpu-device PROPERTY INTERFACE_LINK_LIBRARIES VellumGpuRuntime::Device)\n"
        "endif()\n")
    string(JOIN "" _prelude ${_prelude})
    _write_prefix("${_root}/prefix" ON ON "${_version}" "${_prelude}")
    get_filename_component(_vellum_sdk "${VELLUM_DIR}/../../.." ABSOLUTE)
    file(MAKE_DIRECTORY "${_root}/prefix/share/pulp/vellum-sdk/0.0.0")
    file(CREATE_LINK "${_vellum_sdk}" "${_root}/prefix/share/pulp/vellum-sdk/0.0.0/sdk" SYMBOLIC)
    # The probe references both halves of the device layer: a gpu_runtime::
    # entry point and the wgpu C API.
    file(MAKE_DIRECTORY "${_root}/src")
    file(WRITE "${_root}/src/main.cpp"
        "#include <gpu_runtime/gpu_surface.hpp>\n"
        "#include <cstdio>\n"
        "extern \"C\" void wgpuAdapterAddRef(void*);\n"
        "int main(int argc, char**) {\n"
        "    if (argc > 99) { auto s = gpu_runtime::GpuSurface::create_dawn(); wgpuAdapterAddRef(s.get()); }\n"
        "    std::puts(\"linked\");\n"
        "    return 0;\n"
        "}\n")
    set(_generator)
    find_program(_ninja ninja)
    if(_ninja)
        set(_generator -G Ninja)
    endif()
    set(_jobs 4)
    if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
        set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    # Builds <name> from a CMakeLists body; sets <name>_rc and <name>_log.
    function(_gpu_project name body)
        set(_src "${_root}/${name}")
        file(MAKE_DIRECTORY "${_src}")
        file(WRITE "${_src}/CMakeLists.txt"
            "cmake_minimum_required(VERSION 3.24)\nproject(${name} CXX)\n${body}\n")
        execute_process(COMMAND ${_clean_env} "${CMAKE_COMMAND}" ${_generator} -S "${_src}"
                -B "${_src}/build" -DCMAKE_BUILD_TYPE=Release
                -DPulp_DIR=${_root}/prefix/lib/cmake/Pulp ${_isolation}
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        if(_rc EQUAL 0)
            execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_src}/build" -j ${_jobs}
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _bout ERROR_VARIABLE _berr)
            string(APPEND _out "${_bout}${_berr}")
        endif()
        set(${name}_rc "${_rc}" PARENT_SCOPE)
        set(${name}_log "${_out}${_err}" PARENT_SCOPE)
    endfunction()
    # Counts the gpu_runtime:: and wgpu symbols `file` defines.
    function(_gpu_owned file out_runtime out_wgpu)
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "gpu-owner-real: ${file} was not built")
        endif()
        execute_process(COMMAND "${_nm}" -U -C "${file}"
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _syms ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "gpu-owner-real: nm failed on ${file}: ${_err}")
        endif()
        string(REGEX MATCHALL "\n[0-9a-f]+ [A-Za-z] gpu_runtime::" _runtime "\n${_syms}")
        string(REGEX MATCHALL "\n[0-9a-f]+ [A-Za-z] _?wgpu[A-Z]" _wgpu "\n${_syms}")
        list(LENGTH _runtime _n_runtime)
        list(LENGTH _wgpu _n_wgpu)
        set(${out_runtime} ${_n_runtime} PARENT_SCOPE)
        set(${out_wgpu} ${_n_wgpu} PARENT_SCOPE)
    endfunction()

    # The Vellum-backed path: the device layer comes from the provider.
    _gpu_project(vellum_only
        "find_package(Pulp CONFIG REQUIRED)\nadd_executable(app ../src/main.cpp)\ntarget_link_libraries(app PRIVATE Pulp::render VellumGpuRuntime::DeviceApi)")
    if(NOT vellum_only_rc EQUAL 0)
        message(FATAL_ERROR "gpu-owner-real: the Vellum-backed consumer failed:\n${vellum_only_log}")
    endif()
    # The control: the provider and the static device, linked directly.
    string(CONCAT _raw_body
        "set(GPU_RUNTIME_SKIA_DIR \"${GPU_RUNTIME_SKIA_DIR}\")\n"
        "find_package(VellumGpuRuntime CONFIG REQUIRED PATHS \"${GPU_RUNTIME_DIR}\" NO_DEFAULT_PATH)\n"
        "find_package(Vellum CONFIG REQUIRED PATHS \"${VELLUM_DIR}\" NO_DEFAULT_PATH)\n"
        "add_executable(app ../src/main.cpp)\n"
        "target_link_libraries(app PRIVATE VellumGpuRuntime::Device Vellum::MirrorRender_2d)")
    _gpu_project(raw_both "${_raw_body}")
    if(NOT raw_both_rc EQUAL 0)
        message(FATAL_ERROR "gpu-owner-real: the control failed to build:\n${raw_both_log}")
    endif()
    # The same pair through the Pulp names.
    _gpu_project(pulp_both
        "find_package(Pulp CONFIG REQUIRED)\nadd_executable(app ../src/main.cpp)\ntarget_link_libraries(app PRIVATE Pulp::render Pulp::gpu-device)")
    string(REGEX REPLACE "[ \n]+" " " _flat "${pulp_both_log}")
    if(pulp_both_rc EQUAL 0)
        message(FATAL_ERROR "gpu-owner-real: Pulp::render with Pulp::gpu-device configured:\n${pulp_both_log}")
    endif()
    _expect("${_flat}" "app links two GPU device layers" "the refusal for the Pulp-named pair")

    file(GLOB _provider "${_vellum_sdk}/lib/libvellum-skia-dawn-provider-*.dylib"
        "${_vellum_sdk}/lib/libvellum-skia-dawn-provider-*.so")
    list(LENGTH _provider _n_provider)
    if(NOT _n_provider EQUAL 1)
        message(FATAL_ERROR "gpu-owner-real: expected one provider library under ${_vellum_sdk}/lib, found '${_provider}'")
    endif()
    _gpu_owned("${_provider}" _provider_runtime _provider_wgpu)
    _gpu_owned("${_root}/vellum_only/build/app" _vellum_runtime _vellum_wgpu)
    _gpu_owned("${_root}/raw_both/build/app" _raw_runtime _raw_wgpu)
    message(STATUS "gpu-owner-real: provider defines gpu_runtime=${_provider_runtime} wgpu=${_provider_wgpu}")
    message(STATUS "gpu-owner-real: Vellum-backed app defines gpu_runtime=${_vellum_runtime} wgpu=${_vellum_wgpu}")
    message(STATUS "gpu-owner-real: control app defines gpu_runtime=${_raw_runtime} wgpu=${_raw_wgpu}")
    if(_provider_runtime EQUAL 0 OR _provider_wgpu EQUAL 0)
        message(FATAL_ERROR "gpu-owner-real: the provider defines no gpu_runtime:: or wgpu symbol; nm reads nothing here")
    endif()
    if(_raw_runtime EQUAL 0 OR _raw_wgpu EQUAL 0)
        message(FATAL_ERROR "gpu-owner-real: the control defines no gpu_runtime:: or wgpu symbol, so nm cannot see a second owner")
    endif()
    # The app must still reference both halves, so that zero defined means
    # resolved from the provider rather than never linked.
    execute_process(COMMAND "${_nm}" -u -C "${_root}/vellum_only/build/app" OUTPUT_VARIABLE _undefined)
    if(NOT _undefined MATCHES "gpu_runtime::GpuSurface::create_dawn" OR NOT _undefined MATCHES "_?wgpuAdapterAddRef")
        message(FATAL_ERROR "gpu-owner-real: the Vellum-backed app does not reference the device layer:\n${_undefined}")
    endif()
    if(NOT _vellum_runtime EQUAL 0 OR NOT _vellum_wgpu EQUAL 0)
        message(FATAL_ERROR "gpu-owner-real: the Vellum-backed app defines its own gpu_runtime::/wgpu symbols beside the provider's")
    endif()
    message(STATUS "gpu-owner-real: one owner on the Vellum-backed path, two on the control, Pulp-named pair refused")

elseif(SCENARIO STREQUAL "no-vellum" OR SCENARIO STREQUAL "real" OR SCENARIO STREQUAL "wrong-version")
    set(_root "${FIXTURE_DIR}/${SCENARIO}")
    file(REMOVE_RECURSE "${_root}")
    set(_version "")
    if(NOT SCENARIO STREQUAL "no-vellum")
        if(NOT VELLUM_DIR OR NOT EXISTS "${VELLUM_DIR}/VellumConfigVersion.cmake")
            if(REQUIRE_VELLUM)
                message(FATAL_ERROR "${SCENARIO}: REQUIRE_VELLUM is on but VELLUM_DIR='${VELLUM_DIR}' holds no Vellum package")
            endif()
            message(STATUS "SKIP: no installed Vellum SDK (pass -DVELLUM_DIR=<dir with VellumConfig.cmake>)")
            return()
        endif()
        file(STRINGS "${VELLUM_DIR}/VellumConfigVersion.cmake" _version_line
            REGEX "^set\\(PACKAGE_VERSION \"[^\"]+\"\\)")
        if(NOT _version_line MATCHES "\"([^\"]+)\"")
            message(FATAL_ERROR "${SCENARIO}: cannot read the version from ${VELLUM_DIR}/VellumConfigVersion.cmake")
        endif()
        set(_version "${CMAKE_MATCH_1}")
        if(SCENARIO STREQUAL "wrong-version")
            set(_version "0.0.1")
        endif()
    endif()
    _write_prefix("${_root}/prefix" ON ON "${_version}" "")
    if(NOT SCENARIO STREQUAL "no-vellum")
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
    if(NOT SCENARIO STREQUAL "real")
        if(_rc EQUAL 0)
            message(FATAL_ERROR "${SCENARIO}: the consumer configured without a usable Vellum:\n${_out}${_err}")
        endif()
        # The warning comes from the config's Vellum_FOUND branch, so it proves
        # the refusal is the gate's and not an accident of missing targets.
        # CMake wraps warning text, so match it with whitespace collapsed.
        string(REGEX REPLACE "[ \n]+" " " _flat "${_out}${_err}")
        _expect("${_flat}" "CMake Warning at [^ ]*/PulpConfig.cmake:[0-9]+ \\(message\\): Pulp: this SDK serves Pulp::view;Pulp::canvas;Pulp::render;Pulp::view-native;Pulp::design-import from Vellum, but the Vellum package was not found" "the config's warning naming the cause")
        _expect("${_out}${_err}" "Pulp::view is not Vellum-backed in this SDK" "a configure error naming the first name")
        message(STATUS "${SCENARIO}: warned and refused as expected")
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
    message(STATUS "real: five Vellum-backed names built and ran against Vellum ${_version}")

else()
    message(FATAL_ERROR "unknown SCENARIO '${SCENARIO}'")
endif()
message(STATUS "${SCENARIO}: ok")
