# PulpReleaseArtifactScan.cmake — the one scanner behind every "a release
# artifact must not contain X" check.
#
# The list of forbidden patterns lives beside this file in
# release_artifact_forbidden.json. Each marker names the GATES (build
# conditions: an option that is OFF, a profile that is stripped) under which it
# is forbidden and the KIND of evidence it is looked for in:
#   symbol      a substring of the demangled DEFINED symbol table
#               (`nm -C --defined-only`; an undefined `U` reference to a
#               type is not evidence the implementation was linked)
#   string      a substring of the file's printable runs (file(STRINGS))
#   dependency  a substring of the loader table (otool -L / readelf -d / dumpbin)
# A registration site passes the gates that hold for the artifact it scans; the
# scanner applies every marker carrying one of them. Adding a marker is one
# entry in the JSON — AssertNoJsSymbols.cmake, AssertNoTracingSymbols.cmake,
# check_control_shipping_artifact.cmake and test/cmake/reload_ship_symbol_check.cmake
# are thin wrappers over the functions below, so every artifact class (SDK
# libraries, plug-in bundles, standalones, the CLI, fixture objects) is held to
# the same list.
#
# Like its predecessors this scan is defense in depth, not the authoritative
# guarantee: a static archive member the linker never referenced leaves no
# symbol behind, so a scan can pass on a build that still linked the archive.
# The authoritative guards are configure-time (link-interface assertions,
# empty INTERFACE targets, compile definitions). This scan catches what those
# miss: a force-referenced or whole-archived leak, a literal that survived.
#
# Module use (include() then call):
#   pulp_release_artifact_load([<json>])                 # loads the list into the caller's scope
#   pulp_release_artifact_select(OUT_IDS <v> OUT_PATTERNS <v> KIND <kind>
#                                GATES <gate>... [IDS <id>...])
#   pulp_release_artifact_probe(ARTIFACT <path> GATES <gate>... [IDS <id>...]
#                               [SYMBOLS <var>] [STRINGS <var>] [DEPENDENCIES <var>]
#                               OUT_PRESENT <v> OUT_ABSENT <v> OUT_UNCHECKED <v>)
#   pulp_release_artifact_forbid(<probe args> [LABEL <text>])   # FATAL_ERROR on any hit
#   pulp_release_artifact_require(<probe args> [LABEL <text>])  # FATAL_ERROR on any miss (positive control)
#
# Script use:
#   cmake -DBIN=<artifact> -DGATES="tracing-off;js-off" [-DMODE=forbid|require]
#         [-DIDS=<id;...>] [-DLABEL=<text>] [-DLIST=<json>]
#         -P tools/cmake/PulpReleaseArtifactScan.cmake
include_guard(GLOBAL)
# The checks below use IN_LIST (CMP0057), which `cmake -P` and an includer
# without cmake_minimum_required() leave unset.
cmake_policy(VERSION 3.24)

set(PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST_DEFAULT
    "${CMAKE_CURRENT_LIST_DIR}/release_artifact_forbidden.json")

# Validity rules shared by the loader, the list self-test and the fixture
# generator. A string pattern must be printable ASCII without spaces or
# semicolons: the printable runs are joined into a CMake list with `;`, so a
# pattern containing one could match across two runs, and a space would end
# the run it sits in.
set(PULP_RELEASE_ARTIFACT_STRING_PATTERN_REGEX "^[!-:<-~]+$")
# A symbol pattern is identifiers joined by `::`; a trailing `::` names a
# class's members (`pulp::view::ScriptEngine::`), which matches only where a
# member is DEFINED, never a type name mentioned in another function's
# parameter list.
set(PULP_RELEASE_ARTIFACT_SYMBOL_PATTERN_REGEX
    "^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*(::)?$")
set(PULP_RELEASE_ARTIFACT_DEPENDENCY_PATTERN_REGEX "^[A-Za-z0-9._+-]+$")
set(PULP_RELEASE_ARTIFACT_KINDS symbol string dependency)

