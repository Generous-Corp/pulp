# test_vellum_ui_in_tree.cmake: PULP_UI_FROM_VELLUM_IN_TREE, which leaves
# Pulp's own UI directories out of the source tree and serves pulp::view,
# canvas, render, design-import, view-audio and view-tooling from an installed
# Vellum SDK (integrations/pulp-vellum/cmake/PulpVellumUiInTree.cmake).
#
# Scenarios (-DSCENARIO=...):
#   guards  Stand-in targets, no Vellum needed. A complete set defines all six
#           names on the Vellum stand-ins and marks the in-tree GPU device
#           alias's real target; a defined pulp-view-core, a lower-case
#           pulp::render left on Pulp's own renderer, and a missing editor
#           bridge are each refused.
#   real    The Pulp source tree configured twice against an installed Vellum
#           SDK (-DVELLUM_DIR=<dir with VellumConfig.cmake>), tests and
#           examples off. With the option off it configures and generates with
#           Pulp's own UI. With it on it leaves out exactly the listed
#           directories, serves all six names from Vellum, and the module's
#           scan of every target's link, include and definition properties
#           reports exactly the consumers listed in _unported below, which still
#           name a UI target the mode does not define; every CMake error must be
#           one of those. The list only shrinks: a consumer that is ported must
#           leave it, and a new one, by any spelling, fails the test. Without
#           VELLUM_DIR it prints SKIP, unless REQUIRE_VELLUM is on.
#
# Inputs: PULP_SOURCE_DIR, FIXTURE_DIR, SCENARIO, and for `real` VELLUM_DIR.

cmake_minimum_required(VERSION 3.25)

foreach(_required PULP_SOURCE_DIR FIXTURE_DIR SCENARIO)
    if(NOT ${_required})
        message(FATAL_ERROR "test_vellum_ui_in_tree: ${_required} is required")
    endif()
endforeach()

set(_module "${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumUiInTree.cmake")

function(_expect text pattern what)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${SCENARIO}: expected ${what} (/${pattern}/) in:\n${text}")
    endif()
endfunction()

function(_reject text pattern what)
    if(text MATCHES "${pattern}")
        message(FATAL_ERROR "${SCENARIO}: unexpected ${what} (/${pattern}/) in:\n${text}")
    endif()
endfunction()

# Configures a fixture project whose body is `body` and returns its result code
# and combined output. `_fixture_languages` (default NONE) names the project's
# languages; `_fixture_consumer` is written to consumer/CMakeLists.txt.
set(_fixture_languages NONE)
set(_fixture_consumer "")
function(_fixture name body out_rc out_text)
    set(_dir "${FIXTURE_DIR}/${name}")
    file(REMOVE_RECURSE "${_dir}")
    file(WRITE "${_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(pulp_vellum_ui_in_tree_${name} LANGUAGES ${_fixture_languages})\n"
        "set(PULP_UI_FROM_VELLUM_IN_TREE ON)\n"
        "include(\"${_module}\")\n"
        "${body}")
    if(_fixture_consumer)
        file(WRITE "${_dir}/consumer/CMakeLists.txt" "${_fixture_consumer}")
        file(WRITE "${_dir}/consumer/consumer.c" "int pulp_consumer(void) { return 0; }\n")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_dir}" -B "${_dir}/build"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_text} "${_out}${_err}" PARENT_SCOPE)
endfunction()

