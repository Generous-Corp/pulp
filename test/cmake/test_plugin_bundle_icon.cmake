#[[
Contract test for the plug-in product icon.

Two halves, because neither proves the thing alone:

  1. A real configure of a throwaway project, macOS only, that attaches an
     icon to bundle targets built from Pulp's OWN plug-in Info.plist
     templates, then reads the Info.plist CMake generated. Only that pass
     can prove the chain, because the template's ${MACOSX_BUNDLE_ICON_FILE}
     placeholder is substituted by CMake at GENERATE time from a target
     property -- not by the configure_file() in PulpPluginFormats.cmake,
     which runs @ONLY and leaves the placeholder alone. The templates have
     carried the key since it was added; what was missing was anything that
     filled it, so a test that only checked the key's PRESENCE would have
     passed against an empty string the whole time. This asserts the VALUE.

  2. A source lint that pulp_add_plugin() applies the icon to EVERY bundle
     it creates. The configure pass above proves one target can be wired; it
     cannot prove the public API wires all five, and VST3 / AU / CLAP do not
     share a code path.

Scope: this asserts the asset SHIPS, never that macOS draws it. It does not,
for a non-.app bundle. See the product-icon comment in PulpUtils.cmake.
]]

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_icon_module "${_repo_root}/tools/cmake/PulpAppIcon.cmake")
set(_utils "${_repo_root}/tools/cmake/PulpUtils.cmake")

foreach(_p IN LISTS _icon_module _utils)
    if(NOT EXISTS "${_p}")
        message(FATAL_ERROR "Required file missing: ${_p}")
    endif()
endforeach()

# ── Half 2 (runs everywhere): the public API wires every format ────────
file(READ "${_utils}" _utils_text)

string(FIND "${_utils_text}" "ICON;ICNS" _accepts)
if(_accepts EQUAL -1)
    message(FATAL_ERROR
        "pulp_add_plugin() does not accept ICON/ICNS. The plug-in plist "
        "templates carry a CFBundleIconFile key with nothing able to fill "
        "it, so a consumer must reach past the public API and set "
        "MACOSX_BUNDLE_ICON_FILE on internal target names by hand.")
endif()

set(_missing "")
foreach(_suffix Standalone VST3 AU CLAP AAX)
    string(FIND "${_utils_text}" "\${target}_${_suffix}" _found_target)
    if(_found_target EQUAL -1)
        list(APPEND _missing "${_suffix}")
    endif()
endforeach()
if(_missing)
    string(REPLACE ";" ", " _rendered "${_missing}")
    message(FATAL_ERROR
        "pulp_add_plugin() icon loop omits: ${_rendered}")
endif()

# The loop must actually call the attach helper, not merely name the targets.
string(FIND "${_utils_text}" "_pulp_icon_configure_macos(\${_pulp_icon_bundle}" _calls)
if(_calls EQUAL -1)
    message(FATAL_ERROR
        "pulp_add_plugin() names the bundle targets but never calls "
        "_pulp_icon_configure_macos() on them.")
endif()

