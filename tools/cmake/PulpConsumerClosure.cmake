# PulpConsumerClosure.cmake — what does an INSTALLED Pulp SDK cost a consumer?
#
# PulpLinkFloor.cmake asks the same shape of question about a target inside
# Pulp's own build. This module asks it from the other side of the install
# boundary: a project that ran find_package(Pulp CONFIG) has no source tree, no
# options, and no in-tree aliases. All it has are the IMPORTED targets
# PulpTargets.cmake recreated, so that is the only thing worth measuring. An
# edge that exists in-tree but is stripped by $<BUILD_INTERFACE:> is invisible
# here, and correctly so: the consumer never pays for it.
#
# Why the resolved property rather than the source text, restated because it is
# the whole reason this can be trusted: a PRIVATE link to a static library is
# recorded as $<LINK_ONLY:dep> in INTERFACE_LINK_LIBRARIES and still lands on
# the consumer's link line, because a static archive cannot resolve its own
# dependencies. Reading `PRIVATE` in a CMakeLists and concluding "not
# propagated" under-reports, which is the one error that makes an exclusion
# audit worthless.
#
# What this instrument CANNOT see, stated plainly so a clean run is not
# over-read:
#
#   1. It reports one configuration of one SDK. A dependency behind an option
#      that was OFF when the SDK was built is genuinely absent, not forgiven,
#      and an exclusion asserted against it proves nothing. That is what
#      pulp_consumer_exclusion_liveness() exists to say out loud, and why a
#      caller must treat an unenforceable group as unmeasured rather than
#      clean.
#   2. It sees LINK EDGES, never archive CONTENTS. A dependency compiled
#      directly into a static library Pulp does export has no edge to find, so
#      this reports the consumer does not link it while the object code is in
#      the binary regardless. Symbol-level auditing is a separate instrument
#      and the two disagree by design; pair them.
#
# The entry-resolution walk below is deliberately a second copy of the one in
# PulpLinkFloor.cmake rather than a shared helper. That module is part of
# Pulp's own build and is not installed, while this one has to be readable by a
# project that has only a staged prefix. Sharing it would mean either shipping
# an in-tree gate module inside the SDK or making an external fixture depend on
# a source tree it is specifically written not to have. The duplicated part is
# the stable half (CMake's link-property grammar); if that grammar ever changes
# both copies want the same edit, and the self-test beside this file is what
# would catch a copy that drifted.

include_guard(GLOBAL)

# Resolve a link-property entry to the targets it names.
#
# Entries arrive in whatever spelling CMake stored: a plain target, an alias, a
# framework flag, an absolute path, or a generator expression. Rather than
# parse that grammar, strip the expression scaffolding and keep every fragment
# that survives if(TARGET); a non-target fragment such as `LINK_ONLY` or
# `-framework` simply fails the test and is dropped.
#
# $<COMPILE_ONLY:...> is the one expression whose meaning cannot be recovered
# after stripping. It is a usage requirement that is deliberately NOT linked,
# so keeping its operand would invent an edge. It is skipped whole.
function(_pulp_consumer_entry_targets out_var)
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
            # $<IF:...,a,b> separates operands with commas, so a target behind
            # one is only visible once commas break too.
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
    list(REMOVE_DUPLICATES _result)
    set("${out_var}" "${_result}" PARENT_SCOPE)
endfunction()