# Loads the JSON list into the caller's scope as
#   PULP_RELEASE_ARTIFACT_GATE_IDS       declared gate ids
#   PULP_RELEASE_ARTIFACT_MARKER_COUNT   number of markers
#   PULP_RELEASE_ARTIFACT_MARKER_<i>_{ID,KIND,PATTERN,GATES,REASON}
# A macro so the per-marker variables land where the caller can read them.
# Fails closed on a missing or malformed file: a scanner with no list is the
# vacuous pass this module exists to prevent.
macro(pulp_release_artifact_load)
    set(_pra_list "${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST_DEFAULT}")
    if(NOT "${ARGN}" STREQUAL "")
        set(_pra_list "${ARGN}")
    endif()
    if(NOT EXISTS "${_pra_list}")
        message(FATAL_ERROR
            "release-artifact scan: forbidden-marker list is missing: ${_pra_list}")
    endif()
    file(READ "${_pra_list}" _pra_json)
    string(JSON _pra_schema ERROR_VARIABLE _pra_err GET "${_pra_json}" schema)
    if(_pra_err OR NOT _pra_schema STREQUAL "dev.pulp.release/forbidden-markers@1")
        message(FATAL_ERROR
            "release-artifact scan: unsupported forbidden-marker list schema "
            "'${_pra_schema}' in ${_pra_list}")
    endif()
    set(PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST "${_pra_list}")
    set(PULP_RELEASE_ARTIFACT_GATE_IDS "")
    string(JSON _pra_gates GET "${_pra_json}" gates)
    string(JSON _pra_gate_count LENGTH "${_pra_gates}")
    if(_pra_gate_count GREATER 0)
        math(EXPR _pra_gate_last "${_pra_gate_count} - 1")
        foreach(_pra_g RANGE ${_pra_gate_last})
            string(JSON _pra_gate_id MEMBER "${_pra_gates}" ${_pra_g})
            list(APPEND PULP_RELEASE_ARTIFACT_GATE_IDS "${_pra_gate_id}")
        endforeach()
    endif()
    string(JSON _pra_markers GET "${_pra_json}" markers)
    string(JSON PULP_RELEASE_ARTIFACT_MARKER_COUNT LENGTH "${_pra_markers}")
    if(PULP_RELEASE_ARTIFACT_MARKER_COUNT GREATER 0)
        math(EXPR _pra_last "${PULP_RELEASE_ARTIFACT_MARKER_COUNT} - 1")
        foreach(_pra_i RANGE ${_pra_last})
            string(JSON _pra_m GET "${_pra_markers}" ${_pra_i})
            foreach(_pra_field id kind pattern reason)
                string(JSON _pra_v ERROR_VARIABLE _pra_err GET "${_pra_m}" ${_pra_field})
                if(_pra_err)
                    message(FATAL_ERROR
                        "release-artifact scan: marker ${_pra_i} lacks '${_pra_field}' in ${_pra_list}")
                endif()
                string(TOUPPER "${_pra_field}" _pra_upper)
                set(PULP_RELEASE_ARTIFACT_MARKER_${_pra_i}_${_pra_upper} "${_pra_v}")
            endforeach()
            string(JSON _pra_mg ERROR_VARIABLE _pra_err GET "${_pra_m}" gates)
            if(_pra_err)
                message(FATAL_ERROR
                    "release-artifact scan: marker ${_pra_i} lacks 'gates' in ${_pra_list}")
            endif()
            set(PULP_RELEASE_ARTIFACT_MARKER_${_pra_i}_GATES "")
            string(JSON _pra_mg_count LENGTH "${_pra_mg}")
            if(_pra_mg_count GREATER 0)
                math(EXPR _pra_mg_last "${_pra_mg_count} - 1")
                foreach(_pra_j RANGE ${_pra_mg_last})
                    string(JSON _pra_gate GET "${_pra_mg}" ${_pra_j})
                    list(APPEND PULP_RELEASE_ARTIFACT_MARKER_${_pra_i}_GATES "${_pra_gate}")
                endforeach()
            endif()
        endforeach()
    endif()