if(SCENARIO STREQUAL "guards")
    # The installed Vellum targets as GLOBAL imported stand-ins, and the
    # in-tree targets the mapping reaches through Pulp:: aliases.
    set(_vellum "")
    foreach(_target IN ITEMS MirrorCanvas MirrorRender_2d MirrorView_core
            MirrorDesign_import MirrorView_script)
        string(APPEND _vellum "add_library(Vellum::${_target} INTERFACE IMPORTED GLOBAL)\n")
    endforeach()
    set(_in_tree "")
    foreach(_target IN ITEMS pulp-vellum pulp-vellum-audio pulp-editor-bridge pulp-gpu-device)
        string(APPEND _in_tree "add_library(${_target} INTERFACE)\n")
    endforeach()
    set(_define "pulp_vellum_in_tree_define_ui_targets()\n")

    _fixture(complete
        "${_vellum}${_in_tree}${_define}get_target_property(_owner pulp-gpu-device PULP_GPU_RUNTIME_OWNER)\nmessage(STATUS \"gpu-device owner=\${_owner}\")\n"
        _rc _text)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "guards: a complete stand-in set did not configure:\n${_text}")
    endif()
    _expect("${_text}" "pulp::canvas -> Vellum::MirrorCanvas" "canvas on Vellum")
    _expect("${_text}" "pulp::render -> Vellum::MirrorRender_2d" "render on Vellum")
    _expect("${_text}" "pulp::design-import -> Vellum::MirrorDesign_import" "design-import on Vellum")
    _expect("${_text}" "pulp::view -> Vellum::MirrorView_core;Vellum::MirrorDesign_import;Vellum::MirrorView_script;Pulp::vellum" "view on Vellum")
    _expect("${_text}" "pulp::view-audio -> Pulp::vellum-audio" "view-audio on the audio package")
    _expect("${_text}" "pulp::view-tooling -> Vellum::MirrorView_core;Pulp::vellum;Pulp::editor-bridge;Vellum::MirrorView_script" "view-tooling on the editor bridge")
    _expect("${_text}" "gpu-device owner=static" "the GPU owner mark on the alias's real target")

    _fixture(view-core "${_vellum}${_in_tree}add_library(pulp-view-core INTERFACE)\n${_define}" _rc _text)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "guards: configured with pulp-view-core defined:\n${_text}")
    endif()
    _expect("${_text}" "pulp-view-core is defined in this tree" "the view-core refusal")

    _fixture(own-render
        "${_vellum}${_in_tree}add_library(pulp-render INTERFACE)\nadd_library(pulp::render ALIAS pulp-render)\n${_define}"
        _rc _text)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "guards: configured with pulp::render on Pulp's own renderer:\n${_text}")
    endif()
    _expect("${_text}" "pulp::render resolves to 'pulp-render'" "the lower-case alias refusal")

    string(REPLACE "add_library(pulp-editor-bridge INTERFACE)\n" "" _no_bridge "${_in_tree}")
    _fixture(no-bridge "${_vellum}${_no_bridge}${_define}" _rc _text)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "guards: configured without the editor bridge:\n${_text}")
    endif()
    _expect("${_text}" "Pulp::view-tooling is not Vellum-backed" "the view-tooling refusal")

    # A consumer configured before the names are defined, as every core/
    # directory is in the source tree, must still resolve them. The Vellum
    # stand-ins here are directory-scoped, like an ordinary find_package.
    string(REPLACE " INTERFACE IMPORTED GLOBAL)" " INTERFACE IMPORTED)" _vellum_local "${_vellum}")
    set(_fixture_languages C)
    set(_fixture_consumer
        "add_library(consumer STATIC consumer.c)\n"
        "target_link_libraries(consumer PRIVATE pulp::view pulp::view-tooling)\n")
    string(JOIN "" _fixture_consumer ${_fixture_consumer})
    _fixture(early-consumer "${_vellum_local}add_subdirectory(consumer)\n${_in_tree}${_define}" _rc _text)
    set(_fixture_languages NONE)
    set(_fixture_consumer "")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "guards: a consumer added before the names were defined did not resolve them:\n${_text}")
    endif()

    message(STATUS "guards: ok")