# ── Half 1 (macOS): the key is FILLED in a generated plug-in plist ─────
if(APPLE)
    if(NOT PULP_PLUGIN_ICON_TEST_DIR)
        if(DEFINED ENV{TMPDIR})
            set(PULP_PLUGIN_ICON_TEST_DIR "$ENV{TMPDIR}/pulp-plugin-bundle-icon")
        else()
            set(PULP_PLUGIN_ICON_TEST_DIR "/tmp/pulp-plugin-bundle-icon")
        endif()
    endif()
    set(_pdir "${PULP_PLUGIN_ICON_TEST_DIR}/project")
    set(_bdir "${PULP_PLUGIN_ICON_TEST_DIR}/build")
    file(REMOVE_RECURSE "${PULP_PLUGIN_ICON_TEST_DIR}")
    file(MAKE_DIRECTORY "${_pdir}")
    file(WRITE "${_pdir}/main.c" "int f(void) { return 0; }\n")

    # A minimal but real .icns, so the attach path is exercised on a file
    # macOS recognises rather than an empty placeholder.
    file(WRITE "${_pdir}/Fixture.icns" "")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E echo "icns"
        OUTPUT_FILE "${_pdir}/Fixture.icns")

    file(WRITE "${_pdir}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(PulpPluginIconFixture C)
include("${PULP_ICON_MODULE}")

# Three bundles built the way _pulp_add_vst3 / _pulp_add_au / _pulp_add_clap
# build theirs: a MODULE library, BUNDLE TRUE, and Pulp's own template as
# MACOSX_BUNDLE_INFO_PLIST. The template is what carries the key under test.
set(PULP_PLUGIN_NAME "Fixture")
set(PULP_BUNDLE_ID "com.example.fixture")
set(PULP_VERSION "1.0.0")
set(PULP_MANUFACTURER "Example")
set(PULP_MANUFACTURER_CODE "Exmp")
set(PULP_PLUGIN_CODE "Fixt")
set(PULP_AU_TYPE "aufx")
set(PULP_AU_FACTORY_NAME "FixtureAUFactory")
set(PULP_AU_VERSION_INT "65536")

foreach(_fmt vst3 au clap)
    if(_fmt STREQUAL "vst3")
        set(_tgt Fixture_VST3)
    elseif(_fmt STREQUAL "au")
        set(_tgt Fixture_AU)
    else()
        set(_tgt Fixture_CLAP)
    endif()
    add_library(${_tgt} MODULE main.c)
    configure_file("${PULP_TEMPLATE_DIR}/PulpInfoPlist.${_fmt}.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/${_tgt}_Info.plist" @ONLY)
    set_target_properties(${_tgt} PROPERTIES
        BUNDLE TRUE
        OUTPUT_NAME "Fixture"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_BINARY_DIR}/${_tgt}_Info.plist")
    if(NOT PULP_ICON_FIXTURE_OMIT_ICON)
        _pulp_icon_configure_macos(${_tgt} "" "${CMAKE_CURRENT_SOURCE_DIR}/Fixture.icns")
    endif()
    # Dump what generate-time actually produced, so the assertions read
    # CMake's output rather than this file's intent.
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/icon-${_tgt}.txt"
        CONTENT "key=$<TARGET_PROPERTY:${_tgt},MACOSX_BUNDLE_ICON_FILE>\nsources=$<TARGET_PROPERTY:${_tgt},SOURCES>\n")
endforeach()
]])

    get_filename_component(_template_dir "${_repo_root}/tools/cmake" ABSOLUTE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_pdir}" -B "${_bdir}"
                "-DPULP_ICON_MODULE=${_icon_module}"
                "-DPULP_TEMPLATE_DIR=${_template_dir}"
                "-DPULP_ICON_FIXTURE_OMIT_ICON=${PULP_ICON_FIXTURE_OMIT_ICON}"
        RESULT_VARIABLE _rv OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "Fixture configure failed (rc=${_rv}):\n${_out}\n${_err}")
    endif()

    set(_failures "")
    foreach(_tgt Fixture_VST3 Fixture_AU Fixture_CLAP)
        set(_dump "${_bdir}/icon-${_tgt}.txt")
        if(NOT EXISTS "${_dump}")
            list(APPEND _failures "${_tgt}: no generate-time dump")
            continue()
        endif()
        file(READ "${_dump}" _d)
        if(NOT _d MATCHES "key=Fixture\\.icns")
            list(APPEND _failures
                "${_tgt}: MACOSX_BUNDLE_ICON_FILE is not Fixture.icns")
        endif()
        if(NOT _d MATCHES "Fixture\\.icns.*\n?.*" OR NOT _d MATCHES "sources=[^\n]*Fixture\\.icns")
            list(APPEND _failures "${_tgt}: the .icns is not attached as a source")
        endif()

        # The end-to-end half: the key in the plist CMake generated must be
        # NON-EMPTY. Presence proves nothing -- the templates always emit it.
        file(GLOB_RECURSE _plists "${_bdir}/*${_tgt}_Info.plist")
        set(_generated "")
        foreach(_p IN LISTS _plists)
            file(READ "${_p}" _ptext)
            if(_ptext MATCHES "CFBundleIconFile")
                set(_generated "${_p}")
            endif()
        endforeach()
        if(_generated)
            file(READ "${_generated}" _ptext)
            if(_ptext MATCHES "<key>CFBundleIconFile</key>[ \t\r\n]*<string></string>")
                list(APPEND _failures
                    "${_tgt}: CFBundleIconFile is present but EMPTY in the generated plist")
            endif()
        endif()
    endforeach()

    if(_failures)
        string(REPLACE ";" "\n  " _r "${_failures}")
        message(FATAL_ERROR "Plug-in bundle icon not wired:\n  ${_r}")
    endif()
endif()

message(STATUS "plugin_bundle_icon_verified=true")