endmacro()

# Checks one marker's shape. Sets ${out_var} to an empty string when valid,
# else to the reason it is not.
function(pulp_release_artifact_validate_marker id kind pattern gates reason out_var)
    set(_problem "")
    if(NOT id MATCHES "^[a-z0-9][a-z0-9.-]*$")
        set(_problem "id '${id}' is not lowercase dotted-kebab")
    elseif(NOT kind IN_LIST PULP_RELEASE_ARTIFACT_KINDS)
        set(_problem "marker '${id}' has unknown kind '${kind}'")
    elseif(kind STREQUAL "string" AND NOT pattern MATCHES "${PULP_RELEASE_ARTIFACT_STRING_PATTERN_REGEX}")
        set(_problem "marker '${id}' string pattern must be printable ASCII without spaces or semicolons: '${pattern}'")
    elseif(kind STREQUAL "symbol" AND NOT pattern MATCHES "${PULP_RELEASE_ARTIFACT_SYMBOL_PATTERN_REGEX}")
        set(_problem "marker '${id}' symbol pattern must be identifiers joined by '::': '${pattern}'")
    elseif(kind STREQUAL "dependency" AND NOT pattern MATCHES "${PULP_RELEASE_ARTIFACT_DEPENDENCY_PATTERN_REGEX}")
        set(_problem "marker '${id}' dependency pattern must be a library basename stem: '${pattern}'")
    elseif("${gates}" STREQUAL "")
        set(_problem "marker '${id}' names no gate")
    elseif("${reason}" STREQUAL "")
        set(_problem "marker '${id}' has an empty reason")
    else()
        foreach(_gate IN LISTS gates)
            if(NOT _gate IN_LIST PULP_RELEASE_ARTIFACT_GATE_IDS)
                set(_problem "marker '${id}' names undeclared gate '${_gate}'")
            endif()
        endforeach()
    endif()
    set(${out_var} "${_problem}" PARENT_SCOPE)
endfunction()

# Parallel lists of the ids and patterns of one KIND that carry any of GATES
# or are named in IDS. Requires pulp_release_artifact_load() in an enclosing
# scope.
function(pulp_release_artifact_select)
    cmake_parse_arguments(_s "" "OUT_IDS;OUT_PATTERNS;KIND" "GATES;IDS" ${ARGN})
    if(NOT _s_KIND IN_LIST PULP_RELEASE_ARTIFACT_KINDS)
        message(FATAL_ERROR "pulp_release_artifact_select: KIND must be one of ${PULP_RELEASE_ARTIFACT_KINDS}")
    endif()
    if(NOT DEFINED PULP_RELEASE_ARTIFACT_MARKER_COUNT)
        message(FATAL_ERROR "pulp_release_artifact_select: call pulp_release_artifact_load() first")
    endif()
    foreach(_gate IN LISTS _s_GATES)
        if(NOT _gate IN_LIST PULP_RELEASE_ARTIFACT_GATE_IDS)
            message(FATAL_ERROR
                "release-artifact scan: unknown gate '${_gate}' (declared: ${PULP_RELEASE_ARTIFACT_GATE_IDS})")
        endif()
    endforeach()
    set(_ids "")
    set(_patterns "")
    if(PULP_RELEASE_ARTIFACT_MARKER_COUNT GREATER 0)
        math(EXPR _last "${PULP_RELEASE_ARTIFACT_MARKER_COUNT} - 1")
        foreach(_i RANGE ${_last})
            if(NOT PULP_RELEASE_ARTIFACT_MARKER_${_i}_KIND STREQUAL _s_KIND)
                continue()
            endif()
            set(_selected FALSE)
            if(PULP_RELEASE_ARTIFACT_MARKER_${_i}_ID IN_LIST _s_IDS)
                set(_selected TRUE)
            else()
                foreach(_gate IN LISTS PULP_RELEASE_ARTIFACT_MARKER_${_i}_GATES)
                    if(_gate IN_LIST _s_GATES)
                        set(_selected TRUE)
                    endif()
                endforeach()
            endif()
            if(_selected)
                list(APPEND _ids "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_ID}")
                list(APPEND _patterns "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_PATTERN}")
            endif()
        endforeach()
    endif()
    set(${_s_OUT_IDS} "${_ids}" PARENT_SCOPE)
    set(${_s_OUT_PATTERNS} "${_patterns}" PARENT_SCOPE)
