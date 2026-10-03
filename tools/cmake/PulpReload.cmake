# Pulp hot-reload helpers.
# Included by PulpUtils.cmake to preserve its public compatibility surface.

function(pulp_add_reload_logic target)
    cmake_parse_arguments(RL "RESOLVE_FROM_HOST" "OUTPUT_NAME;PUBLISH_DIR" "SOURCES" ${ARGN})
    if(NOT RL_SOURCES)
        message(FATAL_ERROR "pulp_add_reload_logic(${target}): SOURCES is required")
    endif()
    if(NOT RL_OUTPUT_NAME)
        set(RL_OUTPUT_NAME "${target}")
    endif()
    if(APPLE)
        set(_rl_suffix ".dylib")
    elseif(WIN32)
        set(_rl_suffix ".dll")
    else()
        set(_rl_suffix ".so")
    endif()

    add_library(${target} MODULE ${RL_SOURCES})
    _pulp_pick_target(_RL_FORMAT_TARGET Pulp::format pulp::format)
    if(RL_RESOLVE_FROM_HOST AND NOT WIN32)
        # Thin: SDK headers only; resolve pulp::* from the host at dlopen so there
        # is a single copy of the SDK in the process (no duplicate ObjC classes /
        # statics). Enumerate the SDK targets' interface include dirs without
        # linking their archives.
        #
        # Intentionally an explicit dual-prefix list, NOT the in-tree
        # PULP_SDK_TARGETS (PulpInstallRules.cmake): this helper must also work for
        # downstream find_package(Pulp) consumers, who get imported `Pulp::*`
        # targets and no PULP_SDK_TARGETS. Each entry is if(TARGET)-guarded, so a
        # missing module is skipped silently — when a NEW SDK module's headers are
        # needed by thin logic, add it here.
        foreach(_rl_dep Pulp::format pulp::format Pulp::view pulp::view Pulp::canvas pulp::canvas
                        Pulp::state pulp::state Pulp::audio pulp::audio Pulp::midi pulp::midi
                        Pulp::runtime pulp::runtime Pulp::platform pulp::platform
                        Pulp::events pulp::events Pulp::signal pulp::signal)
            if(TARGET ${_rl_dep})
                target_include_directories(${target} PRIVATE
                    $<TARGET_PROPERTY:${_rl_dep},INTERFACE_INCLUDE_DIRECTORIES>)
            endif()
        endforeach()
        if(APPLE)
            target_link_options(${target} PRIVATE -undefined dynamic_lookup)
        endif()
        # Linux: a MODULE may keep undefined symbols, resolved from the host at
        # dlopen — no extra flag needed.
        #
        # The thin logic MUST compile at the SAME C++ standard as the SDK/host or
        # the build-fingerprint's cpp_standard field mismatches and the reload is
        # rejected. A static logic links pulp::format (PUBLIC cxx_std_23) and so
        # inherits C++23; a THIN logic links no archives, so it would fall back to
        # the root CMAKE_CXX_STANDARD (20) → host=202302 vs logic=202002 mismatch.
        # Match pulp-format, which sets CXX_STANDARD 23 authoritatively
        # (core/format/CMakeLists.txt). Keep in lockstep if the SDK bumps standard.
        set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    else()
        if(RL_RESOLVE_FROM_HOST AND WIN32)
            message(WARNING "pulp_add_reload_logic(${target}): RESOLVE_FROM_HOST is "
                            "unsupported on Windows; using the static link model.")
        endif()
        target_link_libraries(${target} PRIVATE ${_RL_FORMAT_TARGET})
    endif()
    set_target_properties(${target} PROPERTIES
        PREFIX "" OUTPUT_NAME "${RL_OUTPUT_NAME}" SUFFIX "${_rl_suffix}"
        POSITION_INDEPENDENT_CODE ON)

    if(RL_PUBLISH_DIR)
        # Publish the freshly-built module to the watched path so the shell finds
        # its DSP the moment the host loads it. NB: a literal $ENV{HOME} in
        # PUBLISH_DIR is the CONFIGURING user's home (captured now); a plugin
        # loaded by a different user should set PULP_RELOAD_LOGIC_PATH instead.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${RL_PUBLISH_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${target}>"
                    "${RL_PUBLISH_DIR}/${RL_OUTPUT_NAME}${_rl_suffix}"
            COMMENT "Publishing reload logic to ${RL_PUBLISH_DIR}/${RL_OUTPUT_NAME}${_rl_suffix}"
            VERBATIM)
    endif()
endfunction()

# ── pulp_reload_host — export a host's SDK symbols for thin reload logic ───
#
# A logic library built with pulp_add_reload_logic(... RESOLVE_FROM_HOST) leaves
# its pulp::* references undefined, to be resolved from whatever HOST dlopen's it
# (the standalone app, the plugin bundle, a capture tool). For that to work the
# host must publish its (statically-linked) SDK symbols in its dynamic symbol
# table. Call this on each host target that loads thin logic. No-op on Windows.
function(pulp_reload_host target)
    if(WIN32)
        return()
    elseif(APPLE)
        target_link_options(${target} PRIVATE -Wl,-export_dynamic)
    else()  # Linux / other ELF
        target_link_options(${target} PRIVATE -rdynamic)
    endif()
endfunction()

# Like pulp_reload_host, but ALSO force-retains + exports the editor ABI surface
# (View/Label/widgets) so a UI-carrying hot-reload logic (its create_view()
# resolves pulp::view from the host at dlopen) can render its editor inside this
# host. Use this instead of pulp_reload_host for a host that loads a logic which
# builds its own UI. (Live-swap M2b — see planning/2026-07-03-m2b-...) The plain
# host would dead-strip the editor symbols (nothing in the host references them);
# force-loading pulp-view-reload-exports keeps them, and export_dynamic below
# makes them resolvable by the dlopened logic. Force-loads ONLY the tiny
# curated-surface lib, NOT all of view-core (which drags in the mac-incompatible
# SDL-host TU).
function(pulp_reload_host_ui target)
    pulp_reload_host(${target})
    if(TARGET pulp-view-reload-exports)
        target_link_libraries(${target} PRIVATE pulp-view-reload-exports)
        if(APPLE)
            target_link_options(${target} PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pulp-view-reload-exports>"
                # The plugin's -exported_symbols_list exports only the entry point
                # and localizes everything else, defeating -export_dynamic for the
                # SDK symbols a THIN (RESOLVE_FROM_HOST) logic resolves at dlopen.
                # -exported_symbol ADDS patterns to the export set: the whole pulp::
                # surface (mangled Itanium prefix _ZN4pulp..., leading '_' on Mach-O)
                # so the dlopened logic resolves BOTH its editor (pulp::view
                # View/Label/widgets) AND its Processor base (pulp::format virtuals
                # like create_ara_document_controller) + any state/canvas/midi it uses.
                "-Wl,-exported_symbol,__ZN4pulp*"
                "-Wl,-exported_symbol,__ZNK4pulp*"
                "-Wl,-exported_symbol,__ZTVN4pulp*"    # vtables
                "-Wl,-exported_symbol,__ZTIN4pulp*"    # typeinfo
                "-Wl,-exported_symbol,__ZTSN4pulp*")   # typeinfo name
        elseif(NOT WIN32)
            target_link_options(${target} PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pulp-view-reload-exports>"
                "-Wl,--no-whole-archive")
        endif()
    endif()
endfunction()
