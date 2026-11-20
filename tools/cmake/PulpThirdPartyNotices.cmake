# PulpThirdPartyNotices.cmake — one combined third-party notices file per bundle.
#
# Every SDK a plug-in links can describe its third-party code in a notices
# manifest (schema pulp.notices.v1): one entry per dependency with its name,
# version, SPDX license id, copyright, source URL, a license text file, the
# CMake targets whose presence in a link closure means the code ships
# (`required_by`), and an optional `feature` variable. Pulp generates its own from
# tools/deps/manifest.json and NOTICE.md (tools/deps/notices.py); another SDK
# ships a notices.json and registers it with pulp_add_notice_manifest(), or
# installs it at share/<namespace>/notices/notices.json beside its package
# config.
#
# pulp_add_plugin() writes ThirdPartyNotices.json and ThirdPartyNotices.txt into
# every bundle it builds, next to pulp-build-info.json. The entries are those
# whose `required_by` meets that format target's link closure. The closure is
# measured at the end of the directory that called pulp_add_plugin(), so links
# added there after it returns are counted; a link added to the plug-in from
# another directory afterwards is not.
#
# Matching and its limits:
#   - `Pulp::x`, `pulp::x`, `pulp-x` and `x` name the same Pulp target, so one
#     entry serves the source tree and an installed SDK.
#   - A `feature` variable that is defined and false drops the entry; an
#     undefined one counts as on, which over-reports rather than omits.
#   - The closure is per target, not per symbol: a header-only edge (the format
#     SDK headers on pulp-format-core) names its dependency even when no code
#     from it is compiled in.
#   - Duplicates across manifests are merged on (name, version), first wins.
#   - A namespaced target (`Vellum::x`) that no manifest claims is reported under
#     `sdks_without_manifest`; any other unclaimed non-Pulp target under
#     `unattributed_targets`. Neither fails the build.

include_guard(GLOBAL)


# Register an SDK's notices manifest for every plug-in in this configure.
function(pulp_add_notice_manifest manifest)
    get_filename_component(_abs "${manifest}" ABSOLUTE)
    if(NOT EXISTS "${_abs}")
        message(WARNING "pulp_add_notice_manifest: ${_abs} does not exist")
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY PULP_NOTICE_MANIFESTS "${_abs}")
endfunction()

# Generate Pulp's own manifest into the build tree and install it with the SDK.
# Needs Python 3; without it plug-ins still get a notices file, which then lists
# Pulp under `sdks_without_manifest`.
function(pulp_generate_sdk_notices)
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../..")
    set(_out "${CMAKE_BINARY_DIR}/share/pulp/notices")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_root}/tools/deps/manifest.json" "${_root}/NOTICE.md" "${_root}/tools/deps/notices.py")
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter QUIET)
    endif()
    if(NOT Python3_EXECUTABLE)
        message(WARNING "Pulp: no Python 3, so plug-ins carry no Pulp notices manifest")
        return()
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_root}/tools/deps/notices.py"
            --out "${_out}" --sdk-version "${PROJECT_VERSION}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tools/deps/notices.py failed:\n${_stderr}")
    endif()
    set_property(GLOBAL PROPERTY PULP_SDK_NOTICE_MANIFEST "${_out}/notices.json")
    install(DIRECTORY "${_out}/" DESTINATION "share/pulp/notices" OPTIONAL)
endfunction()

# JSON string literal for an arbitrary text value.
function(_pulp_notices_json_string out_var value)
    string(REPLACE "\\" "\\\\" _s "${value}")
    string(REPLACE "\"" "\\\"" _s "${_s}")
    string(REPLACE "\n" "\\n" _s "${_s}")
    string(REPLACE "\r" "\\r" _s "${_s}")
    string(REPLACE "\t" "\\t" _s "${_s}")
    string(ASCII 12 _ff)
    string(REPLACE "${_ff}" "\\f" _s "${_s}")
    set(${out_var} "\"${_s}\"" PARENT_SCOPE)
endfunction()

function(_pulp_notices_json_array out_var)
    set(_json "[")
    set(_sep "")
    foreach(_v IN LISTS ARGN)
        _pulp_notices_json_string(_q "${_v}")
        string(APPEND _json "${_sep}${_q}")
        set(_sep ", ")
    endforeach()
    set(${out_var} "${_json}]" PARENT_SCOPE)
endfunction()

