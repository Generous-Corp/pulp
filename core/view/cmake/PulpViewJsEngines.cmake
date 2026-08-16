# JS engine backend selection.
# PULP_JS_ENGINE: auto | quickjs | jsc | v8
#   auto    → QuickJS everywhere (never V8 — V8 is opt-in)
#   quickjs → QuickJS only (always available, portable)
#   jsc     → JavaScriptCore (Apple platforms only)
#   v8      → V8 via the pinned sealed libv8 (FindV8.cmake →
#             external/v8-build/<platform>/, populated by
#             tools/scripts/fetch_v8_for_release.py). Not supported on iOS.
set(PULP_JS_ENGINE "auto" CACHE STRING "JS engine backend (auto|quickjs|jsc|v8)")
set_property(CACHE PULP_JS_ENGINE PROPERTY STRINGS auto quickjs jsc v8)
set(_pulp_js_engine_choices auto quickjs jsc v8)
if(NOT PULP_JS_ENGINE IN_LIST _pulp_js_engine_choices)
    message(FATAL_ERROR
        "PULP_JS_ENGINE must be one of auto, quickjs, jsc, or v8; got "
        "'${PULP_JS_ENGINE}'.")
endif()
if(PULP_JS_ENGINE STREQUAL "jsc" AND NOT APPLE)
    message(FATAL_ERROR
        "PULP_JS_ENGINE=jsc is only supported on Apple platforms. Use auto or "
        "quickjs for the portable QuickJS backend.")
endif()
unset(_pulp_js_engine_choices)
# Advanced local-experiment overrides only — normal v8 builds need none (FindV8
# resolves external/v8-build or a baked $V8_DIR). When v8 is selected, the
# provider is the pinned sealed libv8, nothing else.
set(V8_LIBRARY_PATH "" CACHE FILEPATH "Advanced: full path to a V8 runtime library (local experiments)")

# V8 provider-identity reporting (additive).
# The pinned V8 provider is resolved by FindV8.cmake (external/v8-build/<platform>/,
# fetched by tools/scripts/fetch_v8_for_release.py). The V8 backend block below
# fills in the provider-identity vars from the resolved sealed artifact and emits
# them as compile definitions. Defaults describe an unknown/unselected provider.
option(PULP_VALIDATE_V8_PROVIDER_STRICT
    "Run the strict (no skip-pass) V8 provider-identity + cube-coexistence CTest" OFF)
set(PULP_V8_PROVIDER_KIND "")
set(PULP_V8_PROVIDER_PATH "")
set(PULP_V8_EXPECTED_RUNTIME_VERSION "")

# QuickJS backend is always compiled (portable fallback).
set(PULP_JS_ENGINE_SOURCES
    src/js_quickjs_engine.cpp
    src/js_engine_factory.cpp
)

set(_pulp_ubsan_enabled OFF)
if(PULP_SANITIZER STREQUAL "undefined"
        OR CMAKE_CXX_FLAGS MATCHES "(^| )-fsanitize=[^ ]*undefined"
        OR CMAKE_C_FLAGS MATCHES "(^| )-fsanitize=[^ ]*undefined")
    set(_pulp_ubsan_enabled ON)
endif()