endfunction()

# Looks every selected marker up in ARTIFACT. SYMBOLS / STRINGS / DEPENDENCIES
# name variables already holding the respective evidence (a caller that ran
# nm or otool itself passes them so the table is read once); when absent the
# symbol table and printable runs are extracted here, and dependency markers
# are reported UNCHECKED because the loader table is platform-specific and
# only a caller that scanned it can supply it. Each OUT_* list holds records
# `id (kind) pattern`.
function(pulp_release_artifact_probe)
    cmake_parse_arguments(_p ""
        "ARTIFACT;SYMBOLS;STRINGS;DEPENDENCIES;OUT_PRESENT;OUT_ABSENT;OUT_UNCHECKED"
        "GATES;IDS" ${ARGN})
    if(NOT _p_ARTIFACT OR NOT EXISTS "${_p_ARTIFACT}")
        message(FATAL_ERROR "release-artifact scan: artifact does not exist: '${_p_ARTIFACT}'")
    endif()
    if("${_p_GATES}" STREQUAL "" AND "${_p_IDS}" STREQUAL "")
        message(FATAL_ERROR "release-artifact scan: no GATES or IDS selected for ${_p_ARTIFACT}")
    endif()
    set(_present "")
    set(_absent "")
    set(_unchecked "")

    pulp_release_artifact_select(OUT_IDS _sym_ids OUT_PATTERNS _sym_patterns
        KIND symbol GATES ${_p_GATES} IDS ${_p_IDS})
    pulp_release_artifact_select(OUT_IDS _str_ids OUT_PATTERNS _str_patterns
        KIND string GATES ${_p_GATES} IDS ${_p_IDS})
    pulp_release_artifact_select(OUT_IDS _dep_ids OUT_PATTERNS _dep_patterns
        KIND dependency GATES ${_p_GATES} IDS ${_p_IDS})
    # A selection that names no marker would pass vacuously in both verdicts;
    # refuse it so a gate emptied by a list edit fails every wrapper that
    # scans under it rather than only the list self-test.
    if(NOT _sym_ids AND NOT _str_ids AND NOT _dep_ids)
        message(FATAL_ERROR
            "release-artifact scan: gates '${_p_GATES}' / ids '${_p_IDS}' select no marker in "
            "${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST}")
    endif()

    # Symbols.
    if(_sym_ids)
        if(_p_SYMBOLS)
            set(_symbols "${${_p_SYMBOLS}}")
            set(_have_symbols TRUE)
        else()
            find_program(PULP_RELEASE_ARTIFACT_NM NAMES nm llvm-nm)
            if(PULP_RELEASE_ARTIFACT_NM)
                execute_process(COMMAND "${PULP_RELEASE_ARTIFACT_NM}" -C --defined-only "${_p_ARTIFACT}"
                    OUTPUT_VARIABLE _symbols RESULT_VARIABLE _nm_rc ERROR_VARIABLE _nm_err)
                if(NOT _nm_rc EQUAL 0)
                    message(FATAL_ERROR "release-artifact scan: nm failed on ${_p_ARTIFACT}: ${_nm_err}")
                endif()
                set(_have_symbols TRUE)
            else()
                message(WARNING "release-artifact scan: nm not found; symbol markers unchecked for ${_p_ARTIFACT}")
                set(_have_symbols FALSE)
            endif()
        endif()
        # A stripped binary keeps at most the Mach-O header symbol, so a table
        # with fewer than two defined entries proves nothing; those markers are
        # UNCHECKED and the string backstop every gate carries does the work.
        if(_have_symbols)
            string(REGEX MATCHALL "(^|\n)[0-9a-fA-F]+ [A-Za-z] [^\n]" _defined_lines "${_symbols}")
            list(LENGTH _defined_lines _defined_count)
            if(_defined_count LESS 2)
                set(_have_symbols FALSE)
            endif()
        endif()
        list(LENGTH _sym_ids _n)
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE ${_last})
            list(GET _sym_ids ${_i} _id)
            list(GET _sym_patterns ${_i} _pattern)
            if(NOT _have_symbols)
                list(APPEND _unchecked "${_id} (symbol) ${_pattern}")
                continue()
            endif()
            string(FIND "${_symbols}" "${_pattern}" _idx)
            if(_idx EQUAL -1)
                list(APPEND _absent "${_id} (symbol) ${_pattern}")
            else()
                list(APPEND _present "${_id} (symbol) ${_pattern}")
            endif()
        endforeach()
    endif()

    # Strings. The run floor is the shortest selected pattern, so every
    # pattern lies inside one extracted run (a run shorter than the pattern
    # could not contain it); searching the runs is then the same as searching
    # the raw bytes, without hex-encoding a 40 MB plug-in.
    if(_str_ids)
        if(_p_STRINGS)
            set(_runs "${${_p_STRINGS}}")
        else()
            set(_floor 0)
            foreach(_pattern IN LISTS _str_patterns)
                string(LENGTH "${_pattern}" _len)
                if(_floor EQUAL 0 OR _len LESS _floor)
                    set(_floor ${_len})
                endif()
            endforeach()
            file(STRINGS "${_p_ARTIFACT}" _runs LENGTH_MINIMUM ${_floor})
        endif()
        list(LENGTH _str_ids _n)
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE ${_last})
            list(GET _str_ids ${_i} _id)
            list(GET _str_patterns ${_i} _pattern)
            if(NOT _pattern MATCHES "${PULP_RELEASE_ARTIFACT_STRING_PATTERN_REGEX}")
                message(FATAL_ERROR
                    "release-artifact scan: string marker '${_id}' is not searchable in printable runs: '${_pattern}'")
            endif()
            string(FIND "${_runs}" "${_pattern}" _idx)
            if(_idx EQUAL -1)
                list(APPEND _absent "${_id} (string) ${_pattern}")
            else()
                list(APPEND _present "${_id} (string) ${_pattern}")
            endif()
        endforeach()
    endif()

    # Dependencies.
    if(_dep_ids)
        list(LENGTH _dep_ids _n)
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE ${_last})
            list(GET _dep_ids ${_i} _id)
            list(GET _dep_patterns ${_i} _pattern)
            if(NOT _p_DEPENDENCIES)
                list(APPEND _unchecked "${_id} (dependency) ${_pattern}")
                continue()
            endif()
            string(FIND "${${_p_DEPENDENCIES}}" "${_pattern}" _idx)
            if(_idx EQUAL -1)
                list(APPEND _absent "${_id} (dependency) ${_pattern}")
            else()
                list(APPEND _present "${_id} (dependency) ${_pattern}")
            endif()
        endforeach()
    endif()

    if(_p_OUT_PRESENT)
        set(${_p_OUT_PRESENT} "${_present}" PARENT_SCOPE)
    endif()
    if(_p_OUT_ABSENT)
        set(${_p_OUT_ABSENT} "${_absent}" PARENT_SCOPE)
    endif()
    if(_p_OUT_UNCHECKED)
        set(${_p_OUT_UNCHECKED} "${_unchecked}" PARENT_SCOPE)
    endif()
