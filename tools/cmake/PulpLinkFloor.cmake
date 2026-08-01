# PulpLinkFloor.cmake — what does a target actually link, and is it within bound?
#
# The engine-side floor check (tools/scripts/timeline_engine_dependency_floor_check.py)
# is outbound: for each engine module it asks "does this module reach up?". This
# file asks the inbound question about a *consumer* — a plugin, an app, a test —
# "what does linking you cost, and did you say so?".
#
# The two questions need different instruments and this one cannot be a text
# scan, for two reasons that are properties of CMake rather than of style:
#
#   1. A PRIVATE link to a static library still propagates. CMake records it as
#      `$<LINK_ONLY:dep>` in INTERFACE_LINK_LIBRARIES, because a static archive
#      cannot resolve its own dependencies, so the consumer's link line carries
#      it. A reader of the source text sees the PRIVATE keyword and has to model
#      that rule to get the right answer; a reader of the resolved property
#      observes it. Modelling it wrongly under-reports, which is the one failure
#      mode that makes a floor check worthless.
#   2. A plugin's link edges are not written down anywhere a parser can read.
#      `pulp_add_plugin(StepSequencer FORMATS CLAP)` creates StepSequencer_CLAP
#      inside a function, links `${_PULP_VIEW_TARGET}` — a variable chosen by
#      `_pulp_pick_target` — and only when PULP_HAS_CLAP. Following that from
#      text means evaluating CMake variables, functions and conditionals, i.e.
#      re-implementing CMake. The engine check gets away with text only because
#      it reads core/<module>/CMakeLists.txt, where every link is a literal.
#
# The same trade is already recorded in core/view/CMakeLists.txt, which prefers
# `get_target_property(... LINK_LIBRARIES)` over grepping the built binary
# because "the symbol-grep passes on a misconfigured build. The link INTERFACE
# is the reliable signal, available here at configure time."
#
# What this instrument cannot see, stated plainly so a green run is not
# over-read: it reports the configuration being configured. A link that only
# exists under WIN32 is invisible to a macOS configure, and a module excluded by
# an option is genuinely absent rather than forgiven. A floor asserted here is a
# claim about this build, and it takes a run per configuration to be a claim
# about all of them.

include_guard(GLOBAL)