if(_pulp_ubsan_enabled AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    # CHOC's header-only QuickJS backend is tracked under #1987 for UBSan-only
    # exception corruption. Keep UBSan on Pulp's script/runtime code while
    # compiling only that vendored backend without undefined instrumentation.
    set_source_files_properties(src/js_quickjs_engine.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-sanitize=undefined")
endif()
unset(_pulp_ubsan_enabled)

# V8 backend — provider is the pinned sealed libv8 (FindV8.cmake →
# external/v8-build/<platform>/, fetched by tools/scripts/fetch_v8_for_release.py).
# js_v8_engine.cpp is ALWAYS compiled; it is #ifdef PULP_HAS_V8 internally and
# becomes a stub when V8 is not linked. V8 is linked ONLY when explicitly selected
# with PULP_JS_ENGINE=v8 — `auto` never silently pulls in V8.
list(APPEND PULP_JS_ENGINE_SOURCES src/js_v8_engine.cpp)
if(PULP_JS_ENGINE STREQUAL "v8")
    # iOS cannot JIT — V8 is forbidden in iOS apps / AUv3 extensions. Hard error,
    # not a silent fallback, so a misconfigured iOS build fails loudly.
    if(IOS OR PULP_IOS)
        message(FATAL_ERROR
            "PULP_JS_ENGINE=v8 is not supported on iOS: V8 requires JIT, which iOS "
            "apps and AUv3 extensions forbid. Use QuickJS (the default) or JSC.")
    endif()
    include(${PULP_ROOT_DIR}/tools/cmake/FindV8.cmake)
    if(NOT PULP_V8_FOUND)
        message(FATAL_ERROR
            "PULP_JS_ENGINE=v8 selected but no sealed V8 was found. Populate it with:\n"
            "    python3 tools/scripts/fetch_v8_for_release.py <platform>\n"
            "(unpacks to external/v8-build/), or point V8_DIR at a baked V8, or set "
            "the advanced V8_INCLUDE_DIR + V8_LIB_DIR overrides for a local build.")
    endif()
    target_link_libraries(pulp-view-script PRIVATE v8::v8)
    set(PULP_HAS_V8_ACTUAL ON)

    # Provider identity — the pinned provider IS the sealed v8-builder libv8
    # (FindV8 resolves external/v8-build/<platform>/). Surface its kind, path,
    # and the manifest-pinned runtime version so the runtime can report and
    # cross-check the provider. The js_v8_engine.cpp accessors that read these
    # defines are additive and degrade gracefully when a define is empty.
    set(PULP_V8_PROVIDER_KIND "v8builder")
    set(PULP_V8_PROVIDER_PATH "${V8_RUNTIME_LIBRARY}")
    set(PULP_V8_EXPECTED_RUNTIME_VERSION "")
    if(EXISTS "${PULP_ROOT_DIR}/tools/deps/manifest.json")
        file(READ "${PULP_ROOT_DIR}/tools/deps/manifest.json" _pulp_v8_manifest_text)
        # Derive the expected V8 runtime version structurally from the manifest
        # (the dependency source of truth) so the strict provider-identity gate
        # can cross-check the sealed libv8 against its pinned version. Parse with
        # string(JSON), NOT a text regex over the raw file: scope to the entry
        # whose "name" is exactly "V8" — matching "the first v8- version anywhere"
        # would silently read the wrong entry if another dependency ever carried a
        # v8- prefix or was ordered earlier — and read its "version" field
        # regardless of sibling-field order within the entry (a name→version text
        # bridge breaks when a braces-delimited field like "upstream": { ... } is
        # reordered ahead of "version"). The tag may carry an LKGR suffix (e.g.
        # "v8-m152-15.2.124.7-<sha>", "v8-15.2.24-lkgr-<sha>", or the older
        # bare "v8-15.1.27"); discard an optional milestone prefix and reduce
        # it to the dotted-numeric run so the gate compares the runtime version
        # rather than the full release tag.
        string(JSON _pulp_v8_dep_count ERROR_VARIABLE _pulp_v8_json_err
               LENGTH "${_pulp_v8_manifest_text}" dependencies)
        if(NOT _pulp_v8_json_err AND _pulp_v8_dep_count GREATER 0)
            math(EXPR _pulp_v8_last "${_pulp_v8_dep_count} - 1")
            foreach(_pulp_v8_i RANGE ${_pulp_v8_last})
                string(JSON _pulp_v8_dep ERROR_VARIABLE _pulp_v8_derr
                       GET "${_pulp_v8_manifest_text}" dependencies ${_pulp_v8_i})
                if(_pulp_v8_derr)
                    continue()
                endif()
                string(JSON _pulp_v8_name ERROR_VARIABLE _pulp_v8_nerr
                       GET "${_pulp_v8_dep}" name)
                if(NOT _pulp_v8_nerr AND _pulp_v8_name STREQUAL "V8")
                    string(JSON _pulp_v8_ver ERROR_VARIABLE _pulp_v8_verr
                           GET "${_pulp_v8_dep}" version)
                    if(NOT _pulp_v8_verr AND
                       _pulp_v8_ver MATCHES "^v8-(m[0-9]+-)?([0-9]+(\\.[0-9]+)*)")
                        set(PULP_V8_EXPECTED_RUNTIME_VERSION "${CMAKE_MATCH_2}")
                    endif()
                    break()
                endif()
            endforeach()
        endif()
        unset(_pulp_v8_manifest_text)
    endif()

    # Publish the identity to the global cache so sibling subdirectories added
    # after core/view (examples/threejs-native-demo's strict provider-identity
    # CTest) can read the resolved kind + pinned version without re-deriving it.
    set(PULP_V8_PROVIDER_KIND "${PULP_V8_PROVIDER_KIND}" CACHE INTERNAL "Resolved V8 provider kind")
    set(PULP_V8_PROVIDER_PATH "${PULP_V8_PROVIDER_PATH}" CACHE INTERNAL "Resolved V8 provider library path")
    set(PULP_V8_EXPECTED_RUNTIME_VERSION "${PULP_V8_EXPECTED_RUNTIME_VERSION}" CACHE INTERNAL "Manifest-pinned V8 runtime version")

    # Windows: the sealed v8.dll exposes the Chromium libc++ (__Cr) ABI with
    # pointer compression ON, so the V8 translation unit must be compiled
    # with clang-cl + that libc++ + V8_COMPRESS_POINTERS. This is a no-op off
    # Windows; on Windows it hard-fails if the compiler is MSVC cl rather
    # than clang-cl. See tools/cmake/PulpV8Windows.cmake.
    include(${PULP_ROOT_DIR}/tools/cmake/PulpV8Windows.cmake)
    pulp_v8_windows_apply_abi(pulp-view-script)
endif()

# JSC is opt-in even on Apple. `auto` and `quickjs` are intentionally
# QuickJS-only builds: compiling every platform backend made the documented
# selection knob lie and forced JavaScriptCore.framework into otherwise
# QuickJS-only plugin binaries.
set(PULP_HAS_JSC_ACTUAL OFF)
if(APPLE AND PULP_JS_ENGINE STREQUAL "jsc")
    list(APPEND PULP_JS_ENGINE_SOURCES src/js_jsc_engine.mm)
    target_compile_definitions(pulp-view-script PUBLIC PULP_HAS_JSC=1)
    set(PULP_HAS_JSC_ACTUAL ON)
endif()
# Sibling directories (notably the binary link-floor fixture under test/) need
# the resolved availability after core/view returns to the parent scope. Cache
# it internally and force-refresh it so reconfiguring jsc -> quickjs cannot
# retain a stale ON value.
set(PULP_HAS_JSC_ACTUAL "${PULP_HAS_JSC_ACTUAL}" CACHE INTERNAL
    "Whether this build compiles and links the JavaScriptCore backend" FORCE)

# Set compile definitions for engine availability / defaults.
if(PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view-script PRIVATE PULP_HAS_V8=1)

    # Provider identity, consumed by V8Engine::provider_kind()/provider_path()/
    # expected_runtime_version(). These describe the sealed v8-builder artifact
    # independently of the runtime version V8 reports at startup.
    target_compile_definitions(pulp-view-script PRIVATE
        PULP_V8_PROVIDER_KIND="${PULP_V8_PROVIDER_KIND}"
        PULP_V8_PROVIDER_PATH="${PULP_V8_PROVIDER_PATH}"
        PULP_V8_EXPECTED_RUNTIME_VERSION="${PULP_V8_EXPECTED_RUNTIME_VERSION}")
endif()

# Only set non-QuickJS default when explicitly requested (not auto).
if(PULP_JS_ENGINE STREQUAL "jsc")
    if(PULP_HAS_JSC_ACTUAL)
        target_compile_definitions(pulp-view-script PRIVATE PULP_DEFAULT_ENGINE_JSC=1)
    endif()
elseif(PULP_JS_ENGINE STREQUAL "v8" AND PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view-script PRIVATE PULP_DEFAULT_ENGINE_V8=1)
endif()
# auto → QuickJS default (backward compatible with all existing code)

# Configure-time source/link oracle, called from core/view/CMakeLists.txt AFTER the
# JSC source and framework linkage are attached to pulp-view-script. Calling it
# earlier reads empty target properties and silently passes.
function(pulp_view_assert_js_engine_link_contract)
    # Configure-time source/link oracle. Static archives can dead-strip an unused
    # backend, so final-binary symbol scans alone cannot prove that a QuickJS SDK did
    # not compile or export JSC linkage. Keep the target graph itself fail-closed.
    get_target_property(_pulp_view_script_sources pulp-view-script SOURCES)
    get_target_property(_pulp_view_script_links pulp-view-script LINK_LIBRARIES)
    if(PULP_HAS_JSC_ACTUAL)
        if(NOT "${_pulp_view_script_sources}" MATCHES "js_jsc_engine\\.mm" OR
           NOT "${_pulp_view_script_links}" MATCHES "JavaScriptCore")
            message(FATAL_ERROR
                "PULP_JS_ENGINE=jsc must compile js_jsc_engine.mm and link "
                "JavaScriptCore.framework.")
        endif()
    else()
        if("${_pulp_view_script_sources}" MATCHES "js_jsc_engine\\.mm" OR
           "${_pulp_view_script_links}" MATCHES "JavaScriptCore")
            message(FATAL_ERROR
                "PULP_JS_ENGINE=${PULP_JS_ENGINE} leaked the JSC source or framework: "
                "sources=${_pulp_view_script_sources}; links=${_pulp_view_script_links}")
        endif()
    endif()
    unset(_pulp_view_script_sources)
    unset(_pulp_view_script_links)
endfunction()
