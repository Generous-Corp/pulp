#[[
Contract test for pulp_declare_standalone_document_type().

Two halves:

  1. Argument validation, run everywhere under `cmake -P`. Each negative case
     runs in its own child process, because the function reports a bad ROLE or
     a missing UTI with message(FATAL_ERROR) — the whole point is that a
     mis-declared document type stops the build instead of vanishing.

  2. A real configure of a throwaway two-target project, macOS only, that reads
     the Info.plist CMake generated. Only that pass can prove the end-to-end
     chain, because the `${MACOSX_BUNDLE_*}` placeholders in the template are
     substituted by CMake at generate time, not by this module's
     configure_file(). It is also where the regression that matters is checked:
     an app that never calls the function must still get exactly the bundle
     keys it gets today.
]]

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_module "${_repo_root}/tools/cmake/PulpDocumentTypes.cmake")
set(_template "${_repo_root}/tools/cmake/PulpInfoPlist.standalone.in")
set(_app_targets "${_repo_root}/tools/cmake/PulpAppTargets.cmake")
set(_install_rules "${_repo_root}/tools/cmake/PulpInstallRules.cmake")
set(_reference "${_repo_root}/docs/reference/cmake.md")
set(_status "${_repo_root}/docs/status/cmake-functions.yaml")

foreach(_path IN LISTS _module _template _app_targets _install_rules _reference _status)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Required file missing: ${_path}")
    endif()
endforeach()

if(NOT PULP_DOC_TYPE_TEST_DIR)
    if(DEFINED ENV{TMPDIR})
        set(PULP_DOC_TYPE_TEST_DIR "$ENV{TMPDIR}/pulp-standalone-document-types")
    else()
        set(PULP_DOC_TYPE_TEST_DIR "/tmp/pulp-standalone-document-types")
    endif()
endif()
file(REMOVE_RECURSE "${PULP_DOC_TYPE_TEST_DIR}")
file(MAKE_DIRECTORY "${PULP_DOC_TYPE_TEST_DIR}")

set(_fail 0)

function(_expect_message_contains label haystack needle)
    if(NOT haystack MATCHES "${needle}")
        message("FAIL: ${label}: expected to find '${needle}' in:\n${haystack}")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

# CMake re-wraps message() text at its own width, so a needle that spans a
# wrap point never matches. Compare against whitespace-flattened stderr.
function(_expect_error_contains label haystack needle)
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${haystack}")
    if(NOT _flat MATCHES "${needle}")
        message("FAIL: ${label}: expected to find '${needle}' in:\n${haystack}")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

function(_expect_message_absent label haystack needle)
    if(haystack MATCHES "${needle}")
        message("FAIL: ${label}: did not expect to find '${needle}' in:\n${haystack}")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

# ── 1. Argument validation ──────────────────────────────────────────────────

function(_run_declaration label call expect_success expected_error)
    set(_script "${PULP_DOC_TYPE_TEST_DIR}/case-${label}.cmake")
    file(WRITE "${_script}" "include(\"${_module}\")\n${call}\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${_script}"
        RESULT_VARIABLE _rv
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(expect_success)
        if(NOT _rv EQUAL 0)
            message("FAIL: ${label}: expected success, got rc=${_rv}\n${_err}")
            set(_fail 1 PARENT_SCOPE)
        endif()
        return()
    endif()
    if(_rv EQUAL 0)
        message("FAIL: ${label}: expected configure failure, but it succeeded")
        set(_fail 1 PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " _err_flat "${_err}")
    if(NOT _err_flat MATCHES "${expected_error}")
        message("FAIL: ${label}: error was not the expected one: ${_err}")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

# The instrument's own control: a well-formed declaration must NOT fail. If
# this one ever starts failing, every "expected failure" below is meaningless
# because the function would be rejecting everything. It doubles as the
# non-Apple no-op check — the target does not exist and that is still fine.
# APPLE is pinned rather than inherited so both branches are exercised on
# every platform's CI, not only on a mac.
_run_declaration("non-apple-is-a-no-op"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(Ghost UTI com.example.app.project EXTENSION proj ROLE Viewer RANK None)"
    TRUE "")

_run_declaration("missing-uti"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App EXTENSION proj)"
    FALSE "UTI is required")

_run_declaration("missing-extension"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI com.example.app.project)"
    FALSE "EXTENSION is required")

# A bare word parses fine and then never routes through Launch Services.
_run_declaration("uti-not-reverse-dns"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI myproj EXTENSION proj)"
    FALSE "is not a reverse-DNS identifier")

