# Pulp UI and kit-asset helpers.
# Included by PulpUtils.cmake to preserve its public compatibility surface.

function(_pulp_normalize_ui_script_path out_var source_dir ui_script)
    if(NOT ui_script)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    if(IS_ABSOLUTE "${ui_script}")
        set(_ui_script_path "${ui_script}")
    else()
        set(_ui_script_path "${source_dir}/${ui_script}")
    endif()

    get_filename_component(_ui_script_path "${_ui_script_path}" ABSOLUTE)
    file(TO_CMAKE_PATH "${_ui_script_path}" _ui_script_path)
    set(${out_var} "${_ui_script_path}" PARENT_SCOPE)
endfunction()

function(_pulp_apply_ui_script_definition target ui_script_path)
    if(NOT ui_script_path)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_SCRIPT_PATH="${ui_script_path}"
    )
endfunction()

function(_pulp_apply_ui_theme_definition target theme_path)
    if(NOT theme_path)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_THEME_PATH="${theme_path}"
    )
endfunction()

function(_pulp_apply_ui_asset_roots_definition target asset_roots)
    if(NOT asset_roots)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_ASSET_ROOTS="${asset_roots}"
    )
endfunction()

function(_pulp_select_kit_export out_var helper_name kit_target property explicit_value label)
    get_target_property(_values "${kit_target}" "${property}")
    if(NOT _values)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    set(_selected "")
    if(explicit_value)
        foreach(_value IN LISTS _values)
            if(_value STREQUAL explicit_value)
                set(_selected "${_value}")
            endif()
        endforeach()
        if(NOT _selected)
            message(FATAL_ERROR
                "${helper_name}: ${label} '${explicit_value}' is not exported by the kit")
        endif()
    else()
        list(LENGTH _values _value_count)
        if(_value_count EQUAL 1)
            list(GET _values 0 _selected)
        elseif(_value_count GREATER 1)
            message(FATAL_ERROR
                "${helper_name}: kit exports multiple ${label} entries; pass ${label} <path>")
        endif()
    endif()

    set(${out_var} "${_selected}" PARENT_SCOPE)
endfunction()

function(_pulp_absolute_project_path out_var path)
    if(NOT path)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    if(IS_ABSOLUTE "${path}")
        set(_abs "${path}")
    else()
        set(_abs "${CMAKE_SOURCE_DIR}/${path}")
    endif()
    get_filename_component(_abs "${_abs}" ABSOLUTE)
    file(TO_CMAKE_PATH "${_abs}" _abs)
    set(${out_var} "${_abs}" PARENT_SCOPE)
endfunction()

function(_pulp_absolute_project_paths out_var)
    set(_out)
    foreach(_path IN LISTS ARGN)
        _pulp_absolute_project_path(_abs "${_path}")
        if(_abs)
            list(APPEND _out "${_abs}")
        endif()
    endforeach()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

function(_pulp_pipe_join out_var)
    set(_joined "")
    foreach(_value IN LISTS ARGN)
        if(_value MATCHES "\\|")
            message(FATAL_ERROR
                "pulp_use_kit_ui: asset root paths must not contain '|': ${_value}")
        endif()
        if(_joined)
            string(APPEND _joined "|")
        endif()
        string(APPEND _joined "${_value}")
    endforeach()
    set(${out_var} "${_joined}" PARENT_SCOPE)
endfunction()

function(pulp_use_kit_ui target kit_target)
    set(options)
    set(oneValueArgs SCRIPT TOKENS)
    set(multiValueArgs)
    cmake_parse_arguments(KIT_UI "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(KIT_UI_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ${kit_target}): unknown arguments: ${KIT_UI_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT TARGET "${target}_Core")
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ...): call this after pulp_add_plugin(${target} ...)")
    endif()
    if(NOT TARGET "${kit_target}")
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ${kit_target}): kit target does not exist. "
            "Apply the kit and include(cmake/pulp-kits.cmake OPTIONAL) first.")
    endif()

    _pulp_select_kit_export(_selected_script
        "pulp_use_kit_ui(${target} ${kit_target})"
        "${kit_target}" PULP_UI_SCRIPTS "${KIT_UI_SCRIPT}" "SCRIPT")
    if(NOT _selected_script)
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ${kit_target}): kit exports no PULP_UI_SCRIPTS")
    endif()

    _pulp_select_kit_export(_selected_tokens
        "pulp_use_kit_ui(${target} ${kit_target})"
        "${kit_target}" PULP_DESIGN_TOKENS "${KIT_UI_TOKENS}" "TOKENS")
    get_target_property(_selected_assets "${kit_target}" PULP_ASSETS)
    if(NOT _selected_assets)
        set(_selected_assets)
    endif()

    _pulp_absolute_project_path(_selected_script_abs "${_selected_script}")
    _pulp_absolute_project_path(_selected_tokens_abs "${_selected_tokens}")
    _pulp_absolute_project_paths(_selected_assets_abs ${_selected_assets})
    _pulp_pipe_join(_selected_assets_joined ${_selected_assets_abs})

    if(NOT EXISTS "${_selected_script_abs}")
        message(WARNING
            "pulp_use_kit_ui(${target} ${kit_target}): selected UI script is missing: "
            "${_selected_script_abs}")
    endif()
    if(_selected_tokens_abs AND NOT EXISTS "${_selected_tokens_abs}")
        message(WARNING
            "pulp_use_kit_ui(${target} ${kit_target}): selected design token file is missing: "
            "${_selected_tokens_abs}")
    endif()

    set(PULP_${target}_UI_SCRIPT "${_selected_script_abs}" CACHE INTERNAL "")
    if(_selected_tokens_abs)
        set(PULP_${target}_UI_THEME "${_selected_tokens_abs}" CACHE INTERNAL "")
    endif()
    if(_selected_assets_joined)
        set(PULP_${target}_UI_ASSET_ROOTS "${_selected_assets_joined}" CACHE INTERNAL "")
    endif()
    set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_TARGET "${kit_target}")
    set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_SCRIPT "${_selected_script}")
    if(_selected_tokens)
        set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_TOKENS "${_selected_tokens}")
    endif()
    if(_selected_assets)
        set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_ASSETS "${_selected_assets}")
    endif()

    set(_applied_targets)
    foreach(_fmt VST3 CLAP AU LV2 AAX AUv3 Standalone)
        if(TARGET "${target}_${_fmt}")
            _pulp_apply_ui_script_definition("${target}_${_fmt}" "${_selected_script_abs}")
            _pulp_apply_ui_theme_definition("${target}_${_fmt}" "${_selected_tokens_abs}")
            _pulp_apply_ui_asset_roots_definition("${target}_${_fmt}" "${_selected_assets_joined}")
            list(APPEND _applied_targets "${target}_${_fmt}")
        endif()
    endforeach()

    if(NOT _applied_targets)
        message(WARNING
            "pulp_use_kit_ui(${target} ${kit_target}): no existing format targets were found; "
            "call this after pulp_add_plugin has created at least one format target")
    endif()
endfunction()