endfunction()

function(_pulp_release_artifact_format_records records out_var)
    string(REPLACE ";" "\n    " _text "${records}")
    set(${out_var} "    ${_text}" PARENT_SCOPE)
endfunction()

# The forbid verdict: every selected marker must be absent.
function(pulp_release_artifact_forbid)
    cmake_parse_arguments(_f "" "LABEL" "" ${ARGN})
    if(NOT _f_LABEL)
        set(_f_LABEL "release-artifact scan")
    endif()
    cmake_parse_arguments(_fa "" "ARTIFACT;SYMBOLS;STRINGS;DEPENDENCIES" "GATES;IDS"
        ${_f_UNPARSED_ARGUMENTS})
    pulp_release_artifact_probe(${_f_UNPARSED_ARGUMENTS}
        OUT_PRESENT _present OUT_ABSENT _absent OUT_UNCHECKED _unchecked)
    if(_present)
        _pulp_release_artifact_format_records("${_present}" _text)
        message(FATAL_ERROR
            "${_f_LABEL}: forbidden marker(s) present in\n  ${_fa_ARTIFACT}\n"
            "  gates: ${_fa_GATES} ${_fa_IDS}\n${_text}\n"
            "See ${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST} for why each is forbidden.")
    endif()
    list(LENGTH _absent _checked)
    list(LENGTH _unchecked _skipped)
    set(_note "")
    if(_skipped GREATER 0)
        _pulp_release_artifact_format_records("${_unchecked}" _text)
        set(_note "; UNCHECKED (no evidence source):\n${_text}")
    endif()
    # Nothing checked is not a pass: without nm, or on a stripped binary with
    # no string marker selected, the verdict would rest on no evidence at all.
    if(_checked EQUAL 0)
        _pulp_release_artifact_format_records("${_unchecked}" _text)
        message(FATAL_ERROR
            "${_f_LABEL}: no marker could be checked in\n  ${_fa_ARTIFACT}\n"
            "  gates: ${_fa_GATES} ${_fa_IDS}\n${_text}\n"
            "A scan with no evidence source is not a pass; install nm, or scan an unstripped artifact.")
    endif()
    message(STATUS
        "${_f_LABEL}: ${_checked} forbidden marker(s) absent (gates: ${_fa_GATES} ${_fa_IDS}): ${_fa_ARTIFACT}${_note}")