_run_declaration("bad-role"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI com.example.app.project EXTENSION proj ROLE Owner)"
    FALSE "ROLE must be Editor, Viewer, Shell, or None")

_run_declaration("bad-rank"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI com.example.app.project EXTENSION proj RANK Editor)"
    FALSE "RANK must be Owner, Default, Alternate, or None")

# A typo'd keyword must be reported, not silently dropped. It has to sit after
# a single-value keyword to be seen as unparsed: cmake_parse_arguments keeps
# consuming into a multi-value list (EXTENSION, MIME, CONFORMS_TO) until it
# meets a keyword it knows, so a typo placed after one of those is absorbed
# into that list and caught by value validation instead.
_run_declaration("unknown-keyword"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI com.example.app.project ICON foo.icns EXTENSION proj)"
    FALSE "unrecognized argument")

_run_declaration("unknown-keyword-absorbed-into-extension-list"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI com.example.app.project EXTENSION proj ICON foo.icns)"
    FALSE "is not a bare filename extension")

_run_declaration("glob-extension"
    "set(APPLE FALSE)\npulp_declare_standalone_document_type(App UTI com.example.app.project EXTENSION *.proj)"
    FALSE "is not a bare filename extension")

# Naming the plugin target instead of its _Standalone child is the likely
# mistake, and it must not pass silently.
_run_declaration("apple-rejects-unknown-target"
    "set(APPLE TRUE)\npulp_declare_standalone_document_type(NotATarget UTI com.example.app.project EXTENSION proj)"
    FALSE "no such target")

# ── 2. Generated-plist contract (macOS only) ────────────────────────────────

