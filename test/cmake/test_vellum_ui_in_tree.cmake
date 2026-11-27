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
#   install-rules  A static read of PulpInstallRules.cmake: every install of a
#           path under a skipped directory is gated on the mode, and the target
#           and header drops are wired in.
#   cli-force-load  pulp-cli's force-load of Pulp's UI archives (macOS): still
#           applied with the mode off, refused with it on.
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
#           VELLUM_DIR it prints SKIP, unless REQUIRE_VELLUM is on. A third
#           configure, with the inspector off and the examples on, has no
#           unported consumer and builds pulp-format-core, pulp-format-view,
#           pulp-vellum, every retained example and the NO_EDITOR link floor
#           fixture; nm confirms format-core is the PULP_FORMAT_LEGACY_VIEW=0
#           build, that each plug-in module sits in its bundle with pulp::format
#           and no pulp::view or pulp::canvas symbol and no scripted editor,
#           and the fixture's tests pass. A
#           probe plug-in added to that tree builds its CLAP module without
#           PULP_CLAP_GUI, each Pulp view editor keyword is refused, and a
#           program linking pulp::format-view alone reaches both adapters.
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

elseif(SCENARIO STREQUAL "install-rules")
    # A static read of the install rules: every install of a path under a
    # directory the mode leaves out must sit inside if(NOT
    # PULP_UI_FROM_VELLUM_IN_TREE ...), and the target and header drops must be
    # wired in. Without this the mode would still ship Pulp's own view headers
    # and window-host sources, which every installed-SDK plug-in compiles.
    include("${_module}")
    set(_skipped "")
    foreach(_entry IN LISTS PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS)
        string(REGEX REPLACE "\\|.*" "" _dir "${_entry}")
        list(APPEND _skipped "${_dir}")
    endforeach()
    set(_rules_file "${PULP_SOURCE_DIR}/tools/cmake/PulpInstallRules.cmake")
    if(DEFINED INSTALL_RULES)
        set(_rules_file "${INSTALL_RULES}")
    endif()
    file(READ "${_rules_file}" _rules)
    string(REPLACE ";" "<semicolon>" _rules "${_rules}")
    string(REPLACE "\n" ";" _lines "${_rules}")
    set(_stack "")
    set(_line_no 0)
    set(_ungated "")
    foreach(_line IN LISTS _lines)
        math(EXPR _line_no "${_line_no} + 1")
        string(REGEX REPLACE "#.*$" "" _code "${_line}")
        string(STRIP "${_code}" _code)
        if(_code MATCHES "^if\\((.*)$")
            list(APPEND _stack "${CMAKE_MATCH_1}")
        elseif(_code MATCHES "^endif\\(")
            list(POP_BACK _stack)
        endif()
        foreach(_dir IN LISTS _skipped)
            if(_code MATCHES "/${_dir}/")
                if(NOT _stack MATCHES "NOT PULP_UI_FROM_VELLUM_IN_TREE")
                    list(APPEND _ungated "${_line_no}: ${_code}")
                endif()
            endif()
        endforeach()
    endforeach()
    if(_ungated)
        string(REPLACE ";" "\n  " _report "${_ungated}")
        message(FATAL_ERROR
            "install-rules: installs from a directory PULP_UI_FROM_VELLUM_IN_TREE leaves out, "
            "not gated on the mode:\n  ${_report}")
    endif()
    foreach(_call IN ITEMS pulp_vellum_in_tree_drop_ui_targets pulp_vellum_in_tree_drop_ui_headers)
        if(NOT _rules MATCHES "if\\(PULP_UI_FROM_VELLUM_IN_TREE\\)[ \n]+${_call}\\(")
            message(FATAL_ERROR "install-rules: ${_call}() is not called under if(PULP_UI_FROM_VELLUM_IN_TREE)")
        endif()
    endforeach()
    message(STATUS "install-rules: ok")