endfunction()

# The positive control: every selected marker must be present, so a marker
# whose source rotted away is caught before the forbid half goes vacuous.
function(pulp_release_artifact_require)
    cmake_parse_arguments(_r "" "LABEL" "" ${ARGN})
    if(NOT _r_LABEL)
        set(_r_LABEL "release-artifact positive control")
    endif()
    cmake_parse_arguments(_ra "" "ARTIFACT;SYMBOLS;STRINGS;DEPENDENCIES" "GATES;IDS"
        ${_r_UNPARSED_ARGUMENTS})
    pulp_release_artifact_probe(${_r_UNPARSED_ARGUMENTS}
        OUT_PRESENT _present OUT_ABSENT _absent OUT_UNCHECKED _unchecked)
    if(_absent OR _unchecked)
        _pulp_release_artifact_format_records("${_absent};${_unchecked}" _text)
        message(FATAL_ERROR
            "${_r_LABEL}: marker(s) MISSING from\n  ${_ra_ARTIFACT}\n"
            "  gates: ${_ra_GATES} ${_ra_IDS}\n${_text}\n"
            "The positive control is broken: the forbid half of this check would pass vacuously.")
    endif()
    list(LENGTH _present _n)
    message(STATUS
        "${_r_LABEL}: ${_n} marker(s) present as expected (gates: ${_ra_GATES} ${_ra_IDS}): ${_ra_ARTIFACT}")
endfunction()