elseif(SCENARIO STREQUAL "real")
    if(NOT VELLUM_DIR OR NOT EXISTS "${VELLUM_DIR}/VellumConfig.cmake")
        if(REQUIRE_VELLUM)
            message(FATAL_ERROR "real: REQUIRE_VELLUM is on but VELLUM_DIR='${VELLUM_DIR}' holds no Vellum package")
        endif()
        message(STATUS "SKIP: no installed Vellum SDK (pass -DVELLUM_DIR=<dir with VellumConfig.cmake>)")
        return()
    endif()

    # The directories the mode leaves out, read from the module itself.
    include("${_module}")
    set(_skipped "")
    foreach(_entry IN LISTS PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS)
        string(REGEX REPLACE "\\|.*" "" _dir "${_entry}")
        list(APPEND _skipped "${_dir}")
    endforeach()
    # Targets that still name a UI target this mode does not define, as the
    # module's scan reports them ("<target> -> <name>"). Each leaves this list
    # when it is ported; the test then fails until the entry is removed, so the
    # list cannot go stale, and any entry not listed fails it.
    set(_unported
        "pulp-format-core -> pulp-view-core"
        "pulp-format-view -> pulp::view-core"
        "pulp-format-view -> pulp::view-script"
        "pulp-inspect-telemetry -> pulp::view-core")
    set(_generator)
    find_program(_ninja ninja)
    if(_ninja)
        set(_generator -G Ninja)
    endif()
    set(_common ${_generator} -DCMAKE_BUILD_TYPE=Release
        -DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=OFF -DPULP_ENABLE_GPU=OFF
        -DPULP_ENABLE_PULP_VELLUM=ON -DPULP_ENABLE_PULP_VELLUM_AUDIO=ON
        -DVellum_DIR=${VELLUM_DIR})

    file(REMOVE_RECURSE "${FIXTURE_DIR}/real-off" "${FIXTURE_DIR}/real-on")
    # Control: the same profile with the option off is today's build.
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${PULP_SOURCE_DIR}"
            -B "${FIXTURE_DIR}/real-off" ${_common} -DPULP_UI_FROM_VELLUM_IN_TREE=OFF
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: the option-off control did not configure:\n${_out}${_err}")
    endif()
    _reject("${_out}${_err}" "Pulp UI from Vellum" "in-tree Vellum output with the option off")
    if(NOT EXISTS "${FIXTURE_DIR}/real-off/core/view/CMakeFiles/pulp-view-core.dir")
        message(FATAL_ERROR "real: the option-off control did not configure pulp-view-core")
    endif()
    message(STATUS "real: option off configured core/view as before")

    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${PULP_SOURCE_DIR}"
            -B "${FIXTURE_DIR}/real-on" ${_common} -DPULP_UI_FROM_VELLUM_IN_TREE=ON
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_text "${_out}${_err}")

    foreach(_dir IN LISTS _skipped)
        _expect("${_text}" "Pulp UI from Vellum \\(in tree\\): skipping ${_dir}:" "${_dir} left out")
    endforeach()
    string(REGEX MATCHALL "skipping [^:]+:" _skip_lines "${_text}")
    list(LENGTH _skip_lines _skip_count)
    list(LENGTH _skipped _expected_count)
    if(NOT _skip_count EQUAL _expected_count)
        message(FATAL_ERROR "real: left out ${_skip_lines}, expected exactly ${_skipped}")
    endif()
    if(EXISTS "${FIXTURE_DIR}/real-on/core/view")
        message(FATAL_ERROR "real: core/view was configured with the option on")
    endif()
    foreach(_name IN ITEMS canvas render design-import view view-audio view-tooling)
        _expect("${_text}" "pulp::${_name} -> [^\n]*(Vellum::|Pulp::vellum)" "pulp::${_name} served from Vellum")
    endforeach()
    _expect("${_text}" "pulp::view-tooling -> [^\n]*Pulp::editor-bridge" "view-tooling on the editor bridge")

    # The module's scan is the list of record. It reads every target's link,
    # include and definition properties, so it sees plain names (which CMake
    # links as -l<name> without an error) and every missing name of a target,
    # not only the first one CMake reports.
    string(REGEX MATCHALL "Pulp UI from Vellum \\(in tree\\): unported [^\n]+" _scan "${_text}")
    set(_found "")
    foreach(_line IN LISTS _scan)
        string(REGEX REPLACE "^.*unported " "" _entry "${_line}")
        list(APPEND _found "${_entry}")
    endforeach()
    # Every CMake error must be one the scan explains: a link to, or a generator
    # expression naming, a target it reported as unported.
    string(REGEX MATCHALL "Target \"[^\"]+\" links to:[ \n]+[^ \n]+" _links "${_text}")
    string(REGEX MATCHALL "Error evaluating generator expression:[ \n]+[^\n]+[ \n]+Target \"[^\"]+\" not found" _genex "${_text}")
    string(REGEX MATCHALL "CMake Error at [^\n]+" _errors "${_text}")
    list(LENGTH _errors _error_count)
    list(LENGTH _links _link_count)
    list(LENGTH _genex _genex_count)
    math(EXPR _explained "${_link_count} + ${_genex_count}")
    if(NOT _error_count EQUAL _explained)
        message(FATAL_ERROR "real: errors other than unported consumers:\n${_text}")
    endif()
    foreach(_link IN LISTS _links)
        string(REGEX REPLACE "Target \"([^\"]+)\" links to:[ \n]+([^ \n]+)" "\\1 -> \\2" _entry "${_link}")
        if(NOT _entry IN_LIST _found)
            message(FATAL_ERROR "real: CMake reports '${_entry}', which the scan missed:\n${_text}")
        endif()
    endforeach()
    foreach(_expr IN LISTS _genex)
        string(REGEX REPLACE ".*Target \"([^\"]+)\" not found" "\\1" _name "${_expr}")
        if(NOT _found MATCHES "-> ${_name}(;|$)")
            message(FATAL_ERROR "real: a generator expression names ${_name}, which the scan missed:\n${_text}")
        endif()
    endforeach()
    foreach(_entry IN LISTS _found)
        if(NOT _entry IN_LIST _unported)
            message(FATAL_ERROR "real: a new consumer links a target this mode leaves out: ${_entry}\n${_text}")
        endif()
    endforeach()
    foreach(_entry IN LISTS _unported)
        if(NOT _entry IN_LIST _found)
            message(FATAL_ERROR "real: '${_entry}' no longer fails; remove it from the list in this test")
        endif()
    endforeach()
    if(_unported AND _rc EQUAL 0)
        message(FATAL_ERROR "real: configured although consumers are listed as unported")
    elseif(NOT _unported AND NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: the option-on configure failed:\n${_text}")
    endif()
    list(LENGTH _unported _unported_count)
    message(STATUS "real: six names served from Vellum, ${_skip_count} directories left out, ${_unported_count} consumers still to port")
    message(STATUS "real: ok")

else()
    message(FATAL_ERROR "test_vellum_ui_in_tree: unknown SCENARIO '${SCENARIO}'")
endif()