if(NOT DEFINED PULP_LINK_FLOOR_ROOT)
    get_filename_component(PULP_LINK_FLOOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

# ── Tiers ────────────────────────────────────────────────────────────────────
# A tier is the bound a consumer claims, named centrally so two targets claiming
# the same tier are held to the same closure and neither can widen it alone. The
# names follow the module directories under core/, which is also the vocabulary
# MODULE_FLOORS uses, so a bound and a floor can be read against each other.
#
# Tiers are cumulative by construction rather than by inheritance: each lists
# its whole closure, so reading one row tells you the entire cost. An extra
# module in a row is a deliberate, reviewable widening of every consumer of that
# tier at once.

# The rungs a sequencer document and its editor kernel need, and nothing above
# them. This is the bound behind "a piano roll you can put in a plugin": the
# editor rung reaches the document model and the timebase and stops, so a plugin
# drawing a piano roll over its own engine does not acquire a transport.
#
# No target claims this tier yet. It is the bound a consumer that actually
# carries the editor would claim, and such a consumer should pair it with
# REQUIRE timeline timeline_editor — a tier alone cannot say the editor arrived.
set(PULP_LINK_FLOOR_TIER_sequencer-editor
    platform runtime timebase timeline timeline_editor)

# A loadable plugin binary: the format adapter it is packaged as, and the
# audio/MIDI types that adapter and the engine both speak.
#
# The timeline rungs are deliberately NOT here. A tier is an upper bound, so
# naming a module a claimant does not link is not a harmless over-estimate — it
# reads as a proven link and is not one. StepSequencer_CLAP reaches `timeline`
# only through the view stack (recorded as debt, where a fact about today
# belongs) and does not reach `timeline_editor` at all. A plugin that genuinely
# carries the editor states so with REQUIRE.
set(PULP_LINK_FLOOR_TIER_sequencer-plugin
    platform runtime timebase audio midi format)

# A loadable sequencer plugin that owns both the document and editor rungs.
# This is the union of the two bounds above, not a larger application tier: a
# plugin claiming it still has to name timeline and timeline_editor in REQUIRE.
set(PULP_LINK_FLOOR_TIER_sequencer-plugin-editor
    platform runtime timebase timeline timeline_editor audio midi format)

# ── Debt ─────────────────────────────────────────────────────────────────────
# What a target's link closure drags in beyond its tier, recorded per target.
# An entry is a fact about the artifact, not a permission to build on: it says
# "this edge exists today", and deleting it once the edge is cut tightens the
# gate with no other edit. Kept out of the tiers on purpose — a tier is a claim
# several targets share, and nothing here has earned that reach for all of them.
#
# pulp_assert_link_floor() rejects a debt entry that is no longer reached and a
# debt entry that duplicates its tier, so the list cannot outlive its subject.
#
# That "no longer reached" check measures one configure, and a project does not
# have one closure. A module whose subdirectory is behind an option is absent by
# construction wherever the option is off, and an edge behind a platform guard
# takes everything it alone reached with it. Read as rot, that absence fails the
# gate in its healthy direction — it asks for an entry to be DELETED because a
# narrower configure could not reach it, and deleting it then breaks the wider
# configure where the link genuinely exists. An upper bound is not violated by
# reaching less, so entries that a documented guard can remove are declared
# CONDITIONAL below and exempted from that one check only.
#
# StepSequencer_CLAP: measured, not chosen. Two edges the plugin does not
# write and cannot cut from its own CMakeLists account for all of it.
#
#   pulp-view — every plugin links the view stack whether or not it draws.
#     VST3, CLAP and AU each link ${_PULP_VIEW_TARGET} unconditionally in
#     tools/cmake/PulpPluginFormats.cmake and FATAL_ERROR if it is absent. The
#     stack then reaches the plugin host and, through it, the transport:
#       StepSequencer_CLAP -> pulp-view -> pulp-view-script -> pulp-view-core
#         -> pulp-host -> pulp-playback -> pulp-timeline
#     which is the only reason this plugin reaches pulp-timeline at all: the
#     sequencer's own code carries no pulp/timeline include and no timeline
#     link. view, host, playback, timeline, render and canvas all arrive this
#     way, so cutting the one unconditional link would delete six entries at
#     once.
#
#   pulp-format — the adapter the plugin is packaged as. Its own closure brings
#     the parameter store and the buffer types: state and events directly,
#     signal and sample_bank_manifest through pulp-audio, plus graph and
#     native-components.
#
# None of this is a permission. It is the bill the format packaging currently
# presents, written down so the gate polices the artifact that exists instead
# of passing green on a bound the binary never honoured.
set(PULP_LINK_FLOOR_DEBT_StepSequencer_CLAP
    canvas events graph native-components sample_bank_manifest signal state view)

# The four entries above that a guard can remove, and the guard that removes
# each. They are still debt — reached on an ordinary desktop configure, and
# still rejected if they appear where the tier already grants them — but their
# ABSENCE is not rot, so it is not reported as such.
#
#   render    — CMakeLists.txt gates `add_subdirectory(core/render)` on
#               PULP_ENABLE_GPU, so with GPU off the pulp-render target is never
#               defined and no closure can contain it. A worktree whose
#               external/skia-build is headers-only forces exactly that, which
#               is an ordinary fresh-checkout state rather than an exotic one.
#
#   host      — CMakeLists.txt gates `add_subdirectory(core/host)` on NOT IOS,
#   playback    and core/view drops the pulp-view-core -> pulp::host edge there
#   timeline    too: iOS disallows dlopen of third-party plugins, so hosting is
#               not built at all. That single edge is the plugin's only route to
#               all three (StepSequencer_CLAP -> pulp-view -> pulp-view-script
#               -> pulp-view-core -> pulp-host -> pulp-playback -> pulp-timeline),
#               so the guard takes the chain, not just its first rung.
#
# An entry here is a weaker claim than a plain debt entry: nothing detects it
# going stale, because "absent" is indistinguishable from "absent for the
# declared reason". Keep the list short, and move an entry back to the debt list
# above the moment its guard stops applying to it.
set(PULP_LINK_FLOOR_DEBT_CONDITIONAL_StepSequencer_CLAP
    host playback render timeline)

set(PULP_LINK_FLOOR_DEBT_TimelinePluginProof_CLAP
    canvas events graph host native-components playback render
    sample_bank_manifest signal state view)

# ── Walk ─────────────────────────────────────────────────────────────────────

# Return the targets named by a list of link entries.
#
# Entries arrive in whatever spelling CMake stored: a plain target, an alias, a
# framework flag, an absolute path, or a generator expression. Rather than parse
# the grammar, strip the expression scaffolding and keep every fragment that is
# actually a target — a non-target fragment such as `LINK_ONLY` or `-framework`
# simply fails the `if(TARGET)` test and is dropped.
#
# `$<COMPILE_ONLY:...>` is the one expression whose meaning cannot be recovered
# after stripping: it is a usage requirement that is deliberately NOT linked, so
# keeping its operand would invent an edge. It is skipped whole.
function(_pulp_link_floor_entry_targets out_var)
    set(_result "")
    foreach(_entry IN LISTS ARGN)
        if(_entry MATCHES "COMPILE_ONLY")
            continue()
        endif()
        set(_candidates "${_entry}")
        if(_entry MATCHES "\\$<")
            string(REGEX REPLACE "\\$<[A-Za-z_]+:" ";" _candidates "${_candidates}")
            string(REPLACE "$<" ";" _candidates "${_candidates}")
            string(REPLACE ">" ";" _candidates "${_candidates}")
            # $<IF:...,a,b> and friends separate their operands with commas, so a
            # target behind one is only visible once commas break too.
            string(REPLACE "," ";" _candidates "${_candidates}")
        endif()
        foreach(_candidate IN LISTS _candidates)
            string(STRIP "${_candidate}" _candidate)
            if(_candidate STREQUAL "" OR NOT TARGET "${_candidate}")
                continue()
            endif()
            get_target_property(_aliased "${_candidate}" ALIASED_TARGET)
            if(_aliased)
                set(_candidate "${_aliased}")
            endif()
            list(APPEND _result "${_candidate}")
        endforeach()
    endforeach()
    if(_result)
        list(REMOVE_DUPLICATES _result)
    endif()
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

# Return the core/ module a target belongs to, or "" for anything else.
#
# Derived from SOURCE_DIR rather than from the target's name, so pulp-view,
# pulp-view-core and pulp-view-script all resolve to `view` without a naming
# convention to keep in step, and a target that merely starts with `pulp-` but
# lives elsewhere is not mistaken for a module.
function(_pulp_link_floor_module out_var target)
    set(${out_var} "" PARENT_SCOPE)
    get_target_property(_dir "${target}" SOURCE_DIR)
    if(NOT _dir)
        return()
    endif()
    file(RELATIVE_PATH _relative "${PULP_LINK_FLOOR_ROOT}" "${_dir}")
    if(_relative MATCHES "^core/([A-Za-z0-9_.+-]+)")
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endif()
endfunction()

# Walk everything `target` links, transitively, and report it.
#
#   MODULES_OUT  sorted core/ module names in the closure, excluding the
#                module the target itself lives in
#   TARGETS_OUT  every target reached, module or not
#   PATHS_OUT    one `a>b>c` chain per module, the shortest found, so a report
#                can name the edge that brought a module in rather than only
#                the module
#
# The root is read through LINK_LIBRARIES — its own direct links, both scopes —
# and every dependency through INTERFACE_LINK_LIBRARIES, which is precisely what
# that dependency propagates to whoever links it. Reading a dependency's
# LINK_LIBRARIES instead would invent edges: a shared library resolves its own
# PRIVATE links inside its artifact and charges the consumer nothing for them.
# A static library's PRIVATE links do reach the consumer, and CMake says so by
# writing them into INTERFACE_LINK_LIBRARIES wrapped in $<LINK_ONLY:>.
#
# Breadth-first over a frontier, with every target recorded on first arrival, so
# paths are shortest and the format->view->view-core->host->format cycle
# terminates rather than hanging.
function(pulp_link_closure target)
    cmake_parse_arguments(PLC "" "MODULES_OUT;TARGETS_OUT;PATHS_OUT" "" ${ARGN})
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "pulp_link_closure: no such target: ${target}")
    endif()

    set(_seen "${target}")
    set(_chains "${target}")
    set(_frontier "${target}")
    while(_frontier)
        set(_next "")
        foreach(_current IN LISTS _frontier)
            list(FIND _seen "${_current}" _position)
            list(GET _chains ${_position} _chain)

            set(_entries "")
            if(_current STREQUAL "${target}")
                get_target_property(_type "${_current}" TYPE)
                get_target_property(_imported "${_current}" IMPORTED)
                if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _imported)
                    get_target_property(_direct "${_current}" LINK_LIBRARIES)
                    if(_direct)
                        list(APPEND _entries ${_direct})
                    endif()
                endif()
            endif()
            get_target_property(_interface "${_current}" INTERFACE_LINK_LIBRARIES)
            if(_interface)
                list(APPEND _entries ${_interface})
            endif()

            _pulp_link_floor_entry_targets(_dependencies ${_entries})
            foreach(_dependency IN LISTS _dependencies)
                if(_dependency IN_LIST _seen)
                    continue()
                endif()
                list(APPEND _seen "${_dependency}")
                list(APPEND _chains "${_chain}>${_dependency}")
                list(APPEND _next "${_dependency}")
            endforeach()
        endforeach()
        set(_frontier "${_next}")
    endwhile()

    _pulp_link_floor_module(_own "${target}")
    set(_modules "")
    set(_paths "")
    foreach(_reached IN LISTS _seen)
        _pulp_link_floor_module(_module "${_reached}")
        if(_module STREQUAL "" OR _module STREQUAL "${_own}" OR _module IN_LIST _modules)
            continue()
        endif()
        list(FIND _seen "${_reached}" _position)
        list(GET _chains ${_position} _chain)
        list(APPEND _modules "${_module}")
        list(APPEND _paths "${_module}=${_chain}")
    endforeach()
    if(_modules)
        list(SORT _modules)
    endif()
    if(_paths)
        list(SORT _paths)
    endif()

    if(PLC_MODULES_OUT)
        set(${PLC_MODULES_OUT} "${_modules}" PARENT_SCOPE)
    endif()
    if(PLC_TARGETS_OUT)
        set(${PLC_TARGETS_OUT} "${_seen}" PARENT_SCOPE)
    endif()
    if(PLC_PATHS_OUT)
        set(${PLC_PATHS_OUT} "${_paths}" PARENT_SCOPE)
    endif()
