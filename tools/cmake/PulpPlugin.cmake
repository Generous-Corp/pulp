# PulpPlugin.cmake — private plugin target orchestration.
#
# PulpUtils.cmake remains the stable compatibility entry point. This module
# owns pulp_add_plugin() and pulp_add_plugin_bundle() so consumers that include
# the historical PulpPlugin.cmake path still receive the same public commands.
# Direct inclusion bootstraps through PulpUtils; the shim includes this file
# after target aliases, source probes, and focused helper modules are ready.

if(NOT COMMAND _pulp_pick_target)
    set(_pulp_plugin_direct_include TRUE)
    include("${CMAKE_CURRENT_LIST_DIR}/PulpUtils.cmake")
    if(COMMAND pulp_add_plugin)
        message(DEPRECATION
            "PulpPlugin.cmake is deprecated; include PulpUtils.cmake via find_package(Pulp) instead.")
        unset(_pulp_plugin_direct_include)
        return()
    endif()
endif()

if(COMMAND pulp_add_plugin)
    return()
endif()

# ── pulp_add_plugin ─────────────────────────────────────────────────────────
# Creates plugin targets for each requested format from a single declaration.
#
# Usage:
#   pulp_add_plugin(PulpGain
#       FORMATS         VST3 AU CLAP Standalone
#       PLUGIN_NAME     "PulpGain"
#       BUNDLE_ID       "com.pulp.gain"
#       MANUFACTURER    "Pulp"
#       VERSION         "1.0.0"
#       CATEGORY        Effect           # Effect | Instrument | MidiEffect
#       PLUGIN_CODE     "PGan"           # 4-char code for AU
#       MANUFACTURER_CODE "Pulp"         # 4-char code for AU
#       ACCEPTS_MIDI                     # set if descriptor.accepts_midi is true;
#                                        # flips AU component type from aufx to aumf
#                                        # so hosts route inbound MIDI to the plug-in
#       SOURCES         pulp_gain.hpp main.cpp
#       PROCESSOR_FACTORY create_pulp_gain  # Function that returns unique_ptr<Processor>
#       SOURCE_GIT_SHA  "${MY_SHA}"      # optional; commit recorded in each bundle's
#       SOURCE_GIT_DIRTY FALSE           # pulp-build-info.json (default: read from git
#                                        # at build time; see PulpBuildInfo.cmake)
#   )
#
# This creates:
#   ${target}_Core       — Object library with shared processor code
#   ${target}_VST3       — VST3 bundle (.vst3)
#   ${target}_AU         — AU v2 component (.component)
#   ${target}_CLAP       — CLAP bundle (.clap)
#   ${target}_Standalone — Standalone executable
#
function(pulp_add_plugin target)
    cmake_parse_arguments(PLUGIN
        "ACCEPTS_MIDI;NATIVE_UI;SHIP_INSPECTOR;SHIP_INSPECTOR_RUNTIME_EVAL;ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL"
        "PLUGIN_NAME;BUNDLE_ID;VERSION;MANUFACTURER;CATEGORY;PLUGIN_CODE;MANUFACTURER_CODE;AAX_PRODUCT_CODE;AAX_NATIVE_CODE;PROCESSOR_FACTORY;UI_SCRIPT;ICON;ICNS;DESIGN_WIDTH;DESIGN_HEIGHT;DESIGN_MIN_WIDTH;DESIGN_MIN_HEIGHT;DESIGN_MAX_WIDTH;DESIGN_MAX_HEIGHT;CONTROL_PROFILE;SOURCE_GIT_SHA;SOURCE_GIT_DIRTY"
        "FORMATS;SOURCES;CONTENT_CAPABILITIES;CONTENT_KINDS;CONTENT_HOT_RELOAD_KINDS;CONTENT_MANUAL_RESCAN_KINDS;INSPECTOR_CAPABILITIES;CONTROL_CAPABILITIES"
        ${ARGN}
    )
    if("PRODUCES_MIDI" IN_LIST PLUGIN_UNPARSED_ARGUMENTS)
        message(WARNING
            "pulp_add_plugin(${target}): PRODUCES_MIDI is ignored. "
            "Set PluginDescriptor::produces_midi = true in the processor; "
            "MIDI output is format/runtime metadata, not a CMake packaging flag.")
    endif()

    # Defaults
    if(NOT PLUGIN_PLUGIN_NAME)
        set(PLUGIN_PLUGIN_NAME "${target}")
    endif()
    if(NOT PLUGIN_VERSION)
        set(PLUGIN_VERSION "1.0.0")
    endif()
    if(NOT PLUGIN_MANUFACTURER)
        set(PLUGIN_MANUFACTURER "Unknown")
    endif()
    if(NOT PLUGIN_CATEGORY)
        set(PLUGIN_CATEGORY "Effect")
    endif()

    # ACCEPTS_MIDI mirrors ``PluginDescriptor::accepts_midi`` to CMake so
    # we can pick the correct AU component type (``aumf`` vs ``aufx``).
    # Hosts only route MIDI to AU v2 effects packaged as
    # ``kAudioUnitType_MusicEffect`` (``aumf``); ``aufx``-typed plug-ins
    # never see ``HandleMIDIEvent`` regardless of what the adapter wires.
    # Plug-ins that set ``accepts_midi = true`` in their descriptor MUST
    # also pass ACCEPTS_MIDI to ``pulp_add_plugin`` so the emitted
    # ``.component`` bundle's ``type`` matches.
    if(PLUGIN_ACCEPTS_MIDI)
        set(_plugin_accepts_midi "1")
    else()
        set(_plugin_accepts_midi "0")
    endif()

    _pulp_normalize_ui_script_path(_PULP_UI_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}" "${PLUGIN_UI_SCRIPT}")
    set(PULP_${target}_UI_SCRIPT "${_PULP_UI_SCRIPT}" CACHE INTERNAL "")
    if(PLUGIN_NATIVE_UI)
        if(NOT _PULP_NATIVE_VIEW_TARGET OR NOT _PULP_NATIVE_STANDALONE_TARGET)
            message(FATAL_ERROR
                "pulp_add_plugin(${target} NATIVE_UI): this Pulp SDK does not export the native-only view/standalone targets")
        endif()
        set(PULP_${target}_VIEW_TARGET "${_PULP_NATIVE_VIEW_TARGET}" CACHE INTERNAL "")
        set(PULP_${target}_STANDALONE_TARGET "${_PULP_NATIVE_STANDALONE_TARGET}" CACHE INTERNAL "")
    else()
        set(PULP_${target}_VIEW_TARGET "${_PULP_VIEW_TARGET}" CACHE INTERNAL "")
        set(PULP_${target}_STANDALONE_TARGET "${_PULP_STANDALONE_TARGET}" CACHE INTERNAL "")
    endif()
    set(PULP_${target}_CONTENT_CAPABILITIES "${PLUGIN_CONTENT_CAPABILITIES}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_KINDS "${PLUGIN_CONTENT_KINDS}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_HOT_RELOAD_KINDS "${PLUGIN_CONTENT_HOT_RELOAD_KINDS}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_MANUAL_RESCAN_KINDS "${PLUGIN_CONTENT_MANUAL_RESCAN_KINDS}" CACHE INTERNAL "")
    if(PLUGIN_SHIP_INSPECTOR OR PLUGIN_SHIP_INSPECTOR_RUNTIME_EVAL OR
       PLUGIN_INSPECTOR_CAPABILITIES)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): SHIP_INSPECTOR and INSPECTOR_CAPABILITIES were removed; use CONTROL_PROFILE and CONTROL_CAPABILITIES")
    endif()

    if(PLUGIN_CONTROL_CAPABILITIES AND NOT PLUGIN_CONTROL_PROFILE)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTROL_CAPABILITIES requires an explicit CONTROL_PROFILE")
    endif()

    if(PLUGIN_CONTROL_CAPABILITIES)
        list(LENGTH PLUGIN_FORMATS _pulp_control_format_count)
        if(NOT _pulp_control_format_count EQUAL 1 OR
           NOT "Standalone" IN_LIST PLUGIN_FORMATS)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): CONTROL_CAPABILITIES currently require an exclusively Standalone artifact; mixed-format siblings remain production-stripped")
        endif()
        if(NOT _PULP_CONTROL_STANDALONE_TARGET)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): CONTROL_CAPABILITIES require the installed canonical Standalone host adapter")
        endif()
        if(APPLE AND PULP_ENABLE_GPU AND NOT IOS AND NOT PULP_IOS)
            set(_pulp_standalone_control_capabilities
                dev.pulp.instance/read@1
                dev.pulp.session/control@1
                dev.pulp.state/read@1
                dev.pulp.ui/observe@1
                dev.pulp.diagnostics/read@1
                dev.pulp.logs/read@1
                dev.pulp.ui/capture@1
                dev.pulp.ui/input@1
                dev.pulp.trace/control@1
                dev.pulp.trace/session-control@1
                dev.pulp.state/parameter-gesture@1
                dev.pulp.test/input@1
                dev.pulp.authoring/tweaks@1
                dev.pulp.telemetry/subscribe@1
                dev.pulp.runtime/evaluate@1
                dev.pulp.sequencer/transport.loop.read@1
                dev.pulp.sequencer/transport.loop.write@1)
        else()
            set(_pulp_standalone_control_capabilities
                dev.pulp.instance/read@1
                dev.pulp.state/read@1)
        endif()
        foreach(_pulp_control_capability IN LISTS PLUGIN_CONTROL_CAPABILITIES)
            if(NOT _pulp_control_capability IN_LIST _pulp_standalone_control_capabilities)
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): '${_pulp_control_capability}' is not yet implemented by the canonical Standalone adapter")
            endif()
        endforeach()
        if(("dev.pulp.ui/capture@1" IN_LIST PLUGIN_CONTROL_CAPABILITIES OR
            "dev.pulp.ui/input@1" IN_LIST PLUGIN_CONTROL_CAPABILITIES) AND
           NOT _PULP_CONTROL_UI_TARGET)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): UI control capabilities require the installed exact-target UI adapter")
        endif()
        if(("dev.pulp.trace/control@1" IN_LIST PLUGIN_CONTROL_CAPABILITIES OR
            "dev.pulp.trace/session-control@1" IN_LIST PLUGIN_CONTROL_CAPABILITIES OR
            "dev.pulp.telemetry/subscribe@1" IN_LIST PLUGIN_CONTROL_CAPABILITIES) AND
           NOT _PULP_CONTROL_INSPECT_TARGET)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): trace and telemetry capabilities require the installed host observability runtime")
        endif()
        if("dev.pulp.runtime/evaluate@1" IN_LIST PLUGIN_CONTROL_CAPABILITIES AND
           NOT _PULP_CONTROL_RUNTIME_EVAL_TARGET)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): runtime evaluation requires the separately installed high-risk evaluator")
        endif()
    endif()

    if(PLUGIN_CONTROL_PROFILE)
        set(_control_profiles
            production-stripped developer-local test-deterministic
            support-diagnostics research-unsafe)
        if(NOT PLUGIN_CONTROL_PROFILE IN_LIST _control_profiles)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): unknown CONTROL_PROFILE '${PLUGIN_CONTROL_PROFILE}'")
        endif()
        set(_pulp_control_profile "${PLUGIN_CONTROL_PROFILE}")
        set(_pulp_control_capabilities "${PLUGIN_CONTROL_CAPABILITIES}")
        set(_pulp_control_eval_ack "${PLUGIN_ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL}")
    else()
        set(_pulp_control_profile "production-stripped")
        set(_pulp_control_capabilities "")
        set(_pulp_control_eval_ack "")
    endif()
    _pulp_cache_control_declarations(${target}
        "${_pulp_control_profile}"
        "${_pulp_control_capabilities}"
        "${_pulp_control_eval_ack}")
    _pulp_configure_control_shipping(
        ${target} "${PLUGIN_BUNDLE_ID}" "${PLUGIN_PLUGIN_NAME}")

    if(_PULP_UI_SCRIPT AND NOT EXISTS "${_PULP_UI_SCRIPT}")
        message(WARNING
            "pulp_add_plugin(${target}): UI_SCRIPT points to a missing file: ${_PULP_UI_SCRIPT}. "
            "The editor will fall back to AutoUi until the script exists.")
    endif()
    _pulp_configure_plugin_runtime_manifest(${target} "${PLUGIN_BUNDLE_ID}")
    # Product source identity recorded in each bundle's pulp-build-info.json.
    # Always refreshed so a removed argument does not leave a stale override.
    set(PULP_${target}_SOURCE_GIT_SHA "${PLUGIN_SOURCE_GIT_SHA}" CACHE INTERNAL "")
    set(PULP_${target}_SOURCE_GIT_DIRTY "${PLUGIN_SOURCE_GIT_DIRTY}" CACHE INTERNAL "")

    # ── Core library ────────────────────────────────────────────────────
    # For header-only processors (no SOURCES), create INTERFACE library.
    # For compiled processors, create OBJECT library.
    if(PLUGIN_SOURCES)
        add_library(${target}_Core OBJECT ${PLUGIN_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "${PULP_${target}_CORE_OBJECTS}")
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "")
    endif()
    # Store for format target functions (CMake doesn't propagate local vars to functions)
    set(_PULP_CORE_OBJECTS "${_PULP_CORE_OBJECTS}" PARENT_SCOPE)
    set(PULP_${target}_CORE_OBJECTS "${_PULP_CORE_OBJECTS}" CACHE INTERNAL "")
    if(PLUGIN_SOURCES)
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${PLUGIN_PLUGIN_NAME}"
            PULP_BUNDLE_ID="${PLUGIN_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${PLUGIN_VERSION}"
        )
    endif()

    # ── Design dimensions (auto-sizing for imported-design plugins) ────
    # When DESIGN_WIDTH/HEIGHT are supplied, inject them as compile-defs so
    # `format::Processor::view_size()`'s default returns sensible bounds
    # (derived min = 2/3 preferred, max = 2x preferred, aspect = W/H).
    # Plugins still pull this in via target_compile_definitions on Core,
    # so all linked format adapters see the same defs. Explicit MIN/MAX
    # args override the derived values. See processor.hpp:view_size() and
    # the import-design skill.
    if(PLUGIN_DESIGN_WIDTH AND PLUGIN_DESIGN_HEIGHT)
        if(NOT PLUGIN_DESIGN_MIN_WIDTH)
            set(PLUGIN_DESIGN_MIN_WIDTH 0)
        endif()
        if(NOT PLUGIN_DESIGN_MIN_HEIGHT)
            set(PLUGIN_DESIGN_MIN_HEIGHT 0)
        endif()
        if(NOT PLUGIN_DESIGN_MAX_WIDTH)
            set(PLUGIN_DESIGN_MAX_WIDTH 0)
        endif()
        if(NOT PLUGIN_DESIGN_MAX_HEIGHT)
            set(PLUGIN_DESIGN_MAX_HEIGHT 0)
        endif()
        set(_design_defs
            PULP_PLUGIN_DESIGN_W=${PLUGIN_DESIGN_WIDTH}
            PULP_PLUGIN_DESIGN_H=${PLUGIN_DESIGN_HEIGHT}
            PULP_PLUGIN_DESIGN_MIN_W=${PLUGIN_DESIGN_MIN_WIDTH}
            PULP_PLUGIN_DESIGN_MIN_H=${PLUGIN_DESIGN_MIN_HEIGHT}
            PULP_PLUGIN_DESIGN_MAX_W=${PLUGIN_DESIGN_MAX_WIDTH}
            PULP_PLUGIN_DESIGN_MAX_H=${PLUGIN_DESIGN_MAX_HEIGHT}
        )
        if(PLUGIN_SOURCES)
            target_compile_definitions(${target}_Core PUBLIC ${_design_defs})
        else()
            target_compile_definitions(${target}_Core INTERFACE ${_design_defs})
        endif()
    endif()

    # ── VST3 ─────────────────────────────────────────────────────────────
    if("VST3" IN_LIST PLUGIN_FORMATS AND PULP_HAS_VST3)
        _pulp_add_vst3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                        "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── CLAP ─────────────────────────────────────────────────────────────
    if("CLAP" IN_LIST PLUGIN_FORMATS AND PULP_HAS_CLAP)
        _pulp_add_clap(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                        "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AU v2 ────────────────────────────────────────────────────────────
    if("AU" IN_LIST PLUGIN_FORMATS AND APPLE AND PULP_HAS_AUSDK)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AU format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_au(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                          "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                          "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}"
                          "${_plugin_accepts_midi}")
        endif()
    endif()

    # ── LV2 ───────────────────────────────────────────────────────────────
    if("LV2" IN_LIST PLUGIN_FORMATS AND PULP_HAS_LV2)
        _pulp_add_lv2(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                       "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AAX ───────────────────────────────────────────────────────────────
    if("AAX" IN_LIST PLUGIN_FORMATS)
        if(NOT APPLE AND NOT WIN32)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): AAX is only supported on macOS and Windows. "
                "Remove AAX from FORMATS when configuring on Linux or Ubuntu.")
        elseif(PULP_HAS_AAX)
            if(NOT PLUGIN_MANUFACTURER_CODE OR NOT PLUGIN_AAX_PRODUCT_CODE OR NOT PLUGIN_AAX_NATIVE_CODE)
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): AAX format requires "
                    "MANUFACTURER_CODE, AAX_PRODUCT_CODE, and AAX_NATIVE_CODE")
            endif()

            _pulp_add_aax(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                          "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                          "${PLUGIN_CATEGORY}" "${PLUGIN_MANUFACTURER_CODE}"
                          "${PLUGIN_AAX_PRODUCT_CODE}" "${PLUGIN_AAX_NATIVE_CODE}")
        endif()
    endif()

    # ── AUv3 ──────────────────────────────────────────────────────────────
    if("AUv3" IN_LIST PLUGIN_FORMATS AND APPLE)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AUv3 format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_auv3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                           "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                           "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}"
                           "${_plugin_accepts_midi}")
        endif()
    endif()

    # ── Standalone ───────────────────────────────────────────────────────
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        _pulp_add_standalone(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}" "${PLUGIN_VERSION}" "${PLUGIN_PROCESSOR_FACTORY}")
    endif()

    # ── Product icon ───────────────────────────────────────────────────
    # ICNS bundles a finished .icns as-is; ICON derives one from a 1024x1024
    # PNG. Applied to every bundle this call created, not just the
    # standalone app, because the plug-in Info.plist templates all carry a
    # CFBundleIconFile key and nothing could fill it: the key shipped
    # substituted from the MACOSX_BUNDLE_ICON_FILE target property, which
    # only pulp_app_icon() set, and that helper is documented for apps and
    # dispatches per-platform (a Windows .rc on a MODULE library). A
    # consumer wanting a branded plug-in therefore had to reach past this
    # API and set the property on `${target}_VST3` and friends by hand --
    # internal target names that are not a public contract. That is the gap
    # this closes.
    #
    # Scope, so nobody reads more into it than it does: on a non-.app bundle
    # macOS does not render CFBundleIconFile at all -- Finder draws the
    # generic bundle icon whatever this writes. The key is filled because
    # the templates declare it and an empty declared key is worse than a
    # filled one, not because an icon will appear. The mechanism that does
    # render is a custom Finder icon (an `Icon\r` resource fork plus a
    # com.apple.FinderInfo xattr), which `codesign --strict` rejects as
    # "resource fork, Finder information, or similar detritus not allowed"
    # and which therefore cannot be used by a signed, notarized product.
    # For artwork inside a host, the VST3 Snapshots convention is the path.
    if(APPLE AND (PLUGIN_ICON OR PLUGIN_ICNS))
        if(PLUGIN_ICON AND PLUGIN_ICNS)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): pass ICON or ICNS, not both.")
        endif()
        set(_pulp_plugin_icon_png "")
        set(_pulp_plugin_icon_icns "")
        if(PLUGIN_ICNS)
            _pulp_icon_abs_path(_pulp_plugin_icon_icns
                "${CMAKE_CURRENT_SOURCE_DIR}" "${PLUGIN_ICNS}")
            if(NOT EXISTS "${_pulp_plugin_icon_icns}")
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): ICNS not found: "
                    "${_pulp_plugin_icon_icns}")
            endif()
        else()
            _pulp_icon_abs_path(_pulp_plugin_icon_png
                "${CMAKE_CURRENT_SOURCE_DIR}" "${PLUGIN_ICON}")
            if(NOT EXISTS "${_pulp_plugin_icon_png}")
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): ICON not found: "
                    "${_pulp_plugin_icon_png}")
            endif()
        endif()
        foreach(_pulp_icon_bundle
                ${target}_Standalone ${target}_VST3 ${target}_AU
                ${target}_CLAP ${target}_AAX)
            if(TARGET ${_pulp_icon_bundle})
                _pulp_icon_configure_macos(${_pulp_icon_bundle}
                    "${_pulp_plugin_icon_png}" "${_pulp_plugin_icon_icns}")
            endif()
        endforeach()
    endif()

    # ── Build identity ─────────────────────────────────────────────────
    # pulp-build-info.json in every bundle this call produced, written
    # POST_BUILD so it is sealed by whatever signs the bundle afterwards.
    # The AUv3 framework is skipped by the helper: it is embedded in the
    # extension and host app, which carry their own records.
    foreach(_pulp_build_info_format
            VST3 CLAP AU LV2 AAX AUv3 AUv3Host Standalone)
        _pulp_attach_build_info(${target}
            ${target}_${_pulp_build_info_format} ${_pulp_build_info_format}
            "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}" "${PLUGIN_VERSION}"
            "${PLUGIN_MANUFACTURER}")
    endforeach()

    _pulp_add_plugin_install_target(${target} "${PLUGIN_PLUGIN_NAME}")

    set(_built_formats)
    foreach(_fmt VST3 CLAP AU AUv3 LV2 AAX Standalone)
        if(TARGET ${target}_${_fmt})
            list(APPEND _built_formats ${_fmt})
        endif()
    endforeach()
    if(_built_formats)
        list(JOIN _built_formats ";" _built_formats_display)
    else()
        set(_built_formats_display "none")
    endif()
    message(STATUS "Pulp plugin: ${target} (formats: ${_built_formats_display})")