# Transitive link closure of one or more roots, as a sorted target list.
#
# The roots are included in the result. IMPORTED targets carry their edges on
# INTERFACE_LINK_LIBRARIES only, which is the installed-SDK case; LINK_LIBRARIES
# is read as well so the same function works on a consumer's own executable.
function(pulp_consumer_link_closure out_var)
    set(_pending "${ARGN}")
    set(_seen "")
    while(_pending)
        list(POP_FRONT _pending _target)
        if(NOT TARGET "${_target}")
            continue()
        endif()
        get_target_property(_aliased "${_target}" ALIASED_TARGET)
        if(_aliased)
            set(_target "${_aliased}")
        endif()
        if("${_target}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_target}")

        set(_entries "")
        foreach(_property INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
            get_target_property(_value "${_target}" ${_property})
            if(_value)
                list(APPEND _entries ${_value})
            endif()
        endforeach()
        _pulp_consumer_entry_targets(_next ${_entries})
        foreach(_candidate IN LISTS _next)
            if(NOT "${_candidate}" IN_LIST _seen)
                list(APPEND _pending "${_candidate}")
            endif()
        endforeach()
    endwhile()
    list(SORT _seen)
    set("${out_var}" "${_seen}" PARENT_SCOPE)
endfunction()

# Classify one exclusion group against an SDK and a measured closure.
#
# A group names a capability a clean consumer does not want (a JS engine, a TLS
# stack) and lists the targets that realize it in an installed SDK. Three
# outcomes, and the third is the one that makes the other two mean anything:
#
#   violated      at least one realizing target is in the closure. Names it.
#   clean         realizing targets exist in the SDK and none is reached.
#   unenforceable no realizing target exists in this SDK at all, so the
#                 exclusion is vacuous here and proves nothing.
#
# Reporting `unenforceable` as `clean` is the failure mode this function exists
# to prevent: an SDK built with the GPU off contains no Skia target, so a naive
# "is skia::skia in the closure" check passes for a reason that has nothing to
# do with the consumer being clean.
function(pulp_consumer_classify_exclusion group_name status_var offenders_var)
    cmake_parse_arguments(_arg "" "" "CLOSURE;REALIZED_BY" ${ARGN})

    set(_present "")
    set(_offenders "")
    foreach(_candidate IN LISTS _arg_REALIZED_BY)
        if(TARGET "${_candidate}")
            list(APPEND _present "${_candidate}")
            if("${_candidate}" IN_LIST _arg_CLOSURE)
                list(APPEND _offenders "${_candidate}")
            endif()
        endif()
    endforeach()

    if(NOT _present)
        set("${status_var}" "unenforceable" PARENT_SCOPE)
        set("${offenders_var}" "" PARENT_SCOPE)
    elseif(_offenders)
        list(SORT _offenders)
        set("${status_var}" "violated" PARENT_SCOPE)
        set("${offenders_var}" "${_offenders}" PARENT_SCOPE)
    else()
        set("${status_var}" "clean" PARENT_SCOPE)
        set("${offenders_var}" "" PARENT_SCOPE)
    endif()
endfunction()

# Name the direct edge that carries `dependency` into `closure`.
#
# A violation report that says only "mbedtls is on your link line" leaves the
# reader to find out why. This walks the closure looking for the target whose
# own interface names the dependency, so the report can say which Pulp library
# introduced it. Several targets may carry the same edge; all are returned.
function(pulp_consumer_edge_carriers out_var dependency)
    cmake_parse_arguments(_arg "" "" "CLOSURE" ${ARGN})
    set(_carriers "")
    foreach(_target IN LISTS _arg_CLOSURE)
        if(NOT TARGET "${_target}" OR "${_target}" STREQUAL "${dependency}")
            continue()
        endif()
        set(_entries "")
        foreach(_property INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
            get_target_property(_value "${_target}" ${_property})
            if(_value)
                list(APPEND _entries ${_value})
            endif()
        endforeach()
        _pulp_consumer_entry_targets(_direct ${_entries})
        if("${dependency}" IN_LIST _direct)
            list(APPEND _carriers "${_target}")
        endif()
    endforeach()
    list(SORT _carriers)
    set("${out_var}" "${_carriers}" PARENT_SCOPE)
endfunction()

# How does `carrier` hold `dependency`: as a plain interface entry, or wrapped
# in $<LINK_ONLY:>?
#
# The distinction decides what cutting the edge would take, so a report that
# omits it is not actionable. A plain INTERFACE_LINK_LIBRARIES entry is a
# declared public dependency and removing it is an interface change. A
# LINK_ONLY entry is CMake's own record of a PRIVATE link to a static library:
# nobody wrote it, the archive's inability to resolve its own symbols created
# it, and it goes away by making the dependency not be linked into that archive
# rather than by editing an interface.
#
# Returns a `;`-separated list when a target holds the dependency more than one
# way, and `none` when the dependency is not directly held at all.
function(pulp_consumer_edge_kind out_var carrier dependency)
    set(_kinds "")
    foreach(_property INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
        get_target_property(_value "${carrier}" ${_property})
        if(NOT _value)
            continue()
        endif()
        foreach(_entry IN LISTS _value)
            _pulp_consumer_entry_targets(_named "${_entry}")
            if(NOT "${dependency}" IN_LIST _named)
                continue()
            endif()
            if(_entry MATCHES "LINK_ONLY")
                list(APPEND _kinds "${_property}/LINK_ONLY")
            else()
                list(APPEND _kinds "${_property}/direct")
            endif()
        endforeach()
    endforeach()
    if(NOT _kinds)
        set(_kinds "none")
    endif()
    list(REMOVE_DUPLICATES _kinds)
    set("${out_var}" "${_kinds}" PARENT_SCOPE)
endfunction()

# Shortest link path from `root` to `goal`, as `a -> b -> c`.
#
# Breadth-first, so the reported path is a shortest one rather than whichever
# the depth-first closure walk happened to take. A shortest path is the right
# thing to print because it names the fewest edges anybody has to cut, and it
# keeps the report stable when an unrelated module gains a longer route to the
# same dependency. Returns an empty string when goal is unreachable.
function(pulp_consumer_shortest_path out_var root goal)
    if("${root}" STREQUAL "${goal}")
        set("${out_var}" "${root}" PARENT_SCOPE)
        return()
    endif()

    set(_frontier "${root}")
    set(_visited "${root}")
    # Parallel lists acting as a map: _from[i] was reached from _via[i].
    set(_from "")
    set(_via "")
    set(_found FALSE)

    while(_frontier AND NOT _found)
        set(_next_frontier "")
        foreach(_target IN LISTS _frontier)
            set(_entries "")
            foreach(_property INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
                get_target_property(_value "${_target}" ${_property})
                if(_value)
                    list(APPEND _entries ${_value})
                endif()
            endforeach()
            _pulp_consumer_entry_targets(_neighbours ${_entries})
            foreach(_neighbour IN LISTS _neighbours)
                if("${_neighbour}" IN_LIST _visited)
                    continue()
                endif()
                list(APPEND _visited "${_neighbour}")
                list(APPEND _from "${_neighbour}")
                list(APPEND _via "${_target}")
                if("${_neighbour}" STREQUAL "${goal}")
                    set(_found TRUE)
                    break()
                endif()
                list(APPEND _next_frontier "${_neighbour}")
            endforeach()
            if(_found)
                break()
            endif()
        endforeach()
        set(_frontier "${_next_frontier}")
    endwhile()

    if(NOT _found)
        set("${out_var}" "" PARENT_SCOPE)
        return()
    endif()

    # Walk predecessors back to the root, then reverse.
    set(_path "${goal}")
    set(_cursor "${goal}")
    while(NOT "${_cursor}" STREQUAL "${root}")
        list(FIND _from "${_cursor}" _index)
        if(_index EQUAL -1)
            break()
        endif()
        list(GET _via ${_index} _cursor)
        list(APPEND _path "${_cursor}")
    endwhile()
    list(REVERSE _path)
    string(REPLACE ";" " -> " _path "${_path}")
    set("${out_var}" "${_path}" PARENT_SCOPE)
endfunction()