elseif(SCENARIO STREQUAL "cli-force-load")
    # pulp-cli force-loads Pulp's own UI archives on macOS. With the mode off it
    # still does; with it on it must refuse rather than silently drop them.
    if(NOT CMAKE_HOST_APPLE)
        message(STATUS "SKIP: the CLI force-load is macOS only")
        return()
    endif()
    function(_cli_fixture name mode out_rc out_text)
        set(_dir "${FIXTURE_DIR}/${name}")
        file(REMOVE_RECURSE "${_dir}")
        file(WRITE "${_dir}/main.c" "int main(void) { return 0; }\n")
        set(_body "cmake_minimum_required(VERSION 3.25)\nproject(cli_force_load LANGUAGES C)\n")
        string(APPEND _body "set(PULP_UI_FROM_VELLUM_IN_TREE ${mode})\n")
        foreach(_archive IN ITEMS pulp-view-core pulp-design-import pulp-view-script pulp-canvas pulp-runtime pulp-inspect-protocol)
            string(APPEND _body
                "add_library(${_archive} STATIC IMPORTED)\n"
                "set_target_properties(${_archive} PROPERTIES IMPORTED_LOCATION \"\${CMAKE_CURRENT_SOURCE_DIR}/lib${_archive}.a\")\n")
        endforeach()
        string(APPEND _body
            "add_executable(pulp-cli main.c)\n"
            "include(\"${PULP_SOURCE_DIR}/tools/cli/cmake/PulpCliForceLoad.cmake\")\n"
            "pulp_cli_force_load_archives(pulp-cli)\n"
            "get_target_property(_options pulp-cli LINK_OPTIONS)\n"
            "message(STATUS \"cli options=\${_options}\")\n")
        file(WRITE "${_dir}/CMakeLists.txt" "${_body}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_dir}" -B "${_dir}/build"
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        set(${out_rc} "${_rc}" PARENT_SCOPE)
        set(${out_text} "${_out}${_err}" PARENT_SCOPE)
    endfunction()

    _cli_fixture(cli-off OFF _rc _text)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "cli-force-load: the mode-off control did not configure:\n${_text}")
    endif()
    foreach(_archive IN ITEMS pulp-view-core pulp-design-import pulp-view-script pulp-canvas)
        _expect("${_text}" "-Wl,-force_load,\\$<TARGET_FILE:${_archive}>" "${_archive} force-loaded with the mode off")
    endforeach()

    _cli_fixture(cli-on ON _rc _text)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "cli-force-load: configured with the mode on, dropping the UI archives:\n${_text}")
    endif()
    _expect("${_text}" "force-loads Pulp's own UI archives" "the CLI refusal")
    message(STATUS "cli-force-load: ok")

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

    # Build: the same profile with the inspector off and the examples on, which
    # leaves out every consumer still listed above, must configure and generate
    # cleanly. pulp-format-core, pulp-format-view and the Vellum editor adapter
    # that serves the view half must build, and so must every retained example
    # (examples/vellum_in_tree.cmake) and the NO_EDITOR link floor fixture.
    # format-core must be the UI-free one: its archive holds the reserved
    # create_view slot and the mode marker, and no create_view under its plain
    # name. A deferred include (registered by a project include, so no file in
    # the source tree changes) writes the list of plug-in modules the examples
    # define, with their source directories, and adds an object that defines a
    # pulp::view symbol as the control for the symbol scan below.
    set(_plugins_src "${FIXTURE_DIR}/real-plugins-src")
    file(REMOVE_RECURSE "${FIXTURE_DIR}/real-build" "${_plugins_src}")
    file(WRITE "${_plugins_src}/view_marker.cpp"
        "namespace pulp::view { int in_tree_plugin_scan_marker() { return 6; } }\n")
    file(WRITE "${_plugins_src}/plugins.cmake"
        "add_library(pulp-in-tree-view-marker OBJECT \"${_plugins_src}/view_marker.cpp\")\n"
        "add_custom_target(pulp-in-tree-plugin-proof)\n"
        "add_dependencies(pulp-in-tree-plugin-proof pulp-in-tree-view-marker pulp-examples-all\n"
        "    pulp-format-core pulp-format-view pulp-vellum)\n"
        "foreach(_t PulpNoEditorLinkFloor_VST3 PulpNoEditorLinkFloor_CLAP PulpNoEditorLinkFloor_AU\n"
        "        PulpNoEditorLinkFloorControl_CLAP)\n"
        "    if(TARGET \${_t})\n"
        "        add_dependencies(pulp-in-tree-plugin-proof \${_t})\n"
        "    endif()\n"
        "endforeach()\n"
        "include(\"\${CMAKE_SOURCE_DIR}/tools/cmake/PulpCollectTargets.cmake\")\n"
        "pulp_collect_targets_recursive(_targets \"\${CMAKE_SOURCE_DIR}/examples\"\n"
        "    TYPES MODULE_LIBRARY SHARED_LIBRARY EXECUTABLE)\n"
        "get_property(_dirs DIRECTORY \"\${CMAKE_SOURCE_DIR}/examples\" PROPERTY SUBDIRECTORIES)\n"
        "set(_lines \"\")\n"
        "foreach(_dir IN LISTS _dirs)\n"
        "    string(APPEND _lines \"dir|\${_dir}\\n\")\n"
        "endforeach()\n"
        "foreach(_t IN LISTS _targets)\n"
        "    if(_t MATCHES \"_(VST3|CLAP|AU|Standalone|AUv3Framework)$\")\n"
        "        get_target_property(_src \${_t} SOURCE_DIR)\n"
        "        string(APPEND _lines \"module|\${_t}|\${_src}|$<TARGET_FILE:\${_t}>\\n\")\n"
        "    endif()\n"
        "endforeach()\n"
        "string(APPEND _lines \"marker|$<TARGET_OBJECTS:pulp-in-tree-view-marker>\\n\")\n"
        "file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/pulp-in-tree-plugin-modules.txt\"\n"
        "    CONTENT \"\${_lines}\")\n")
    set(_plugins_include "${FIXTURE_DIR}/real-plugins-include.cmake")
    file(WRITE "${_plugins_include}"
        "if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)\n"
        "    cmake_language(DEFER DIRECTORY \"\${CMAKE_SOURCE_DIR}\" CALL include\n"
        "        \"${_plugins_src}/plugins.cmake\")\n"
        "endif()\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${PULP_SOURCE_DIR}"
            -B "${FIXTURE_DIR}/real-build" ${_common} -DPULP_UI_FROM_VELLUM_IN_TREE=ON
            -DPULP_ENABLE_INSPECTOR=OFF -DPULP_BUILD_EXAMPLES=ON
            -DPULP_VELLUM_IN_TREE_LINK_FLOOR_FIXTURE=ON
            -DCMAKE_PROJECT_Pulp_INCLUDE=${_plugins_include}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: the inspector-off, examples-on configure failed:\n${_out}${_err}")
    endif()
    _reject("${_out}${_err}" "unported " "an unported consumer with the inspector off")
    # Through the build governor, so the build takes a share of a shared host
    # rather than every core.
    execute_process(COMMAND "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
            "${CMAKE_COMMAND}" --build "${FIXTURE_DIR}/real-build"
            --target pulp-in-tree-plugin-proof
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: format-core, format-view, pulp-vellum, the retained examples or "
            "the link floor fixture did not build in the mode:\n${_out}${_err}")
    endif()
    find_program(_nm NAMES nm llvm-nm)
    if(NOT _nm)
        message(FATAL_ERROR "real: nm is required to read pulp-format-core's archive")
    endif()
    file(GLOB_RECURSE _core_archive "${FIXTURE_DIR}/real-build/core/format/*pulp-format-core.a")
    list(LENGTH _core_archive _core_archive_count)
    if(NOT _core_archive_count EQUAL 1)
        message(FATAL_ERROR "real: expected one pulp-format-core archive, found [${_core_archive}]")
    endif()
    execute_process(COMMAND "${_nm}" "${_core_archive}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _symbols ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: nm could not read ${_core_archive}")
    endif()
    _expect("${_symbols}" " [ABCDGRST] _?pulp_format_core_was_built_with_PULP_FORMAT_LEGACY_VIEW_0\n"
        "the UI-free mode marker defined in pulp-format-core")
    _expect("${_symbols}" " T _?_ZN4pulp6format9Processor[0-9]+reserved_legacy_view_create_view_Ev\n"
        "the reserved create_view slot defined in pulp-format-core")
    _reject("${_symbols}" "9Processor11create_viewE" "Processor::create_view under its plain name")
    message(STATUS "real: format-core, format-view and pulp-vellum build in the mode; format-core is UI-free")

    # Every retained example builds, and every plug-in module it defines is a
    # bundle that carries the format adapter and none of Pulp's view.
    #   - Count: each example vellum_in_tree.cmake adds without a guard must
    #     have been configured, and each configured example that calls
    #     pulp_add_plugin must contribute a module, so an example that silently
    #     drops out fails here.
    #   - Bundle: each VST3, CLAP, AU, Standalone and AU v3 framework module
    #     lies inside its bundle, next to the bundle's Info.plist.
    #   - Symbols: nm finds no pulp::view or pulp::canvas symbol in a module,
    #     and at least one pulp::format symbol, so a near-empty symbol table
    #     cannot pass as clean. The same nm and pattern must find the
    #     pulp::view symbol the marker object defines.
    #   - Scripted editors stay opt-in: no module loads Vellum's view-script
    #     library or names its script engine; the pattern is shown to match
    #     the engine in the Vellum provider library that defines it.
    set(_manifest_file "${FIXTURE_DIR}/real-build/pulp-in-tree-plugin-modules.txt")
    if(NOT EXISTS "${_manifest_file}")
        message(FATAL_ERROR "real: the plug-in module list was not written: ${_manifest_file}")
    endif()
    file(STRINGS "${_manifest_file}" _manifest)
    set(_configured_dirs "")
    set(_modules "")
    set(_marker "")
    foreach(_line IN LISTS _manifest)
        if(_line MATCHES "^dir\\|(.+)$")
            list(APPEND _configured_dirs "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^module\\|")
            list(APPEND _modules "${_line}")
        elseif(_line MATCHES "^marker\\|(.+)$")
            set(_marker "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    file(STRINGS "${PULP_SOURCE_DIR}/examples/vellum_in_tree.cmake" _example_lines
        REGEX "^add_subdirectory\\(")
    if(NOT _example_lines)
        message(FATAL_ERROR "real: examples/vellum_in_tree.cmake adds no example")
    endif()
    foreach(_line IN LISTS _example_lines)
        string(REGEX REPLACE "^add_subdirectory\\(([^)]+)\\).*$" "\\1" _example "${_line}")
        if(NOT "${PULP_SOURCE_DIR}/examples/${_example}" IN_LIST _configured_dirs)
            message(FATAL_ERROR "real: the retained example ${_example} was not configured in the mode")
        endif()
    endforeach()
    foreach(_required IN ITEMS pulp-gain pulp-tone PulpSynth)
        if(NOT "${PULP_SOURCE_DIR}/examples/${_required}" IN_LIST _configured_dirs)
            message(FATAL_ERROR "real: ${_required} is not among the examples the mode builds")
        endif()
    endforeach()
    set(_plugin_examples 0)
    foreach(_dir IN LISTS _configured_dirs)
        file(READ "${_dir}/CMakeLists.txt" _example_cmake)
        if(NOT _example_cmake MATCHES "pulp_add_plugin")
            continue()
        endif()
        math(EXPR _plugin_examples "${_plugin_examples} + 1")
        set(_contributed FALSE)
        foreach(_module IN LISTS _modules)
            string(REPLACE "|" ";" _fields "${_module}")
            list(GET _fields 2 _module_dir)
            string(FIND "${_module_dir}/" "${_dir}/" _at)
            if(_at EQUAL 0)
                set(_contributed TRUE)
            endif()
        endforeach()
        if(NOT _contributed)
            message(FATAL_ERROR "real: the plug-in example ${_dir} built no module in the mode")
        endif()
    endforeach()

    set(_view_pattern "[A-Za-z] pulp::(view|canvas)::")
    set(_format_pattern "[A-Za-z] pulp::format::")
    set(_script_pattern "[A-Za-z] (pulp::vellum::scripted_|vellum::view::Script(Engine|edUiSession))")
    if(NOT _marker OR NOT EXISTS "${_marker}")
        message(FATAL_ERROR "real: the pulp::view marker object was not built: '${_marker}'")
    endif()
    execute_process(COMMAND "${_nm}" -C "${_marker}" OUTPUT_VARIABLE _symbols RESULT_VARIABLE _rc ERROR_QUIET)
    string(REGEX MATCHALL "${_view_pattern}" _hits "${_symbols}")
    if(NOT _rc EQUAL 0 OR NOT _hits)
        message(FATAL_ERROR "real: nm finds no pulp::view symbol in the marker object, so the module scan is blind:\n${_symbols}")
    endif()
    file(GLOB _provider "${VELLUM_DIR}/../../libvellum-skia-dawn-provider-*.dylib")
    file(GLOB _view_script "${VELLUM_DIR}/../../../mirror/lib/libvellum-mirror-view-script-*.dylib")
    if(NOT _provider OR NOT _view_script)
        message(FATAL_ERROR "real: the Vellum provider or view-script library is missing from ${VELLUM_DIR}/../../..")
    endif()
    list(GET _provider 0 _provider)
    execute_process(COMMAND "${_nm}" -C "${_provider}" OUTPUT_VARIABLE _symbols ERROR_QUIET)
    string(REGEX MATCHALL "${_script_pattern}" _hits "${_symbols}")
    if(NOT _hits)
        message(FATAL_ERROR "real: nm finds no script engine in ${_provider}, so the scripted-editor scan is blind")
    endif()

    set(_counts "")
    set(_total_bytes 0)
    foreach(_module IN LISTS _modules)
        string(REPLACE "|" ";" _fields "${_module}")
        list(GET _fields 1 _target)
        list(GET _fields 3 _file)
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "real: ${_target} did not produce ${_file}")
        endif()
        if(_file MATCHES "^(.*\\.(clap|vst3|component|app))/Contents/MacOS/[^/]+$")
            set(_plist "${CMAKE_MATCH_1}/Contents/Info.plist")
        elseif(_file MATCHES "^(.*\\.framework)/Versions/A/[^/]+$")
            set(_plist "${CMAKE_MATCH_1}/Versions/A/Resources/Info.plist")
        else()
            message(FATAL_ERROR "real: ${_target}'s module is not inside a bundle: ${_file}")
        endif()
        if(NOT EXISTS "${_plist}")
            message(FATAL_ERROR "real: ${_target}'s bundle has no Info.plist: ${_plist}")
        endif()
        execute_process(COMMAND "${_nm}" -C "${_file}" OUTPUT_VARIABLE _symbols RESULT_VARIABLE _rc ERROR_QUIET)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "real: nm could not read ${_file}")
        endif()
        string(REGEX MATCHALL "${_view_pattern}" _hits "${_symbols}")
        if(_hits)
            list(REMOVE_DUPLICATES _hits)
            message(FATAL_ERROR "real: ${_target} carries Pulp view symbols in the mode (${_hits}): ${_file}")
        endif()
        string(REGEX MATCHALL "${_format_pattern}" _hits "${_symbols}")
        if(NOT _hits)
            message(FATAL_ERROR "real: nm finds no pulp::format symbol in ${_target}, so its scan proves nothing: ${_file}")
        endif()
        string(REGEX MATCHALL "${_script_pattern}" _hits "${_symbols}")
        execute_process(COMMAND otool -L "${_file}" OUTPUT_VARIABLE _deps ERROR_QUIET)
        if(_hits OR _deps MATCHES "libvellum-mirror-view-script")
            message(FATAL_ERROR "real: ${_target} carries a scripted editor although no example selects one: ${_file}")
        endif()
        file(SIZE "${_file}" _bytes)
        math(EXPR _total_bytes "${_total_bytes} + ${_bytes}")
        string(REGEX REPLACE "^.*_" "" _format "${_target}")
        list(APPEND _counts "${_format}")
    endforeach()
    list(LENGTH _modules _module_count)
    set(_summary "")
    foreach(_format IN ITEMS VST3 CLAP AU Standalone AUv3Framework)
        set(_n 0)
        foreach(_seen IN LISTS _counts)
            if(_seen STREQUAL _format)
                math(EXPR _n "${_n} + 1")
            endif()
        endforeach()
        string(APPEND _summary " ${_format}=${_n}")
    endforeach()
    math(EXPR _total_kib "${_total_bytes} / 1024")
    message(STATUS "real: ${_plugin_examples} plug-in examples built ${_module_count} modules in their bundles "
        "(${_summary} ), ${_total_kib} KiB of module code, none with a Pulp view symbol or a scripted editor")

    # The NO_EDITOR link floor in the mode: its editor-less modules carry
    # neither Pulp's view nor Vellum's, and its control carries Vellum's.
    execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${FIXTURE_DIR}/real-build"
            --output-on-failure -R "^(no-editor-plugin-link-floor-artifacts|no-editor-link-floor-runtime-pattern|plugin-third-party-notices)$"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0 OR NOT _out MATCHES "100% tests passed[^\n]* out of 3")
        message(FATAL_ERROR "real: the NO_EDITOR link floor fixture failed in the mode:\n${_out}${_err}")
    endif()
    message(STATUS "real: the NO_EDITOR link floor holds in the mode")

    # Plug-ins in the mode, through the real pulp_add_plugin. The probe runs
    # at the end of the top-level directory, after every Pulp target exists,
    # from a deferred include that a project include registers, so no file
    # in the source tree changes.
    #   - With no editor keyword, the CLAP module builds view-free: no
    #     PULP_CLAP_GUI, so its PulpClapPlugin layout matches the adapter
    #     pulp-format-view serves in the mode, which has no GUI extension.
    #     The same configure builds a program that links pulp::format-view
    #     alone and calls the CLAP adapter and the Vellum editor adapter, so
    #     each of those links of pulp-format-view is proved.
    #   - Every keyword that selects Pulp's own view editor, the only routes
    #     that compile a CLAP entry with PULP_CLAP_GUI, is refused at
    #     configure time.
    set(_probe_src "${FIXTURE_DIR}/real-probe-src")
    file(REMOVE_RECURSE "${_probe_src}" "${FIXTURE_DIR}/real-probe")
    file(WRITE "${_probe_src}/ui.js" "// stand-in scripted UI\n")
    file(WRITE "${_probe_src}/format_view_links.cpp"
        "#include <pulp/format/clap_contract_gui.hpp>\n"
        "#include <pulp/state/parameter.hpp>\n"
        "#include <pulp/vellum/mapping.hpp>\n"
        "int main(int argc, char**) {\n"
        "    if (argc < 1000) return 0;\n"
        "    const pulp::state::ParamInfo info{};\n"
        "    return pulp::format::clap_adapter::contract_gui_extension(nullptr) != nullptr\n"
        "        && pulp::vellum::to_normalized(info, 0.0f) > 0.0;\n"
        "}\n")
    file(WRITE "${_probe_src}/probe.cmake"
        "string(REPLACE \"=\" \";\" _keyword \"\${PULP_IN_TREE_PROBE_KEYWORD}\")\n"
        "pulp_add_plugin(InTreeProbe BUNDLE_ID dev.pulp.in-tree-probe FORMATS CLAP \${_keyword})\n"
        "get_target_property(_definitions InTreeProbe_CLAP COMPILE_DEFINITIONS)\n"
        "message(STATUS \"in-tree probe: CLAP definitions=[\${_definitions}]\")\n"
        "add_executable(pulp-in-tree-format-view-links \"${_probe_src}/format_view_links.cpp\")\n"
        "target_link_libraries(pulp-in-tree-format-view-links PRIVATE pulp::format-view)\n")
    set(_probe_include "${FIXTURE_DIR}/real-probe-include.cmake")
    file(WRITE "${_probe_include}"
        "if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)\n"
        "    cmake_language(DEFER DIRECTORY \"\${CMAKE_SOURCE_DIR}\" CALL include\n"
        "        \"${_probe_src}/probe.cmake\")\n"
        "endif()\n")
    set(_probe_common ${_common} -DPULP_UI_FROM_VELLUM_IN_TREE=ON -DPULP_ENABLE_INSPECTOR=OFF
        -DCMAKE_PROJECT_Pulp_INCLUDE=${_probe_include})

    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${PULP_SOURCE_DIR}"
            -B "${FIXTURE_DIR}/real-probe" ${_probe_common} "-DPULP_IN_TREE_PROBE_KEYWORD="
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: a CLAP plug-in with no editor keyword did not configure in the mode:\n${_out}${_err}")
    endif()
    if(NOT "${_out}" MATCHES "in-tree probe: CLAP definitions=\\[([^\n]*)\\]")
        message(FATAL_ERROR "real: the plug-in probe did not run:\n${_out}${_err}")
    endif()
    if(CMAKE_MATCH_1 MATCHES "PULP_CLAP_GUI")
        message(FATAL_ERROR "real: the default CLAP module compiles PULP_CLAP_GUI in the mode: ${CMAKE_MATCH_1}")
    endif()
    execute_process(COMMAND "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
            "${CMAKE_COMMAND}" --build "${FIXTURE_DIR}/real-probe"
            --target pulp-in-tree-format-view-links
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "real: a program linking pulp::format-view alone cannot reach the CLAP "
            "adapter and the Vellum editor adapter in the mode:\n${_out}${_err}")
    endif()
    foreach(_keyword IN ITEMS NATIVE_UI GPU_UI FULL_EDITOR LITE_EDITOR "UI_SCRIPT=${_probe_src}/ui.js")
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${PULP_SOURCE_DIR}"
                -B "${FIXTURE_DIR}/real-probe" ${_probe_common}
                "-DPULP_IN_TREE_PROBE_KEYWORD=${_keyword}"
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
        if(_rc EQUAL 0)
            message(FATAL_ERROR "real: a CLAP plug-in with ${_keyword} configured in the mode:\n${_text}")
        endif()
        _expect("${_text}" "pulp_add_plugin\\(InTreeProbe\\): this Pulp SDK serves Pulp::view from Vellum"
            "${_keyword} refused as Pulp's own view editor")
    endforeach()
    message(STATUS "real: CLAP plug-ins build view-free in the mode, every Pulp view editor keyword is refused, and pulp::format-view reaches both adapters")
    message(STATUS "real: ok")

else()
    message(FATAL_ERROR "test_vellum_ui_in_tree: unknown SCENARIO '${SCENARIO}'")
endif()