if(APPLE)
    set(_project_dir "${PULP_DOC_TYPE_TEST_DIR}/project")
    set(_build_dir "${PULP_DOC_TYPE_TEST_DIR}/build")
    file(MAKE_DIRECTORY "${_project_dir}")
    file(WRITE "${_project_dir}/main.c" "int main(void) { return 0; }\n")
    file(WRITE "${_project_dir}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(PulpDocumentTypeFixture C)
include("${PULP_DOCUMENT_TYPES_MODULE}")

# The control: an app that never declares a document type. Its plist is what
# every existing Pulp standalone gets today.
add_executable(PlainApp MACOSX_BUNDLE main.c)
set_target_properties(PlainApp PROPERTIES
    OUTPUT_NAME "PlainApp"
    MACOSX_BUNDLE_BUNDLE_NAME "PlainApp"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.plain"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.2.3"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.2.3")

add_executable(DocApp MACOSX_BUNDLE main.c)
set_target_properties(DocApp PROPERTIES
    OUTPUT_NAME "DocApp"
    MACOSX_BUNDLE_BUNDLE_NAME "DocApp"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.doc"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.2.3"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.2.3")

pulp_declare_standalone_document_type(DocApp
    UTI         com.example.doc.project
    EXTENSION   docproj
    MIME        application/vnd.example.doc+zip
    DESCRIPTION "DocApp Project"
    CONFORMS_TO public.data public.archive
    ROLE        Editor
    RANK        Owner)

# Second call, same target: an app with two file types is ordinary, and the
# second declaration must add to the first rather than replace it.
pulp_declare_standalone_document_type(DocApp
    UTI         com.example.doc.preset
    EXTENSION   .docpreset
    DESCRIPTION "DocApp Preset"
    ROLE        Viewer
    RANK        None)

# Icon set AFTER the declarations. The generated template keeps CMake's own
# ${MACOSX_BUNDLE_ICON_FILE} placeholder rather than a value baked at call
# time, so pulp_app_icon() works in either order.
set_target_properties(DocApp PROPERTIES MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")

# A description carrying every character that either configure pass could eat.
# The `\$` is escaped so a LITERAL `${NOT_A_VAR}` reaches the function — an
# unescaped one would be expanded by CMake at this call site, like any other
# argument, long before the plist exists. What is under test is the pass that
# comes after: CMake configures the generated plist a second time at generate
# time, and this module configure_file()s it once itself, so a surviving
# `${...}` or `@...@` would be eaten there. `&` and `<` are plain XML hazards.
# OUTPUT_NAME carries a space, the way _pulp_add_standalone sets it from a
# plugin's display name — so the bundle directory has one too.
add_executable(NastyApp MACOSX_BUNDLE main.c)
set_target_properties(NastyApp PROPERTIES
    OUTPUT_NAME "Nasty App"
    MACOSX_BUNDLE_BUNDLE_NAME "Nasty App")
pulp_declare_standalone_document_type(NastyApp
    UTI         com.example.nasty.project
    EXTENSION   nasty
    DESCRIPTION "A & B <ok> \${NOT_A_VAR} @NOT_A_VAR@")
]])

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_project_dir}" -B "${_build_dir}"
                "-DPULP_DOCUMENT_TYPES_MODULE=${_module}"
        RESULT_VARIABLE _configure_rv
        OUTPUT_VARIABLE _configure_out
        ERROR_VARIABLE _configure_err)
    if(NOT _configure_rv EQUAL 0)
        message(FATAL_ERROR
            "Fixture configure failed (rc=${_configure_rv}):\n${_configure_out}\n${_configure_err}")
    endif()

    set(_plain_plist "${_build_dir}/PlainApp.app/Contents/Info.plist")
    set(_doc_plist "${_build_dir}/DocApp.app/Contents/Info.plist")
    foreach(_path IN LISTS _plain_plist _doc_plist)
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR "CMake did not generate the expected plist: ${_path}")
        endif()
    endforeach()
    file(READ "${_plain_plist}" _plain_text)
    file(READ "${_doc_plist}" _doc_text)

    # (3) The regression that matters: adopting the template must not change
    # the four bundle keys _pulp_add_standalone sets today. Assert them on the
    # untouched control first, then assert the same shape on the document app.
    foreach(_expected IN ITEMS
            "<key>CFBundleName</key>[ \t\r\n]*<string>PlainApp</string>"
            "<key>CFBundleIdentifier</key>[ \t\r\n]*<string>com\\.example\\.plain</string>"
            "<key>CFBundleVersion</key>[ \t\r\n]*<string>1\\.2\\.3</string>"
            "<key>CFBundleShortVersionString</key>[ \t\r\n]*<string>1\\.2\\.3</string>")
        _expect_message_contains("plain app default plist" "${_plain_text}" "${_expected}")
    endforeach()
    foreach(_expected IN ITEMS
            "<key>CFBundleName</key>[ \t\r\n]*<string>DocApp</string>"
            "<key>CFBundleIdentifier</key>[ \t\r\n]*<string>com\\.example\\.doc</string>"
            "<key>CFBundleVersion</key>[ \t\r\n]*<string>1\\.2\\.3</string>"
            "<key>CFBundleShortVersionString</key>[ \t\r\n]*<string>1\\.2\\.3</string>"
            "<key>CFBundleExecutable</key>[ \t\r\n]*<string>DocApp</string>"
            "<key>CFBundlePackageType</key>[ \t\r\n]*<string>APPL</string>")
        _expect_message_contains("document app keeps bundle keys" "${_doc_text}" "${_expected}")
    endforeach()

    # The control app must be untouched — if this leaked, the "contains" checks
    # below would pass for the wrong reason.
    _expect_message_absent("plain app stays free of document types"
        "${_plain_text}" "CFBundleDocumentTypes")
    _expect_message_absent("plain app stays free of exported types"
        "${_plain_text}" "UTExportedTypeDeclarations")

    # (1) Declared type reaches BOTH arrays with its extension.
    _expect_message_contains("document types array" "${_doc_text}" "<key>CFBundleDocumentTypes</key>")
    _expect_message_contains("exported types array" "${_doc_text}" "<key>UTExportedTypeDeclarations</key>")
    _expect_message_contains("declared content type" "${_doc_text}"
        "<key>LSItemContentTypes</key>[ \t\r\n]*<array>[ \t\r\n]*<string>com\\.example\\.doc\\.project</string>")
    _expect_message_contains("exported identifier" "${_doc_text}"
        "<key>UTTypeIdentifier</key>[ \t\r\n]*<string>com\\.example\\.doc\\.project</string>")
    _expect_message_contains("exported extension" "${_doc_text}"
        "<key>public\\.filename-extension</key>[ \t\r\n]*<array>[ \t\r\n]*<string>docproj</string>")
    _expect_message_contains("exported mime type" "${_doc_text}"
        "<key>public\\.mime-type</key>[ \t\r\n]*<array>[ \t\r\n]*<string>application/vnd\\.example\\.doc\\+zip</string>")
    _expect_message_contains("declared conformance" "${_doc_text}" "<string>public\\.archive</string>")
    _expect_message_contains("declared role" "${_doc_text}"
        "<key>CFBundleTypeRole</key>[ \t\r\n]*<string>Editor</string>")
    _expect_message_contains("declared rank" "${_doc_text}"
        "<key>LSHandlerRank</key>[ \t\r\n]*<string>Owner</string>")

    # (2) The second call accumulated rather than replacing the first.
    _expect_message_contains("second type identifier" "${_doc_text}"
        "<key>UTTypeIdentifier</key>[ \t\r\n]*<string>com\\.example\\.doc\\.preset</string>")
    # A leading dot on EXTENSION is normalized away, not emitted verbatim.
    _expect_message_contains("second type extension" "${_doc_text}"
        "<string>docpreset</string>")
    _expect_message_absent("leading dot normalized" "${_doc_text}" "<string>\\.docpreset</string>")
    # RANK None must be expressible: a helper app that opens a type but must
    # never become its default handler.
    _expect_message_contains("rank none" "${_doc_text}"
        "<key>LSHandlerRank</key>[ \t\r\n]*<string>None</string>")
    _expect_message_contains("second role" "${_doc_text}"
        "<key>CFBundleTypeRole</key>[ \t\r\n]*<string>Viewer</string>")

    string(REGEX MATCHALL "<key>UTTypeIdentifier</key>" _identifier_hits "${_doc_text}")
    list(LENGTH _identifier_hits _identifier_count)
    if(NOT _identifier_count EQUAL 2)
        message("FAIL: expected 2 exported type declarations, found ${_identifier_count}")
        set(_fail 1)
    endif()
    string(REGEX MATCHALL "<key>LSItemContentTypes</key>" _content_hits "${_doc_text}")
    list(LENGTH _content_hits _content_count)
    if(NOT _content_count EQUAL 2)
        message("FAIL: expected 2 document type entries, found ${_content_count}")
        set(_fail 1)
    endif()

    set(_nasty_plist "${_build_dir}/Nasty App.app/Contents/Info.plist")
    if(NOT EXISTS "${_nasty_plist}")
        message(FATAL_ERROR "CMake did not generate the expected plist: ${_nasty_plist}")
    endif()
    file(READ "${_nasty_plist}" _nasty_text)
    _expect_message_contains("spaced bundle name reaches the plist" "${_nasty_text}"
        "<key>CFBundleName</key>[ \t\r\n]*<string>Nasty App</string>")
    _expect_message_contains("spaced executable name reaches the plist" "${_nasty_text}"
        "<key>CFBundleExecutable</key>[ \t\r\n]*<string>Nasty App</string>")

    # pulp_app_icon() may run after the declarations; the icon must still land.
    _expect_message_contains("icon survives a later pulp_app_icon" "${_doc_text}"
        "<key>CFBundleIconFile</key>[ \t\r\n]*<string>AppIcon\.icns</string>")

    # The generated plist has to survive an actual property-list parse, and the
    # hostile description has to come back out byte for byte. A stray unescaped
    # character would otherwise only surface when macOS refuses to launch the
    # app, and a swallowed `${...}` would silently truncate the type's name.
    find_program(_plutil plutil)
    if(_plutil)
        foreach(_path IN LISTS _doc_plist _nasty_plist)
            execute_process(COMMAND "${_plutil}" -lint "${_path}"
                RESULT_VARIABLE _lint_rv OUTPUT_VARIABLE _lint_out ERROR_VARIABLE _lint_err)
            if(NOT _lint_rv EQUAL 0)
                message("FAIL: generated plist is not valid (${_path}): ${_lint_out}${_lint_err}")
                set(_fail 1)
            endif()
        endforeach()
        execute_process(
            COMMAND "${_plutil}" -extract
                    UTExportedTypeDeclarations.0.UTTypeDescription raw -o -
                    "${_nasty_plist}"
            RESULT_VARIABLE _extract_rv
            OUTPUT_VARIABLE _extract_out
            ERROR_VARIABLE _extract_err
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _extract_rv EQUAL 0)
            message("FAIL: could not read back the exported type description: ${_extract_err}")
            set(_fail 1)
        elseif(NOT _extract_out STREQUAL "A & B <ok> \${NOT_A_VAR} @NOT_A_VAR@")
            message("FAIL: description did not round-trip; got '${_extract_out}'")
            set(_fail 1)
        endif()
    else()
        message("FAIL: plutil not found; the generated plist was never parsed")
        set(_fail 1)
    endif()

    # A second declaration of the SAME UTI is a mistake, not an accumulation.
    file(WRITE "${_project_dir}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(PulpDocumentTypeDuplicate C)
include("${PULP_DOCUMENT_TYPES_MODULE}")
add_executable(DupApp MACOSX_BUNDLE main.c)
pulp_declare_standalone_document_type(DupApp UTI com.example.dup.project EXTENSION a)
pulp_declare_standalone_document_type(DupApp UTI com.example.dup.project EXTENSION b)
]])
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_project_dir}" -B "${PULP_DOC_TYPE_TEST_DIR}/build-dup"
                "-DPULP_DOCUMENT_TYPES_MODULE=${_module}"
        RESULT_VARIABLE _dup_rv OUTPUT_VARIABLE _dup_out ERROR_VARIABLE _dup_err)
    if(_dup_rv EQUAL 0)
        message("FAIL: duplicate UTI declaration should fail configure")
        set(_fail 1)
    else()
        _expect_error_contains("duplicate uti" "${_dup_err}" "is already declared on this target")
    endif()

    # A non-bundle target has no Info.plist, so accepting the call would drop
    # the declaration on the floor.
    file(WRITE "${_project_dir}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(PulpDocumentTypeNonBundle C)
include("${PULP_DOCUMENT_TYPES_MODULE}")
add_executable(FlatApp main.c)
pulp_declare_standalone_document_type(FlatApp UTI com.example.flat.project EXTENSION a)
]])
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_project_dir}" -B "${PULP_DOC_TYPE_TEST_DIR}/build-flat"
                "-DPULP_DOCUMENT_TYPES_MODULE=${_module}"
        RESULT_VARIABLE _flat_rv OUTPUT_VARIABLE _flat_out ERROR_VARIABLE _flat_err)
    if(_flat_rv EQUAL 0)
        message("FAIL: non-bundle target should fail configure")
        set(_fail 1)
    else()
        _expect_error_contains("non-bundle target" "${_flat_err}"
            "is not a macOS application bundle")
    endif()
