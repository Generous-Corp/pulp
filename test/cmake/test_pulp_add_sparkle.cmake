# pulp_add_sparkle(): configure-time contract, checked without a network or a
# real Sparkle download (DIST_DIR points at a stub distribution, and nothing is
# built, so the stub framework is never linked).
#   * the Sparkle keys land in the app's Info.plist template, whether the
#     document-type helper ran before or after the call,
#   * a non-app target (a plug-in bundle) is refused,
#   * a private-key-shaped value or a plain-http feed is refused.
cmake_minimum_required(VERSION 3.24)

if(NOT CMAKE_HOST_APPLE)
    message(STATUS "SKIP: pulp_add_sparkle is macOS-only")
    return()
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
set(_dist "${FIXTURE_DIR}/dist")
file(MAKE_DIRECTORY "${_dist}/Sparkle.framework/Versions/B")
set(_key "mosCtB7H9gxWzbWUYyHiHTapl4sWMgkd4t09iIUnO2g=")

function(_fixture name body)
    set(_src "${FIXTURE_DIR}/${name}-src")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/main.cpp" "int main() { return 0; }\n")
    file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.24)
project(SparkleFixture LANGUAGES CXX)
set(CMAKE_OSX_ARCHITECTURES arm64)
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpDocumentTypes.cmake\")
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpSparkle.cmake\")
add_executable(App MACOSX_BUNDLE main.cpp)
add_library(Plug MODULE main.cpp)
set_target_properties(Plug PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst3)
${body}
")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${FIXTURE_DIR}/${name}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_rc "${_rc}" PARENT_SCOPE)
    set(_log "${_out}${_err}" PARENT_SCOPE)
endfunction()

function(_require_keys name)
    file(GLOB _templates "${FIXTURE_DIR}/${name}/PulpSparkle/App-Info.plist.in")
    if(NOT _templates)
        message(FATAL_ERROR "${name}: no derived Info.plist template")
    endif()
    file(READ "${_templates}" _t)
    foreach(_needle
            "<key>SUFeedURL</key>"
            "<string>https://example.com/releases/latest/download/appcast.xml</string>"
            "<key>SUPublicEDKey</key>" "<string>${_key}</string>"
            "<key>SUEnableAutomaticChecks</key>\n\t<false/>"
            "\${MACOSX_BUNDLE_BUNDLE_VERSION}")
        string(FIND "${_t}" "${_needle}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "${name}: template lacks ${_needle}:\n${_t}")
        endif()
    endforeach()
    # The keys sit inside the top-level dict, before it closes.
    string(FIND "${_t}" "<key>SUFeedURL</key>" _feed_at)
    string(FIND "${_t}" "</dict>" _close_at REVERSE)
    if(NOT _feed_at LESS _close_at)
        message(FATAL_ERROR "${name}: keys were written after </dict>")
    endif()
endfunction()

set(_call "pulp_add_sparkle(App FEED_URL \"https://example.com/releases/latest/download/appcast.xml\"
    PUBLIC_ED_KEY \"${_key}\" AUTOMATIC_CHECKS OFF DIST_DIR \"${_dist}\")")
set(_doc "pulp_declare_standalone_document_type(App UTI com.example.fixture.doc EXTENSION fxd
    DESCRIPTION \"Fixture\")")

_fixture(after_doc_types "${_doc}\n${_call}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "after_doc_types: configure failed:\n${_log}")
endif()
_require_keys(after_doc_types)

_fixture(before_doc_types "${_call}\n${_doc}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "before_doc_types: configure failed:\n${_log}")
endif()
_require_keys(before_doc_types)
file(READ "${FIXTURE_DIR}/before_doc_types/PulpSparkle/App-Info.plist.in" _t)
string(FIND "${_t}" "CFBundleDocumentTypes" _docs_at)
if(_docs_at EQUAL -1)
    message(FATAL_ERROR "before_doc_types: the document types were lost")
endif()

# What CMake actually generated into the bundle parses and carries the keys.
foreach(_name after_doc_types before_doc_types)
    set(_plist "${FIXTURE_DIR}/${_name}/App.app/Contents/Info.plist")
    execute_process(COMMAND plutil -extract SUPublicEDKey raw "${_plist}"
        RESULT_VARIABLE _prc OUTPUT_VARIABLE _got OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _prc EQUAL 0 OR NOT _got STREQUAL _key)
        message(FATAL_ERROR "${_name}: generated Info.plist lacks SUPublicEDKey (${_got})")
    endif()
endforeach()

_fixture(plugin_refused
    "pulp_add_sparkle(Plug FEED_URL \"https://e.com/a.xml\" PUBLIC_ED_KEY \"${_key}\" DIST_DIR \"${_dist}\")")
if(_rc EQUAL 0 OR NOT _log MATCHES "never in an AU/VST3/CLAP plug-in bundle")
    message(FATAL_ERROR "plugin_refused: a plug-in bundle was accepted:\n${_log}")
endif()

_fixture(private_key_refused
    "pulp_add_sparkle(App FEED_URL \"https://e.com/a.xml\" PUBLIC_ED_KEY \"not-a-public-key\" DIST_DIR \"${_dist}\")")
if(_rc EQUAL 0 OR NOT _log MATCHES "PUBLIC_ED_KEY must be")
    message(FATAL_ERROR "private_key_refused: a malformed key was accepted:\n${_log}")
endif()

_fixture(http_refused
    "pulp_add_sparkle(App FEED_URL \"http://e.com/a.xml\" PUBLIC_ED_KEY \"${_key}\" DIST_DIR \"${_dist}\")")
if(_rc EQUAL 0 OR NOT _log MATCHES "FEED_URL must be https")
    message(FATAL_ERROR "http_refused: a plain-http feed was accepted:\n${_log}")
endif()

message(STATUS "pulp_add_sparkle configure contract: OK")