endfunction()

# Write a target's measured closure to ${CMAKE_BINARY_DIR}/link-floor/<target>.txt
# and echo the module list. A report, not a gate — this is how the closure gets
# read before anyone decides what the bound should be.
function(pulp_report_link_closure target)
    pulp_link_closure(${target} MODULES_OUT _modules TARGETS_OUT _targets PATHS_OUT _paths)
    set(_report "${CMAKE_BINARY_DIR}/link-floor/${target}.txt")
    set(_body "target: ${target}\n")
    string(APPEND _body "modules: ${_modules}\n\n")
    foreach(_path IN LISTS _paths)
        string(REPLACE "=" ": " _line "${_path}")
        string(REPLACE ">" " -> " _line "${_line}")
        string(APPEND _body "${_line}\n")
    endforeach()
    string(APPEND _body "\ntargets:\n")
    foreach(_reached IN LISTS _targets)
        string(APPEND _body "  ${_reached}\n")
    endforeach()
    file(WRITE "${_report}" "${_body}")
    message(STATUS "link-floor: ${target} reaches [${_modules}] (${_report})")
endfunction()

# Assert a target's link closure matches what it declares.
#
# TIER is an upper bound: nothing outside tier ∪ debt may be reached. On its own
# that is the only claim a tier can support, and it is worth being exact about
# how little it proves — a tier naming a module the target does not link is
# still green, because "reaches nothing extra" is satisfied most easily by
# reaching nothing at all. So an acceptance criterion of the form "the editor
# ships inside the plugin" cannot be read off a tier, however generous the tier
# looks.
#
# REQUIRE is the other half: an optional lower bound listing modules the closure
# MUST contain. It is what turns "did not drag the editing stack in" into a pair
# with "and did carry the piano roll", and it fails naming the modules that are
# absent, so the claim cannot quietly become true by deletion.
#
# The two lists stay orthogonal on purpose. REQUIRE grants nothing: a required
# module must also be declared in the tier or the debt list, or no closure could
# satisfy both bounds at once, and that contradiction is reported as such rather
# than letting REQUIRE become a third permission list nobody reviews.
#
# Fails the configure, in the manner of core/view's native-only guard: a link
# that breaches the bound is a contract violation, and letting it build and be
# discovered later is how the bound stops meaning anything.
function(pulp_assert_link_floor target)
    cmake_parse_arguments(PALF "" "TIER" "REQUIRE" ${ARGN})
    if(NOT PALF_TIER)
        message(FATAL_ERROR "pulp_assert_link_floor(${target}): TIER is required")
    endif()
    if(NOT DEFINED PULP_LINK_FLOOR_TIER_${PALF_TIER})
        message(FATAL_ERROR
            "pulp_assert_link_floor(${target}): unknown tier '${PALF_TIER}'. "
            "Tiers are declared in tools/cmake/PulpLinkFloor.cmake.")
    endif()
    set(_allowed ${PULP_LINK_FLOOR_TIER_${PALF_TIER}})
    set(_debt ${PULP_LINK_FLOOR_DEBT_${target}})
    # Conditional debt: allowed in the closure exactly as debt is, but exempt
    # from the "no longer linked" check, because a guarded subdirectory or a
    # guarded edge legitimately removes it in a narrower configure.
    set(_conditional ${PULP_LINK_FLOOR_DEBT_CONDITIONAL_${target}})

    pulp_link_closure(${target} MODULES_OUT _modules PATHS_OUT _paths)

    set(_failures "")
    set(_over FALSE)
    foreach(_module IN LISTS _modules)
        if(_module IN_LIST _allowed OR _module IN_LIST _debt
                OR _module IN_LIST _conditional)
            continue()
        endif()
        set(_over TRUE)
        # Split on the first `=` rather than matching the module name as a
        # pattern: a module directory may legitimately contain `.` or `+`, and
        # a name read as a regex would silently report the wrong chain.
        set(_chain "${_module}")
        foreach(_path IN LISTS _paths)
            string(FIND "${_path}" "=" _split)
            string(SUBSTRING "${_path}" 0 ${_split} _named)
            if(_named STREQUAL "${_module}")
                math(EXPR _split "${_split} + 1")
                string(SUBSTRING "${_path}" ${_split} -1 _chain)
                string(REPLACE ">" " -> " _chain "${_chain}")
            endif()
        endforeach()
        list(APPEND _failures
            "  pulp/${_module} is outside tier '${PALF_TIER}', reached by ${_chain}")
    endforeach()
    foreach(_entry IN LISTS _debt)
        if(_entry IN_LIST _allowed)
            set(_over TRUE)
            list(APPEND _failures
                "  debt entry '${_entry}' is already inside tier '${PALF_TIER}'; delete it")
        elseif(NOT _entry IN_LIST _modules)
            set(_over TRUE)
            list(APPEND _failures
                "  debt entry '${_entry}' is no longer linked. Delete it to tighten the bound")
        endif()
    endforeach()

    # Conditional entries keep every declaration check that does not depend on
    # this configure reaching them. Only the absence check is relaxed; naming a
    # module the tier already grants is still a stale declaration, and naming
    # one in both lists is a contradiction rather than a belt-and-braces.
    foreach(_entry IN LISTS _conditional)
        if(_entry IN_LIST _allowed)
            set(_over TRUE)
            # One string, not two arguments: list(APPEND) would make a second
            # element and the JOIN below would break the sentence across lines.
            string(CONCAT _line
                "  conditional debt entry '${_entry}' is already inside tier "
                "'${PALF_TIER}'; delete it")
            list(APPEND _failures "${_line}")
        elseif(_entry IN_LIST _debt)
            set(_over TRUE)
            string(CONCAT _line
                "  '${_entry}' is in both PULP_LINK_FLOOR_DEBT_${target} and "
                "PULP_LINK_FLOOR_DEBT_CONDITIONAL_${target}; an entry is either "
                "checked for rot or exempt from that check, not both")
            list(APPEND _failures "${_line}")
        endif()
    endforeach()

    # The lower bound. Absence is the failure mode an upper bound cannot see, so
    # it is checked against the same measured closure rather than against the
    # declaration: a REQUIRE entry is satisfied by the module being reached, by
    # whatever edge, and by nothing else.
    set(_missing FALSE)
    set(_undeclared FALSE)
    foreach(_required IN LISTS PALF_REQUIRE)
        if(NOT _required IN_LIST _modules)
            set(_missing TRUE)
            list(APPEND _failures
                "  pulp/${_required} is required by this target but is not linked at all")
        endif()
        if(NOT _required IN_LIST _allowed AND NOT _required IN_LIST _debt
                AND NOT _required IN_LIST _conditional)
            set(_undeclared TRUE)
            # One string, not two arguments: list(APPEND) would make a second
            # element and the JOIN below would break the sentence across lines.
            string(CONCAT _line
                "  required module '${_required}' is in neither tier "
                "'${PALF_TIER}' nor PULP_LINK_FLOOR_DEBT_${target}, "
                "so no closure could satisfy both bounds")
            list(APPEND _failures "${_line}")
        endif()
    endforeach()

    if(_failures)
        list(JOIN _failures "\n" _detail)
        set(_remedy "")
        if(_over)
            string(APPEND _remedy
                "Either cut the link, widen tier '${PALF_TIER}' in "
                "tools/cmake/PulpLinkFloor.cmake for every target that claims it, or "
                "record the edge in PULP_LINK_FLOOR_DEBT_${target} with the reason it "
                "exists. ")
        endif()
        if(_missing)
            string(APPEND _remedy
                "A REQUIRE entry claims the artifact carries that module. Restore the "
                "link if it was lost, or drop the entry if the claim is stale -- but do "
                "not leave it standing, because a bound that no longer matches the "
                "binary is the failure REQUIRE exists to catch. ")
        endif()
        if(_undeclared)
            string(APPEND _remedy
                "REQUIRE grants no reach of its own: declare the module in the tier or "
                "in the debt list as well, or drop it from REQUIRE. ")
        endif()
        message(FATAL_ERROR
            "pulp_assert_link_floor(${target}): declared bound and measured closure disagree.\n"
            "${_detail}\n"
            "Closure measured this configure: ${_modules}\n"
            "${_remedy}")
    endif()
    if(PALF_REQUIRE)
        message(STATUS
            "link-floor: ${target} within tier '${PALF_TIER}' and carries [${PALF_REQUIRE}]")
    else()
        message(STATUS "link-floor: ${target} within tier '${PALF_TIER}'")
    endif()
endfunction()