# Which gates hold for the artifacts of the current configure. Each gate keys
# on the variable that actually governs it, read as a CMake variable of the
# same name: PULP_ENABLE_JS and PULP_TRACING (options), PULP_RELOAD_DEV_WATCHER
# and PULP_BRIDGE_EXEC_ENABLED (the compile definitions, when a configure sets
# them tree-wide), with PULP_DEV_BUILD / PULP_DEV_BRIDGE_EXEC as the
# profile-level fallbacks where tools/cmake/PulpProfiles.cmake defines them.
# An explicit definition wins over its fallback, so -DPULP_RELOAD_DEV_WATCHER=1
# under a release profile does not forbid the watcher it just enabled. Only
# gates whose governing variable is defined contribute, so a tree that
# predates a gate's option scans nothing it cannot know. `control-stripped`
# is per-artifact (the control-shipping manifest decides) and never derived.
function(pulp_release_artifact_active_gates out_var)
    set(_gates "")
    if(DEFINED PULP_ENABLE_JS AND NOT PULP_ENABLE_JS)
        list(APPEND _gates js-off)
    endif()
    if(DEFINED PULP_TRACING AND NOT PULP_TRACING)
        list(APPEND _gates tracing-off)
    endif()
    if(DEFINED PULP_RELOAD_DEV_WATCHER)
        if(NOT PULP_RELOAD_DEV_WATCHER)
            list(APPEND _gates reload-ship)
        endif()
    elseif(DEFINED PULP_DEV_BUILD AND NOT PULP_DEV_BUILD)
        list(APPEND _gates reload-ship)
    endif()
    if(DEFINED PULP_DEV_BUILD AND NOT PULP_DEV_BUILD)
        list(APPEND _gates dev-env-off)
    endif()
    if(DEFINED PULP_BRIDGE_EXEC_ENABLED)
        if(NOT PULP_BRIDGE_EXEC_ENABLED)
            list(APPEND _gates bridge-exec-off)
        endif()
    elseif(DEFINED PULP_DEV_BRIDGE_EXEC AND NOT PULP_DEV_BRIDGE_EXEC)
        list(APPEND _gates bridge-exec-off)
    endif()
    set(${out_var} "${_gates}" PARENT_SCOPE)
endfunction()