# Write `content` to `path` only when it differs, so an unchanged notice set
# does not relink the bundle that lists it in LINK_DEPENDS.
function(_pulp_notices_write_if_changed path content)
    if(EXISTS "${path}")
        file(READ "${path}" _old)
        if(_old STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

# Where a sidecar file goes for one bundle-producing target: the same place as
# pulp-build-info.json (see _pulp_attach_build_info).
function(_pulp_notices_output out_var format_target format name file_name)
    get_target_property(_is_bundle ${format_target} BUNDLE)
    get_target_property(_is_app_bundle ${format_target} MACOSX_BUNDLE)
    if("${format_target}" MATCHES "_LV2$")
        set(_output "$<TARGET_FILE_DIR:${format_target}>/${file_name}")
    elseif(WIN32 AND "${format}" STREQUAL "VST3")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpWindowsTargetArch.cmake")
        pulp_windows_target_arch(_arch)
        set(_output
            "${CMAKE_BINARY_DIR}/VST3/${name}.vst3/Contents/${_arch}/$<TARGET_FILE_BASE_NAME:${format_target}>.${file_name}")
    elseif(APPLE AND (_is_bundle OR _is_app_bundle))
        _pulp_build_info_apple_resource_subdir("${CMAKE_SYSTEM_NAME}" _sub)
        if(_sub)
            set(_sub "/${_sub}")
        endif()
        set(_output "$<TARGET_BUNDLE_CONTENT_DIR:${format_target}>${_sub}/${file_name}")
    else()
        set(_output
            "$<TARGET_FILE_DIR:${format_target}>/$<TARGET_FILE_BASE_NAME:${format_target}>.${file_name}")
    endif()
    set(${out_var} "${_output}" PARENT_SCOPE)
endfunction()

# Attach ThirdPartyNotices.{json,txt} to every bundle pulp_add_plugin(target)
# produced. The files are computed at the end of the configure and copied into
# the bundle after each link.
function(_pulp_attach_third_party_notices target name)
    foreach(_format VST3 CLAP AU LV2 AAX AUv3 AUv3Host Standalone)
        set(_format_target ${target}_${_format})
        if(NOT TARGET ${_format_target})
            continue()
        endif()
        get_target_property(_is_framework ${_format_target} FRAMEWORK)
        if(_is_framework)
            continue()
        endif()
        set(_dir "${CMAKE_BINARY_DIR}/pulp-notices/${_format_target}")
        set_target_properties(${_format_target} PROPERTIES
            PULP_NOTICES_DIR "${_dir}"
            PULP_NOTICES_PRODUCT "${name}"
            PULP_NOTICES_BASE "${target}"
            PULP_NOTICES_FORMAT "${_format}")
        # Exist before the first build even if the deferred write is skipped.
        foreach(_ext json txt)
            if(NOT EXISTS "${_dir}/ThirdPartyNotices.${_ext}")
                file(WRITE "${_dir}/ThirdPartyNotices.${_ext}" "")
            endif()
        endforeach()
        set_property(TARGET ${_format_target} APPEND PROPERTY LINK_DEPENDS
            "${_dir}/ThirdPartyNotices.json" "${_dir}/ThirdPartyNotices.txt")
        _pulp_notices_output(_json_out ${_format_target} ${_format} "${name}" ThirdPartyNotices.json)
        _pulp_notices_output(_txt_out ${_format_target} ${_format} "${name}" ThirdPartyNotices.txt)
        add_custom_command(TARGET ${_format_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_dir}/ThirdPartyNotices.json" "${_json_out}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_dir}/ThirdPartyNotices.txt" "${_txt_out}"
            VERBATIM)
        # Deferred to the end of the calling directory, not the top level:
        # IMPORTED targets (an SDK's find_package() result) are visible only in
        # the directory that created them and below. Arguments of a deferred
        # call expand when it runs, so bake the target name in now.
        cmake_language(EVAL CODE
            "cmake_language(DEFER CALL _pulp_write_third_party_notices [[${_format_target}]])")
    endforeach()
endfunction()

# Every manifest known to this configure: Pulp's, registered ones, and one per
# namespace found in `closure` that has a <Ns>_DIR package config beside it.
function(_pulp_notices_manifests out_var closure)
    get_property(_sdk GLOBAL PROPERTY PULP_SDK_NOTICE_MANIFEST)
    get_property(_registered GLOBAL PROPERTY PULP_NOTICE_MANIFESTS)
    set(_manifests "")
    foreach(_m IN LISTS _sdk _registered)
        if(_m AND EXISTS "${_m}" AND NOT _m IN_LIST _manifests)
            list(APPEND _manifests "${_m}")
        endif()
    endforeach()
    foreach(_t IN LISTS closure)
        if(NOT _t MATCHES "^([A-Za-z0-9_]+)::")
            continue()
        endif()
        set(_ns "${CMAKE_MATCH_1}")
        if(NOT DEFINED ${_ns}_DIR)
            continue()
        endif()
        string(TOLOWER "${_ns}" _lower)
        foreach(_candidate
                "${${_ns}_DIR}/notices.json"
                "${${_ns}_DIR}/../../../share/${_lower}/notices/notices.json")
            get_filename_component(_candidate "${_candidate}" ABSOLUTE)
            if(EXISTS "${_candidate}" AND NOT _candidate IN_LIST _manifests)
                list(APPEND _manifests "${_candidate}")
            endif()
        endforeach()
    endforeach()
    set(${out_var} "${_manifests}" PARENT_SCOPE)
