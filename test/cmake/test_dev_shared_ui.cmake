#[[
Contract test for PULP_DEV_SHARED_UI (tools/cmake/PulpDevSharedUi.cmake and
tools/cmake/scripts/pulp_dev_shared_link.sh).

Builds a throwaway project whose stand-in archives carry the real naming the
link helper keys on: libpulp-view.a (format-independent, goes shared),
libpulp-format-core.a (an adapter, stays static and calls a hook the module
defines) and libthird.a (third-party, resolved normally). A "module" links all
three through _pulp_dev_shared_ui_attach(), the way a VST3 or CLAP binary does.
A second module's adapter needs a libthird.a member the dylib never loaded, so
its link must fall back from the shortcut that leaves third-party archives out.

With the option ON the module must:
  - load the shared runtime by its @rpath install name and carry an rpath to it;
  - leave the Pulp symbol undefined (resolved from the dylib, no second copy)
    while still defining the adapter symbol itself;
  - load and run: module -> adapter -> shared Pulp code, the third-party code
    the dylib already holds, and the hook the module defines;
and the dylib must export a member nothing references (it was force-loaded).
The second module must load, carry its own copy of the member only it needs,
and be marked so later links skip the shortcut.
With the option OFF (the control) the module defines the Pulp symbol and loads
no dylib, so the assertions above cannot pass for free.

Both single-config generators are exercised: Unix Makefiles links each target
from its own directory, so archive paths differ from Ninja's. A multi-config
generator must fall back to a static link and record the option as OFF.

The helper runs directly against fake linkers too: a response file and a
missing archive record both fail, an unchanged record keeps its timestamp so
modules do not relink for nothing, the exact rewritten link lines, and a
shortcut link that fails is redone in full once and then skipped.

macOS only, like the option.
]]

if(NOT CMAKE_HOST_APPLE)
    message(STATUS "dev-shared ui: not an Apple host; skipping")
    return()