# Writes a C++ translation unit that carries every `symbol` and `string`
# marker of the loaded list (optionally only those of GATES), each retained
# past dead-strip. Built as the negative-control fixture: a scan that does not
# flag every one of them is a scanner that could pass vacuously. `dependency`
# markers are loader-table entries a translation unit cannot fake; the
# negative-control test feeds the probe a synthetic loader table for those.
function(pulp_release_artifact_write_fixture out_path)
    cmake_parse_arguments(_w "" "" "GATES" ${ARGN})
    set(_gates "${_w_GATES}")
    if("${_gates}" STREQUAL "")
        set(_gates "${PULP_RELEASE_ARTIFACT_GATE_IDS}")
    endif()
    set(_src "// Generated by PulpReleaseArtifactScan.cmake from\n")
    string(APPEND _src "// ${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST}\n")
    string(APPEND _src "// Negative-control fixture: carries every symbol and string marker so the\n")
    string(APPEND _src "// scanner is proven to flag each one. Never link this into anything shipped.\n")
    string(APPEND _src "#if defined(_MSC_VER)\n#define PULP_PRA_USED __declspec(dllexport)\n")
    string(APPEND _src "#else\n#define PULP_PRA_USED __attribute__((used, visibility(\"default\")))\n#endif\n\n")
    pulp_release_artifact_select(OUT_IDS _str_ids OUT_PATTERNS _str_patterns KIND string GATES ${_gates})
    set(_n 0)
    foreach(_pattern IN LISTS _str_patterns)
        string(APPEND _src "extern \"C\" PULP_PRA_USED const volatile char pulp_pra_string_${_n}[] = \"${_pattern}\";\n")
        math(EXPR _n "${_n} + 1")
    endforeach()
    pulp_release_artifact_select(OUT_IDS _sym_ids OUT_PATTERNS _sym_patterns KIND symbol GATES ${_gates})
    set(_n 0)
    foreach(_pattern IN LISTS _sym_patterns)
        if(NOT _pattern MATCHES "${PULP_RELEASE_ARTIFACT_SYMBOL_PATTERN_REGEX}")
            message(FATAL_ERROR "release-artifact fixture: symbol pattern is not emittable: '${_pattern}'")
        endif()
        # `a::b::C::` names C's members: emit a member of C, whose demangled
        # name `a::b::C::pulp_pra_member` contains the pattern.
        string(REGEX REPLACE "::$" "" _pattern_path "${_pattern}")
        string(REPLACE "::" ";" _parts "${_pattern_path}")
        list(POP_BACK _parts _leaf)
        if(_parts)
            # a::b::Leaf -> namespace a { namespace b { struct Leaf { static int m; }; } }
            set(_open "")
            set(_close "")
            foreach(_ns IN LISTS _parts)
                string(APPEND _open "namespace ${_ns} { ")
                string(APPEND _close "} ")
            endforeach()
            string(APPEND _src "${_open}struct ${_leaf} { PULP_PRA_USED static int pulp_pra_member; }; "
                "int ${_leaf}::pulp_pra_member = ${_n}; ${_close}\n")
        else()
            string(APPEND _src "extern \"C\" PULP_PRA_USED int ${_pattern_path}_pulp_pra_${_n} = ${_n};\n")
        endif()
        math(EXPR _n "${_n} + 1")
    endforeach()
    string(APPEND _src "\nint main() { return 0; }\n")
    file(CONFIGURE OUTPUT "${out_path}" CONTENT "${_src}" @ONLY)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    # BIN names one artifact; BINS a list (one class scan over every SDK
    # library). Every artifact is scanned with the same gates and the first
    # hit is fatal.
    if(DEFINED BINS AND NOT "${BINS}" STREQUAL "")
        set(_script_bins ${BINS})
    elseif(DEFINED BIN)
        set(_script_bins "${BIN}")
    else()
        message(FATAL_ERROR "PulpReleaseArtifactScan: -DBIN=<artifact> or -DBINS=<artifact;...> is required")
    endif()
    if(NOT DEFINED MODE)
        set(MODE forbid)
    endif()
    if(NOT MODE MATCHES "^(forbid|require)$")
        message(FATAL_ERROR "PulpReleaseArtifactScan: MODE must be forbid or require, not '${MODE}'")
    endif()
    if(DEFINED LIST)
        pulp_release_artifact_load("${LIST}")
    else()
        pulp_release_artifact_load()
    endif()
    set(_script_common "")
    if(DEFINED GATES AND NOT "${GATES}" STREQUAL "")
        list(APPEND _script_common GATES ${GATES})
    endif()
    if(DEFINED IDS AND NOT "${IDS}" STREQUAL "")
        list(APPEND _script_common IDS ${IDS})
    endif()
    if(DEFINED LABEL)
        list(APPEND _script_common LABEL "${LABEL}")
    endif()
    if(APPLE)
        find_program(_pra_otool NAMES otool)
    elseif(UNIX)
        find_program(_pra_readelf NAMES readelf llvm-readelf)
    endif()
    foreach(_script_bin IN LISTS _script_bins)
        set(_script_args ARTIFACT "${_script_bin}" ${_script_common})
        # The loader table, when a native scanner for it exists. A static
        # archive has none; otool/readelf exit non-zero on it, which is a
        # skip (dependency markers reported UNCHECKED), not a fail.
        set(_deps "")
        if(_pra_otool)
            execute_process(COMMAND "${_pra_otool}" -L "${_script_bin}" OUTPUT_VARIABLE _deps
                RESULT_VARIABLE _rc ERROR_QUIET)
            if(NOT _rc EQUAL 0)
                set(_deps "")
            endif()
        elseif(_pra_readelf)
            execute_process(COMMAND "${_pra_readelf}" -d "${_script_bin}" OUTPUT_VARIABLE _deps
                RESULT_VARIABLE _rc ERROR_QUIET)
            if(NOT _rc EQUAL 0)
                set(_deps "")
            endif()
        endif()
        if(NOT "${_deps}" STREQUAL "")
            list(APPEND _script_args DEPENDENCIES _deps)
        endif()
        if(MODE STREQUAL "forbid")
            pulp_release_artifact_forbid(${_script_args})
        else()
            pulp_release_artifact_require(${_script_args})
        endif()
    endforeach()
endif()