endfunction()

# ── pulp_add_plugin_bundle — ONE binary per format hosting MANY plugins ────
#
# The cross-format counterpart to the AU/VST3/CLAP bundle macros: a single
# .vst3 / .clap that exposes N distinct plugins (Expert Sleepers Silent Way
# style). Each format's entry TU (`<fmt>_bundle_entry.cpp` in the source dir)
# uses the bundle macros to register N plugins into one binary; per-plugin
# metadata (category, UI script, and — for AU — component codes) lives in the
# macro calls, not in CMake args. The single-plugin path (`pulp_add_plugin`)
# is unchanged and remains the default (see PULP_PLUGIN_PACKAGING).
#
# v1 supports the internally-enumerating formats CLAP and VST3 (each emits one
# entry symbol whose factory lists N plugins). AU/AUv3/AAX bundles additionally
# need a multi-component Info.plist and multi-symbol export, so they land as a
# separate slice; requesting one here errors with that note rather than emitting
# a silently-single-plugin binary.
#
#   pulp_add_plugin_bundle(MyBundle
#       BUNDLE_NAME  "MyBundle"
#       BUNDLE_ID    "com.example.mybundle"
#       VERSION      "1.0.0"
#       MANUFACTURER "Example"
#       FORMATS      CLAP VST3
#       SOURCES      plugin_a.cpp plugin_b.cpp   # union of the bundled plugins
#   )
function(pulp_add_plugin_bundle target)
    cmake_parse_arguments(BUNDLE
        ""
        "BUNDLE_NAME;BUNDLE_ID;VERSION;MANUFACTURER"
        "FORMATS;SOURCES"
        ${ARGN}
    )
    if(NOT BUNDLE_BUNDLE_NAME)
        set(BUNDLE_BUNDLE_NAME "${target}")
    endif()
    if(NOT BUNDLE_VERSION)
        set(BUNDLE_VERSION "1.0.0")
    endif()
    if(NOT BUNDLE_MANUFACTURER)
        set(BUNDLE_MANUFACTURER "Unknown")
    endif()

    foreach(_fmt ${BUNDLE_FORMATS})
        if(_fmt STREQUAL "AU" OR _fmt STREQUAL "AUv3" OR _fmt STREQUAL "AAX")
            message(FATAL_ERROR
                "pulp_add_plugin_bundle(${target}): ${_fmt} bundles are not yet "
                "supported. An ${_fmt} bundle needs a multi-component Info.plist and "
                "multi-symbol export (a separate slice). Use CLAP and/or VST3 here.")
        endif()
    endforeach()

    # Multi-plugin bundles are ordinary production artifacts until a future
    # bundle-level capability contract exists. They still receive the same
    # per-format negative scan, so this path cannot bypass strip enforcement.
    _pulp_cache_control_declarations(${target} production-stripped "" FALSE)
    _pulp_configure_control_shipping(
        ${target} "${BUNDLE_BUNDLE_ID}" "${BUNDLE_BUNDLE_NAME}")
    _pulp_configure_plugin_runtime_manifest(${target} "${BUNDLE_BUNDLE_ID}")

    # All bundled plugins' code flows into each format binary. With SOURCES, an
    # OBJECT library carries them; header-only plugins (inline factories pulled in
    # by the per-format entry TU) use an INTERFACE library. Either way the objects
    # reach the binary through the link (target_link_libraries(<fmt> PRIVATE
    # ${target}_Core) in the format functions), so CORE_OBJECTS stays empty —
    # mirroring pulp_add_plugin.
    if(BUNDLE_SOURCES)
        add_library(${target}_Core OBJECT ${BUNDLE_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${BUNDLE_BUNDLE_NAME}"
            PULP_BUNDLE_ID="${BUNDLE_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${BUNDLE_VERSION}")
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    set(PULP_${target}_CORE_OBJECTS "" CACHE INTERNAL "")
    # Bundle formats use the same editor host as ordinary non-native plugins.
    # Keep this per-target selection in lockstep with pulp_add_plugin(): format
    # helpers no longer consult the process-global _PULP_VIEW_TARGET directly.
    set(PULP_${target}_VIEW_TARGET "${_PULP_VIEW_TARGET}" CACHE INTERNAL "")
    # No bundle-wide compile-time UI script: a bundle's plugins each resolve
    # their own editor assets from the keyed registry at runtime, rather than
    # from a single compile-time PULP_UI_SCRIPT_PATH shared by the whole binary.
    set(PULP_${target}_UI_SCRIPT "" CACHE INTERNAL "")

    if("VST3" IN_LIST BUNDLE_FORMATS AND PULP_HAS_VST3)
        _pulp_add_vst3(${target} "${BUNDLE_BUNDLE_NAME}" "${BUNDLE_BUNDLE_ID}"
                        "${BUNDLE_VERSION}" "${BUNDLE_MANUFACTURER}" "Effect")
    endif()
    if("CLAP" IN_LIST BUNDLE_FORMATS AND PULP_HAS_CLAP)
        _pulp_add_clap(${target} "${BUNDLE_BUNDLE_NAME}" "${BUNDLE_BUNDLE_ID}"
                        "${BUNDLE_VERSION}" "${BUNDLE_MANUFACTURER}" "Effect")
    endif()

    set(_built_formats)
    foreach(_fmt VST3 CLAP)
        if(TARGET ${target}_${_fmt})
            list(APPEND _built_formats ${_fmt})
        endif()
    endforeach()
    set(PULP_${target}_SOURCE_GIT_SHA "" CACHE INTERNAL "")
    set(PULP_${target}_SOURCE_GIT_DIRTY "" CACHE INTERNAL "")
    foreach(_fmt IN LISTS _built_formats)
        _pulp_attach_build_info(${target} ${target}_${_fmt} ${_fmt}
            "${BUNDLE_BUNDLE_NAME}" "${BUNDLE_BUNDLE_ID}" "${BUNDLE_VERSION}"
            "${BUNDLE_MANUFACTURER}")
    endforeach()
    if(_built_formats)
        list(JOIN _built_formats ";" _built_formats_display)
    else()
        set(_built_formats_display "none")
    endif()
    message(STATUS "Pulp plugin BUNDLE: ${target} (formats: ${_built_formats_display})")
endfunction()