endif()

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT PULP_DEV_SHARED_TEST_DIR)
    set(PULP_DEV_SHARED_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/dev-shared-ui")
endif()
set(_link "${_repo_root}/tools/cmake/scripts/pulp_dev_shared_link.sh")
set(_src "${PULP_DEV_SHARED_TEST_DIR}/src")
file(REMOVE_RECURSE "${PULP_DEV_SHARED_TEST_DIR}")
file(MAKE_DIRECTORY "${_src}")

file(WRITE "${_src}/view.c" "
int pulp_fixture_third(void);
int pulp_fixture_view(int x) { return x * 10 + pulp_fixture_third(); }
")
file(WRITE "${_src}/view_unused.c" "int pulp_fixture_unreferenced(void) { return 7; }\n")
file(WRITE "${_src}/third.c" "int pulp_fixture_third(void) { return 3; }\n")
file(WRITE "${_src}/third_extra.c" "int pulp_fixture_third_extra(void) { return 5; }\n")
file(WRITE "${_src}/format_extra.c" "
int pulp_fixture_third_extra(void);
int pulp_fixture_entry2(void) { return pulp_fixture_third_extra(); }
")
file(WRITE "${_src}/module2.c" "
int pulp_fixture_entry2(void);
int pulp_fixture_module_entry(void) { return pulp_fixture_entry2(); }
")
file(WRITE "${_src}/format.c" "
int pulp_fixture_view(int x);
int pulp_fixture_third(void);
int pulp_fixture_plugin_hook(void);
int pulp_fixture_entry(void) {
    return pulp_fixture_view(4) + pulp_fixture_third() + pulp_fixture_plugin_hook();
}
")
file(WRITE "${_src}/module.c" "
int pulp_fixture_entry(void);
int pulp_fixture_plugin_hook(void) { return 2; }
int pulp_fixture_module_entry(void) { return pulp_fixture_entry(); }
")
file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.24)
project(pulp_dev_shared_fixture VERSION 9.8.7 LANGUAGES C)
set(Pulp_VERSION 9.8.7)
include(\"${_repo_root}/tools/cmake/PulpDevSharedUi.cmake\")
add_library(third STATIC third.c third_extra.c)
add_library(pulp-view STATIC view.c view_unused.c)
target_link_libraries(pulp-view PUBLIC third)
add_library(pulp-format-core STATIC format.c)
target_link_libraries(pulp-format-core PUBLIC pulp-view)
set(_PULP_FORMAT_TARGET pulp-format-core)
set(_PULP_VIEW_TARGET pulp-view)
set(_PULP_STANDALONE_TARGET \"\")
set(PULP_Fixture_VIEW_TARGET pulp-view)
add_library(Fixture_VST3 MODULE module.c)
target_link_libraries(Fixture_VST3 PRIVATE pulp-format-core)
_pulp_dev_shared_ui_attach(Fixture Fixture_VST3 _attached)
file(WRITE \"\${CMAKE_BINARY_DIR}/attached.txt\" \"\${_attached}\")
add_library(pulp-format-extra STATIC format_extra.c)
target_link_libraries(pulp-format-extra PUBLIC pulp-view third)
set(PULP_Fixture2_VIEW_TARGET pulp-view)
add_library(Fixture2_VST3 MODULE module2.c)
target_link_libraries(Fixture2_VST3 PRIVATE pulp-format-extra)
_pulp_dev_shared_ui_attach(Fixture2 Fixture2_VST3 _attached2)
")

find_program(_otool otool REQUIRED)
find_program(_nm nm REQUIRED)
find_program(_python3 python3 REQUIRED)

# Calls pulp_fixture_module_entry() through dlopen: view(4) = 4 * 10 + third,
# plus third again and the module's hook, so 40 + 3 + 3 + 2 = 48.
set(_loader "${PULP_DEV_SHARED_TEST_DIR}/load.py")
file(WRITE "${_loader}" "
import ctypes, sys
module = ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)
print(module.pulp_fixture_module_entry())
")

function(_dev_shared_case label generator option)
    set(_bin "${PULP_DEV_SHARED_TEST_DIR}/${label}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}" -G "${generator}"
            -DCMAKE_BUILD_TYPE=Release -DPULP_DEV_SHARED_UI=${option}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${label}: configure failed\n${_out}\n${_err}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_bin}" -j 2
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${label}: build failed\n${_out}\n${_err}")
    endif()
    file(READ "${_bin}/attached.txt" _attached)
    set(_module "${_bin}/Fixture_VST3.so")
    if(NOT EXISTS "${_module}")
        set(_module "${_bin}/libFixture_VST3.so")
    endif()
    execute_process(COMMAND "${_otool}" -L "${_module}" OUTPUT_VARIABLE _deps)
    execute_process(COMMAND "${_otool}" -l "${_module}" OUTPUT_VARIABLE _loads)
    execute_process(COMMAND "${_nm}" -m "${_module}" OUTPUT_VARIABLE _syms)
    execute_process(COMMAND "${_python3}" "${_loader}" "${_module}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _result ERROR_VARIABLE _err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0 OR NOT _result STREQUAL "48")
        message(FATAL_ERROR "${label}: module did not load and run (rc ${_rc}, got '${_result}')\n${_err}")
    endif()
    string(REGEX MATCH "\\(__TEXT,__text\\) external _pulp_fixture_view" _view_defined "${_syms}")
    string(REGEX MATCH "\\(__TEXT,__text\\) external _pulp_fixture_entry" _entry_defined "${_syms}")
    string(REGEX MATCH "\\(__TEXT,__text\\) external _pulp_fixture_third" _third_defined "${_syms}")
    if(NOT _entry_defined)
        message(FATAL_ERROR "${label}: the adapter archive must stay static in the module\n${_syms}")
    endif()

    if(option)
        set(_dylib "${_bin}/pulp-dev-shared-ui/libpulp-dev-shared-ui.9.8.7.dylib")
        if(NOT _attached)
            message(FATAL_ERROR "${label}: _pulp_dev_shared_ui_attach did not attach")
        endif()
        string(FIND "${_deps}" "@rpath/libpulp-dev-shared-ui.9.8.7.dylib" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "${label}: module does not load the shared runtime\n${_deps}")
        endif()
        string(FIND "${_loads}" "${_bin}/pulp-dev-shared-ui" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "${label}: module has no rpath to the shared runtime\n${_loads}")
        endif()
        if(_view_defined)
            message(FATAL_ERROR "${label}: the module carries its own copy of shared Pulp code\n${_syms}")
        endif()
        # The adapter also calls the third-party archive directly. The dylib
        # already holds that member, and precedes the archive on the module's
        # link line, so the module must not load a second copy.
        if(_third_defined)
            message(FATAL_ERROR "${label}: the module loaded a second copy of a third-party member\n${_syms}")
        endif()
        execute_process(COMMAND "${_nm}" -gU "${_dylib}" OUTPUT_VARIABLE _exports)
        string(FIND "${_exports}" "_pulp_fixture_unreferenced" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "${label}: the dylib did not force-load the whole Pulp archive\n${_exports}")
        endif()
        string(FIND "${_exports}" "_pulp_fixture_entry" _at)
        if(NOT _at EQUAL -1)
            message(FATAL_ERROR "${label}: the adapter archive leaked into the shared runtime\n${_exports}")
        endif()
        file(READ "${_bin}/pulp-dev-shared-ui/archives.txt" _record)
        if(NOT _record STREQUAL "libpulp-view.a\n")
            message(FATAL_ERROR "${label}: unexpected archive record '${_record}'")
        endif()
    else()
        if(_attached)
            message(FATAL_ERROR "${label}: attached with the option off")
        endif()
        string(FIND "${_deps}" "libpulp-dev-shared-ui" _at)
        if(NOT _at EQUAL -1 OR NOT _view_defined)
            message(FATAL_ERROR "${label}: a static module must carry Pulp code and no shared runtime\n${_deps}\n${_syms}")
        endif()
    endif()
    set(_module2 "${_bin}/libFixture2_VST3.so")
    execute_process(COMMAND "${_python3}" "${_loader}" "${_module2}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _result2 ERROR_VARIABLE _err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0 OR NOT _result2 STREQUAL "5")
        message(FATAL_ERROR "${label}: second module did not load and run (rc ${_rc}, got '${_result2}')\n${_err}")
    endif()
    if(option)
        execute_process(COMMAND "${_nm}" -m "${_module2}" OUTPUT_VARIABLE _syms2)
        if(NOT _syms2 MATCHES "\\(__TEXT,__text\\) external _pulp_fixture_third_extra")
            message(FATAL_ERROR "${label}: the fallback link did not take the member only the module needs\n${_syms2}")
        endif()
        file(GLOB _markers "${_bin}/pulp-dev-shared-ui/archives.txt.full-link/*")
        list(LENGTH _markers _marker_count)
        if(NOT _marker_count EQUAL 1 OR NOT _markers MATCHES "Fixture2_VST3")
            message(FATAL_ERROR "${label}: expected one full-link marker, for Fixture2 only: '${_markers}'")
        endif()
        file(READ "${_bin}/pulp-dev-shared-ui/archives.txt.resolved" _resolved)
        if(NOT _resolved STREQUAL "libthird.a\n")
            message(FATAL_ERROR "${label}: unexpected resolved-archive record '${_resolved}'")
        endif()
    endif()
    message(STATUS "dev-shared ui: ${label} ok")
endfunction()

find_program(_ninja ninja)
_dev_shared_case(makefiles-on "Unix Makefiles" ON)
_dev_shared_case(makefiles-off "Unix Makefiles" OFF)
if(_ninja)
    _dev_shared_case(ninja-on "Ninja" ON)
    # A multi-config generator cannot carry the link helper: the configure
    # warns, records OFF (so the ship guards do not refuse a static build), and
    # attaches nothing.
    set(_multi "${PULP_DEV_SHARED_TEST_DIR}/ninja-multi")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_multi}" -G "Ninja Multi-Config"
            -DPULP_DEV_SHARED_UI=ON
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    file(STRINGS "${_multi}/CMakeCache.txt" _cached REGEX "^PULP_DEV_SHARED_UI:")
    file(READ "${_multi}/attached.txt" _attached)
    if(NOT _rc EQUAL 0 OR NOT _cached STREQUAL "PULP_DEV_SHARED_UI:BOOL=OFF" OR _attached
       OR NOT _err MATCHES "PULP_DEV_SHARED_UI is ignored")
        message(FATAL_ERROR "multi-config: expected a warning and OFF (rc ${_rc}, cache '${_cached}', attached '${_attached}')\n${_err}")
    endif()
    message(STATUS "dev-shared ui: ninja-multi-config falls back ok")
else()
    message(STATUS "dev-shared ui: ninja not found; Ninja cases skipped")
endif()

# Direct refusals and record stability, against a fake linker.
set(_fake "${PULP_DEV_SHARED_TEST_DIR}/fake-ld.sh")
file(WRITE "${_fake}" "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"${PULP_DEV_SHARED_TEST_DIR}/fake-ld.argv\"\n")
file(CHMOD "${_fake}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(_record "${PULP_DEV_SHARED_TEST_DIR}/record.txt")

execute_process(COMMAND /bin/bash "${_link}" dylib "${_record}" "${_fake}" @args.rsp
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "response file")
    message(FATAL_ERROR "a response file must be refused (rc ${_rc}): ${_err}")
endif()
execute_process(COMMAND /bin/bash "${_link}" module /x/libpulp-dev-shared-ui.1.dylib
        "${_record}" "${_fake}" a.o
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "is missing")
    message(FATAL_ERROR "a module link without the archive record must fail (rc ${_rc}): ${_err}")
endif()

execute_process(COMMAND /bin/bash "${_link}" dylib "${_record}" "${_fake}" -dynamiclib
        lib/libpulp-view.a lib/libpulp-format-core.a lib/libpulp-view.a
        lib/libpulp-standalone.a lib/libvst3-sdk.a lib/libskia.a -framework Cocoa
    RESULT_VARIABLE _rc)
file(STRINGS "${PULP_DEV_SHARED_TEST_DIR}/fake-ld.argv" _argv)
set(_want -dynamiclib -Wl,-force_load,lib/libpulp-view.a lib/libskia.a -framework Cocoa)
if(NOT _rc EQUAL 0 OR NOT _argv STREQUAL _want)
    message(FATAL_ERROR "dylib link line: got '${_argv}', want '${_want}'")
endif()
file(TIMESTAMP "${_record}" _first "%s")
file(TIMESTAMP "${_record}.resolved" _first_resolved "%s")
execute_process(COMMAND /bin/sleep 1.1)
execute_process(COMMAND /bin/bash "${_link}" dylib "${_record}" "${_fake}" -dynamiclib
        lib/libpulp-view.a lib/libskia.a)
file(TIMESTAMP "${_record}" _second "%s")
file(TIMESTAMP "${_record}.resolved" _second_resolved "%s")
if(NOT _first STREQUAL _second OR NOT _first_resolved STREQUAL _second_resolved)
    message(FATAL_ERROR "an unchanged archive record was rewritten")
endif()

execute_process(COMMAND /bin/bash "${_link}" module /x/rt/libpulp-dev-shared-ui.1.dylib
        "${_record}" "${_fake}" -bundle a.o ../lib/libpulp-format-core.a
        ../lib/libpulp-view.a lib/libskia.a -o out
    RESULT_VARIABLE _rc)
file(STRINGS "${PULP_DEV_SHARED_TEST_DIR}/fake-ld.argv" _argv)
set(_want -bundle a.o /x/rt/libpulp-dev-shared-ui.1.dylib ../lib/libpulp-format-core.a
    -o out -Wl,-rpath,/x/rt)
if(NOT _rc EQUAL 0 OR NOT _argv STREQUAL _want)
    message(FATAL_ERROR "module shortcut link line: got '${_argv}', want '${_want}'")
endif()

# A shortcut that fails is redone with the third-party archives, and the module
# is marked so its next link goes straight to the full line.
set(_picky "${PULP_DEV_SHARED_TEST_DIR}/picky-ld.sh")
file(WRITE "${_picky}" "#!/bin/sh
printf '%s\\n' \"$@\" > \"${PULP_DEV_SHARED_TEST_DIR}/fake-ld.argv\"
echo call >> \"${PULP_DEV_SHARED_TEST_DIR}/picky-ld.calls\"
case \" $* \" in *\" lib/libskia.a \"*) exit 0 ;; esac
echo 'Undefined symbols: _sk_only' >&2
exit 1
")
file(CHMOD "${_picky}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
foreach(_attempt 1 2)
    execute_process(COMMAND /bin/bash "${_link}" module /x/rt/libpulp-dev-shared-ui.1.dylib
            "${_record}" "${_picky}" -bundle a.o lib/libskia.a -o picky/out
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    file(STRINGS "${PULP_DEV_SHARED_TEST_DIR}/picky-ld.calls" _calls)
    list(LENGTH _calls _call_count)
    if(_attempt EQUAL 1)
        set(_want_calls 2)
    else()
        set(_want_calls 3)
    endif()
    if(NOT _rc EQUAL 0 OR NOT _call_count EQUAL _want_calls OR _err MATCHES "Undefined")
        message(FATAL_ERROR "fallback attempt ${_attempt}: rc ${_rc}, ${_call_count} linker calls (want ${_want_calls})\n${_err}")
    endif()
endforeach()
if(NOT EXISTS "${_record}.full-link/picky_out")
    message(FATAL_ERROR "the fallback did not mark the module")
endif()

# A link that allows undefined symbols never takes the shortcut.
file(REMOVE "${PULP_DEV_SHARED_TEST_DIR}/picky-ld.calls")
execute_process(COMMAND /bin/bash "${_link}" module /x/rt/libpulp-dev-shared-ui.1.dylib
        "${_record}" "${_fake}" -bundle a.o lib/libskia.a -undefined dynamic_lookup -o lookup
    RESULT_VARIABLE _rc)
file(STRINGS "${PULP_DEV_SHARED_TEST_DIR}/fake-ld.argv" _argv)
list(FIND _argv lib/libskia.a _skia_at)
if(NOT _rc EQUAL 0 OR _skia_at EQUAL -1)
    message(FATAL_ERROR "a dynamic_lookup link must keep the third-party archives: '${_argv}'")
endif()
message(STATUS "dev-shared ui: link helper ok")