endfunction()

# Deferred: compute and write one format target's notices.
function(_pulp_write_third_party_notices format_target)
    get_target_property(_dir ${format_target} PULP_NOTICES_DIR)
    get_target_property(_product ${format_target} PULP_NOTICES_PRODUCT)
    get_target_property(_format ${format_target} PULP_NOTICES_FORMAT)
    get_target_property(_base ${format_target} PULP_NOTICES_BASE)

    if(COMMAND pulp_link_closure)
        pulp_link_closure(${format_target} TARGETS_OUT _closure)
    else()
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpConsumerClosure.cmake")
        pulp_consumer_link_closure(_closure ${format_target})
    endif()

    # Every spelling a manifest might use for a closure target.
    set(_names "")
    foreach(_t IN LISTS _closure)
        list(APPEND _names "${_t}")
        if(_t MATCHES "^(Pulp|pulp)::(.+)$")
            list(APPEND _names "${CMAKE_MATCH_2}" "pulp-${CMAKE_MATCH_2}")
        endif()
    endforeach()

    _pulp_notices_manifests(_manifests "${_closure}")

    set(_keys "")
    set(_entries_json "")
    set(_text "")
    set(_sdks_json "")
    set(_claimed_namespaces "")
    set(_claimed_targets "")
    set(_have_pulp OFF)
    set(_sep "")
    set(_sdk_sep "")
    foreach(_manifest IN LISTS _manifests)
        file(READ "${_manifest}" _doc)
        get_filename_component(_manifest_dir "${_manifest}" DIRECTORY)
        string(JSON _sdk ERROR_VARIABLE _err GET "${_doc}" sdk)
        if(_err)
            message(WARNING "${_manifest}: not a notices manifest (${_err})")
            continue()
        endif()
        string(JSON _sdk_version ERROR_VARIABLE _err GET "${_doc}" sdk_version)
        if(_err)
            set(_sdk_version "")
        endif()
        if(_sdk STREQUAL "Pulp")
            set(_have_pulp ON)
        endif()
        _pulp_notices_json_string(_j_sdk "${_sdk}")
        _pulp_notices_json_string(_j_sdk_version "${_sdk_version}")
        set(_sdk_used OFF)

        string(JSON _ns_count ERROR_VARIABLE _err LENGTH "${_doc}" namespaces)
        if(NOT _err AND _ns_count GREATER 0)
            math(EXPR _last "${_ns_count} - 1")
            foreach(_i RANGE ${_last})
                string(JSON _ns GET "${_doc}" namespaces ${_i})
                list(APPEND _claimed_namespaces "${_ns}")
            endforeach()
        endif()

        string(JSON _count LENGTH "${_doc}" entries)
        if(_count EQUAL 0)
            continue()
        endif()
        math(EXPR _last "${_count} - 1")
        foreach(_i RANGE ${_last})
            string(JSON _entry GET "${_doc}" entries ${_i})
            string(JSON _name GET "${_entry}" name)
            foreach(_field version license copyright source license_text feature)
                string(JSON _${_field} ERROR_VARIABLE _err GET "${_entry}" ${_field})
                if(_err)
                    set(_${_field} "")
                endif()
            endforeach()
            # Targets named by any entry count as claimed whether or not the
            # entry is enabled, so a feature-gated dependency is not also
            # reported as unattributed.
            set(_hits "")
            string(JSON _req_count ERROR_VARIABLE _err LENGTH "${_entry}" required_by)
            if(_err OR _req_count EQUAL 0)
                continue()
            endif()
            math(EXPR _req_last "${_req_count} - 1")
            foreach(_r RANGE ${_req_last})
                string(JSON _req GET "${_entry}" required_by ${_r})
                list(APPEND _claimed_targets "${_req}")
                if(_req MATCHES "^([A-Za-z0-9_]+)::")
                    list(APPEND _claimed_namespaces "${CMAKE_MATCH_1}")
                endif()
                if(_req IN_LIST _names)
                    list(APPEND _hits "${_req}")
                endif()
            endforeach()
            if(NOT _hits)
                continue()
            endif()
            if(NOT _feature STREQUAL "")
                if(DEFINED ${_feature} AND NOT ${_feature})
                    continue()
                endif()
            endif()
            string(TOLOWER "${_name}@${_version}" _key)
            if(_key IN_LIST _keys)
                continue()
            endif()
            list(APPEND _keys "${_key}")

            set(_body "")
            if(_license_text)
                if(IS_ABSOLUTE "${_license_text}")
                    set(_text_path "${_license_text}")
                else()
                    set(_text_path "${_manifest_dir}/${_license_text}")
                endif()
                if(EXISTS "${_text_path}")
                    file(READ "${_text_path}" _body)
                else()
                    message(WARNING "${_manifest}: ${_name}: missing license text ${_text_path}")
                endif()
            endif()

            _pulp_notices_json_string(_j_name "${_name}")
            _pulp_notices_json_string(_j_version "${_version}")
            _pulp_notices_json_string(_j_license "${_license}")
            _pulp_notices_json_string(_j_copyright "${_copyright}")
            _pulp_notices_json_string(_j_source "${_source}")
            _pulp_notices_json_string(_j_body "${_body}")
            _pulp_notices_json_array(_j_hits ${_hits})
            string(APPEND _entries_json "${_sep}\n    {\"name\": ${_j_name}, \"version\": ${_j_version}, "
                "\"license\": ${_j_license}, \"copyright\": ${_j_copyright}, \"source\": ${_j_source}, "
                "\"sdk\": ${_j_sdk}, \"required_by\": ${_j_hits}, \"license_text\": ${_j_body}}")
            set(_sep ",")
            set(_sdk_used ON)

            string(APPEND _text "================================================================\n")
            string(APPEND _text "${_name} ${_version} (${_license})\n")
            if(_source)
                string(APPEND _text "${_source}\n")
            endif()
            string(APPEND _text "\n${_body}\n")
        endforeach()
        # An SDK is listed when it contributed a notice, or is Pulp itself.
        if(_sdk_used OR _sdk STREQUAL "Pulp")
            string(APPEND _sdks_json
                "${_sdk_sep}\n    {\"name\": ${_j_sdk}, \"version\": ${_j_sdk_version}}")
            set(_sdk_sep ",")
        endif()
    endforeach()

    # What no manifest covers.
    set(_missing_sdks "")
    if(NOT _have_pulp)
        list(APPEND _missing_sdks Pulp)
    endif()
    set(_unattributed "")
    foreach(_t IN LISTS _closure)
        string(FIND "${_t}" "${_base}_" _own_prefix)
        if(_own_prefix EQUAL 0 OR _t IN_LIST _claimed_targets)
            continue()
        endif()
        if(_t MATCHES "^([A-Za-z0-9_]+)::")
            set(_ns "${CMAKE_MATCH_1}")
            if(NOT _ns IN_LIST _claimed_namespaces AND NOT _ns IN_LIST _missing_sdks)
                list(APPEND _missing_sdks "${_ns}")
            endif()
            if(_ns MATCHES "^(Pulp|pulp)$" OR NOT _ns IN_LIST _claimed_namespaces)
                continue()
            endif()
        elseif(_t MATCHES "^pulp-")
            continue()
        endif()
        list(APPEND _unattributed "${_t}")
    endforeach()

    _pulp_notices_json_string(_j_product "${_product}")
    _pulp_notices_json_string(_j_target "${format_target}")
    _pulp_notices_json_string(_j_format "${_format}")
    _pulp_notices_json_array(_j_missing ${_missing_sdks})
    _pulp_notices_json_array(_j_unattributed ${_unattributed})
    if(_entries_json)
        string(APPEND _entries_json "\n  ")
    endif()
    if(_sdks_json)
        string(APPEND _sdks_json "\n  ")
    endif()
    set(_json "{\n")
    string(APPEND _json "  \"schema\": \"pulp.third-party-notices.v1\",\n")
    string(APPEND _json "  \"product\": ${_j_product},\n")
    string(APPEND _json "  \"target\": ${_j_target},\n")
    string(APPEND _json "  \"format\": ${_j_format},\n")
    string(APPEND _json "  \"sdks\": [${_sdks_json}],\n")
    string(APPEND _json "  \"sdks_without_manifest\": ${_j_missing},\n")
    string(APPEND _json "  \"unattributed_targets\": ${_j_unattributed},\n")
    string(APPEND _json "  \"notices\": [${_entries_json}]\n")
    string(APPEND _json "}\n")

    set(_header "Third-party notices for ${_product} (${_format})\n\n")
    string(APPEND _header "This software includes the third-party components listed below.\n")
    if(_missing_sdks)
        list(JOIN _missing_sdks ", " _missing_display)
        string(APPEND _header "No notices manifest was available for: ${_missing_display}.\n")
    endif()
    string(APPEND _header "\n")

    _pulp_notices_write_if_changed("${_dir}/ThirdPartyNotices.json" "${_json}")
    _pulp_notices_write_if_changed("${_dir}/ThirdPartyNotices.txt" "${_header}${_text}")
endfunction()