endif()

# ── 3. Wiring and docs ──────────────────────────────────────────────────────

file(READ "${_app_targets}" _app_targets_text)
if(NOT _app_targets_text MATCHES "PulpDocumentTypes\\.cmake")
    message(FATAL_ERROR "PulpAppTargets.cmake must include PulpDocumentTypes.cmake")
endif()

# An SDK consumer that gets the module but not its template would fail at the
# configure_file() with a path nobody recognizes.
file(READ "${_install_rules}" _install_text)
foreach(_shipped IN ITEMS "PulpDocumentTypes.cmake" "PulpInfoPlist.standalone.in")
    if(NOT _install_text MATCHES "${_shipped}")
        message(FATAL_ERROR "PulpInstallRules.cmake must install ${_shipped}")
    endif()
endforeach()

# The template must keep CMake's own placeholders rather than baked values, or
# an app's icon/name would freeze at whatever was set when the call ran.
file(READ "${_template}" _template_text)
foreach(_placeholder IN ITEMS
        "MACOSX_BUNDLE_EXECUTABLE_NAME"
        "MACOSX_BUNDLE_GUI_IDENTIFIER"
        "MACOSX_BUNDLE_BUNDLE_NAME"
        "MACOSX_BUNDLE_BUNDLE_VERSION"
        "MACOSX_BUNDLE_SHORT_VERSION_STRING"
        "MACOSX_BUNDLE_ICON_FILE")
    if(NOT _template_text MATCHES "\\$\\{${_placeholder}\\}")
        message(FATAL_ERROR
            "PulpInfoPlist.standalone.in must keep the \${${_placeholder}} placeholder")
    endif()
endforeach()

file(READ "${_reference}" _reference_text)
if(NOT _reference_text MATCHES "pulp_declare_standalone_document_type")
    message(FATAL_ERROR
        "docs/reference/cmake.md must document pulp_declare_standalone_document_type")
endif()
file(READ "${_status}" _status_text)
if(NOT _status_text MATCHES "name: pulp_declare_standalone_document_type")
    message(FATAL_ERROR
        "docs/status/cmake-functions.yaml must list pulp_declare_standalone_document_type")
endif()

if(_fail)
    message(FATAL_ERROR "Standalone document-type contract failed.")
endif()

file(REMOVE_RECURSE "${PULP_DOC_TYPE_TEST_DIR}")
message(STATUS "Standalone document-type declaration contract: OK")
