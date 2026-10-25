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

# Whether a build also records the absolute on-disk UI_SCRIPT (and theme and
# asset-root) paths. Only a dev build (PULP_DEV_BUILD, which the root
# CMakeLists defaults from PULP_BUILD_TESTS) does: a hot-reloading editor (the
# standalone host, or a DAW launched with PULP_DEV_HOT_RELOAD=1) reads that
# file while it exists so an edit shows without a rebuild. Every build embeds
# the script, its theme and the files beside it, and every other open evaluates
# the embedded bytes, so a shipped image never names, or depends on, the
# developer's source tree.
# Declared here as well as in the root CMakeLists so an installed-SDK consumer
# has the option: there it defaults OFF (embed only) and the `pulp` CLI's
# configure passes -DPULP_DEV_BUILD=ON for the developer loop. The root
# CMakeLists declares it first for the in-tree checkout, so this is a no-op there.
if(NOT DEFINED PULP_DEV_BUILD)
    option(PULP_DEV_BUILD
        "Record development-only paths (the on-disk ui.js hot-reload override)" OFF)
endif()

function(_pulp_ui_script_dev_path_enabled out_var)
    if(PULP_DEV_BUILD)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# File types embedded from the directory beside UI_SCRIPT (and from a kit's
# declared asset directories): the images, fonts, data sidecars and shaders a
# scripted editor fetches, draws, or registers relative to itself. Source,
# markup and prose beside the script are left out; a UI_SCRIPT must sit in its
# own directory (one holding a CMakeLists.txt is refused), so a project's own
# files are never swept into every format binary.
set(_PULP_UI_EMBED_ASSET_EXTENSIONS
    png jpg jpeg gif webp bmp ico svg
    ttf otf woff woff2
    json
    wgsl glsl sksl frag vert)
# Embedding more than this many bytes beside one script is worth a look.
set(_PULP_UI_EMBED_SIZE_WARNING_BYTES 16777216)

# The files beside `script_path` worth embedding with it, as `relative|absolute`
# pairs. Walks the script's directory; skips the script and theme themselves,
# dot-directories, node_modules, this build tree, and any directory that is
# itself a CMake build tree (holds a CMakeCache.txt). The glob is a configure
# dependency, so a file added later re-runs CMake on the next build.
function(_pulp_ui_embed_sibling_files out_var script_path theme_path)
    get_filename_component(_dir "${script_path}" DIRECTORY)
    if(EXISTS "${_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "UI_SCRIPT ${script_path}: the script sits in a directory that holds a "
            "CMakeLists.txt. Every image, font and data file beside the script is "
            "embedded into every format target, so a project directory would ship its "
            "own files inside the plug-in. Move the script into its own directory "
            "(for example ui/) and point UI_SCRIPT at it.")
    endif()
    _pulp_ui_embed_files_under(_pairs "${_dir}" "${script_path};${theme_path}")
    set(${out_var} "${_pairs}" PARENT_SCOPE)
endfunction()

function(_pulp_ui_embed_files_under out_var root exclude_paths)
    set(_patterns)
    foreach(_ext IN LISTS _PULP_UI_EMBED_ASSET_EXTENSIONS)
        list(APPEND _patterns "${root}/*.${_ext}")
    endforeach()
    file(GLOB_RECURSE _found LIST_DIRECTORIES false RELATIVE "${root}" CONFIGURE_DEPENDS
        ${_patterns})
    list(SORT _found)
    set(_pairs)
    foreach(_rel IN LISTS _found)
        set(_abs "${root}/${_rel}")
        if(_abs IN_LIST exclude_paths)
            continue()
        endif()
        if(_rel MATCHES "(^|/)(\\.[^/]*|node_modules)(/|$)")
            continue()
        endif()
        string(FIND "${_abs}" "${CMAKE_BINARY_DIR}/" _in_build)
        if(_in_build EQUAL 0)
            continue()
        endif()
        # A build tree inside the script's directory is not the editor's.
        set(_skip FALSE)
        set(_prefix "${root}")
        string(REPLACE "/" ";" _segments "${_rel}")
        list(POP_BACK _segments)
        foreach(_segment IN LISTS _segments)
            string(APPEND _prefix "/${_segment}")
            if(EXISTS "${_prefix}/CMakeCache.txt")
                set(_skip TRUE)
                break()
            endif()
        endforeach()
        if(_skip)
            continue()
        endif()
        list(APPEND _pairs "${_rel}|${_abs}")
    endforeach()
    set(${out_var} "${_pairs}" PARENT_SCOPE)
endfunction()

# One static library per distinct (script, theme, files): every file encoded
# as a C array by tools/cmake/scripts/encode_binary_data.py at build time
# (the array regenerates when the file changes, without a reconfigure), plus a
# header that declares the arrays and returns them as a
# pulp::format::EditorAssets record. The target's generated registration unit
# includes that header, so the arrays are referenced from a direct source and
# survive the static link.
function(_pulp_ui_script_embed_target out_lib out_header script_path theme_path file_pairs)
    string(SHA1 _key "${script_path}|${theme_path}|${file_pairs}")
    string(SUBSTRING "${_key}" 0 12 _key)
    set(_lib "pulp_ui_embed_${_key}")
    get_property(_header GLOBAL PROPERTY "PULP_UI_EMBED_${_lib}_HEADER")
    if(NOT TARGET "${_lib}")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(_encoder "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py")
        if(NOT EXISTS "${_encoder}")
            message(FATAL_ERROR
                "UI_SCRIPT embedding: encode_binary_data.py not found at ${_encoder}. "
                "An installed SDK places it beside PulpUiAssets.cmake under "
                "lib/cmake/Pulp/scripts/; re-install Pulp.")
        endif()
        set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp_ui_embed/${_lib}")
        file(MAKE_DIRECTORY "${_gen_dir}")
        get_filename_component(_script_name "${script_path}" NAME)

        # `relative|absolute` for every embedded file, the script and theme first.
        set(_all_pairs "${_script_name}|${script_path}")
        if(theme_path)
            get_filename_component(_theme_name "${theme_path}" NAME)
            list(APPEND _all_pairs "${_theme_name}|${theme_path}")
        endif()
        list(APPEND _all_pairs ${file_pairs})

        set(_cpps)
        set(_vars)
        set(_decls "")
        set(_file_entries "")
        set(_total_bytes 0)
        set(_index 0)
        foreach(_pair IN LISTS _all_pairs)
            string(REPLACE "|" ";" _parts "${_pair}")
            list(GET _parts 0 _rel)
            list(GET _parts 1 _abs)
            string(MAKE_C_IDENTIFIER "${_rel}" _var)
            # Distinct relative paths can share a C identifier; keep them apart.
            if(_var IN_LIST _vars)
                string(APPEND _var "_${_index}")
            endif()
            list(APPEND _vars "${_var}")
            set(_cpp "${_gen_dir}/${_var}.cpp")
            add_custom_command(
                OUTPUT "${_cpp}"
                COMMAND "${Python3_EXECUTABLE}" "${_encoder}"
                        --src "${_abs}" --out "${_cpp}" --namespace "${_lib}" --var "${_var}"
                DEPENDS "${_abs}" "${_encoder}"
                COMMENT "Embedding UI asset ${_rel} into ${_lib}"
                VERBATIM)
            list(APPEND _cpps "${_cpp}")
            string(APPEND _decls
                "extern const unsigned char ${_var}[];\nextern const std::size_t ${_var}_size;\n")
            if(_index EQUAL 0)
                set(_script_var "${_var}")
            elseif(_index EQUAL 1 AND theme_path)
                set(_theme_var "${_var}")
            else()
                string(APPEND _file_entries
                    "    {\"${_rel}\", ${_var}, ${_var}_size},\n")
            endif()
            file(SIZE "${_abs}" _bytes)
            math(EXPR _total_bytes "${_total_bytes} + ${_bytes}")
            math(EXPR _index "${_index} + 1")
        endforeach()
        if(_total_bytes GREATER _PULP_UI_EMBED_SIZE_WARNING_BYTES)
            message(WARNING
                "UI_SCRIPT ${script_path}: embedding ${_total_bytes} bytes of script and "
                "sibling files into every format target. Move files the editor does not "
                "load out of the script's directory, or shrink them.")
        endif()

        if(theme_path)
            set(_theme_data_expr "${_theme_var}")
            set(_theme_size_expr "${_theme_var}_size")
        else()
            set(_theme_data_expr "nullptr")
            set(_theme_size_expr "0")
        endif()
        if(_file_entries)
            list(LENGTH _all_pairs _file_count)
            math(EXPR _file_count "${_file_count} - 1")
            if(theme_path)
                math(EXPR _file_count "${_file_count} - 1")
            endif()
            set(_files_decl
                "inline const ::pulp::format::EmbeddedEditorFile kFiles[] = {\n${_file_entries}};\n")
            set(_files_data_expr "kFiles")
            set(_files_count_expr "${_file_count}")
        else()
            set(_files_decl "")
            set(_files_data_expr "nullptr")
            set(_files_count_expr "0")
        endif()
        set(_header "${_gen_dir}/${_lib}.hpp")
        set(_content "// Generated by pulp_add_plugin(UI_SCRIPT): the editor embedded into the image.
#pragma once
#include <pulp/format/editor_assets.hpp>
#include <cstddef>

namespace ${_lib} {

${_decls}
${_files_decl}
/// The embedded half of the editor record; the registration unit adds the
/// on-disk paths a dev build also records.
inline ::pulp::format::EditorAssets embedded_editor_assets() {
    ::pulp::format::EditorAssets assets{};
    assets.script_name = \"${_script_name}\";
    assets.script = ${_script_var};
    assets.script_size = ${_script_var}_size;
    assets.theme = ${_theme_data_expr};
    assets.theme_size = ${_theme_size_expr};
    assets.files = ${_files_data_expr};
    assets.file_count = ${_files_count_expr};
    return assets;
}

}  // namespace ${_lib}
")
        set(_existing "")
        if(EXISTS "${_header}")
            file(READ "${_header}" _existing)
        endif()
        if(NOT "${_existing}" STREQUAL "${_content}")
            file(WRITE "${_header}" "${_content}")
        endif()

        add_library(${_lib} STATIC ${_cpps})
        set_target_properties(${_lib} PROPERTIES
            LINKER_LANGUAGE CXX
            POSITION_INDEPENDENT_CODE ON)
        set_property(GLOBAL PROPERTY "PULP_UI_EMBED_${_lib}_HEADER" "${_header}")
    endif()
    set(${out_lib} "${_lib}" PARENT_SCOPE)
    set(${out_header} "${_header}" PARENT_SCOPE)
endfunction()

# The editor assets a format target declares are a runtime value owned by that
# target: each selection is recorded as a target property, and one generated
# source per target registers the properties' values at static init through
# pulp::format::register_editor_assets(). A compile definition cannot carry
# them, because the code that builds the editor (ViewBridge) is compiled once
# into the shared Pulp format library, not into each plug-in target. The
# source is generated with $<TARGET_PROPERTY>, so a selection made after the
# target exists (pulp_use_kit_ui) still lands, and the last selection wins.
# When the target embeds a script (PULP_UI_EMBED_HEADER), the same source
# starts from the embedded record and adds the on-disk paths to it.
function(_pulp_attach_editor_assets_source target)
    get_property(_attached TARGET "${target}" PROPERTY _PULP_EDITOR_ASSETS_SOURCE)
    if(_attached)
        return()
    endif()
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/pulp_editor_assets/${target}.cpp")
    set(_script "$<TARGET_PROPERTY:${target},PULP_UI_SCRIPT_PATH>")
    set(_theme "$<TARGET_PROPERTY:${target},PULP_UI_THEME_PATH>")
    set(_roots "$<TARGET_PROPERTY:${target},PULP_UI_ASSET_ROOTS>")
    set(_embed_header "$<TARGET_PROPERTY:${target},PULP_UI_EMBED_HEADER>")
    set(_embed_ns "$<TARGET_PROPERTY:${target},PULP_UI_EMBED_NAMESPACE>")
    set(_has_embed "$<BOOL:${_embed_header}>")
    file(GENERATE OUTPUT "${_source}" CONTENT
"// Generated by Pulp for ${target}: the editor assets this target declares.
#include <pulp/format/editor_assets.hpp>
$<${_has_embed}:#include \"${_embed_header}\">

namespace {
struct PulpEditorAssetsRegistrar {
    PulpEditorAssetsRegistrar() {
        ::pulp::format::EditorAssets assets$<${_has_embed}: = ::${_embed_ns}::embedded_editor_assets()>;
        assets.script_path = R\"pulp_asset(${_script})pulp_asset\";
        assets.theme_path = R\"pulp_asset(${_theme})pulp_asset\";
        assets.asset_roots = R\"pulp_asset(${_roots})pulp_asset\";
        ::pulp::format::register_editor_assets(assets);
    }
} pulp_editor_assets_registrar;
} // namespace
")
    target_sources("${target}" PRIVATE "${_source}")
    set_property(TARGET "${target}" PROPERTY _PULP_EDITOR_ASSETS_SOURCE "${_source}")
endfunction()

# Attach a UI script to one format target: embed its bytes, the theme beside it
# and the files beside it so the image carries the editor, and in a dev build
# also record PULP_UI_SCRIPT_PATH for the hot-reloading edit-and-see loop.
#   THEME <path>        theme override to embed with the script; defaults to a
#                       theme.json beside the script when one exists.
#   ASSET_DIRS <dirs>   further directories whose files are embedded, named
#                       relative to each directory (a kit's PULP_ASSETS).
function(_pulp_apply_ui_script_definition target ui_script_path)
    if(NOT ui_script_path)
        return()
    endif()
    cmake_parse_arguments(UI "" "THEME" "ASSET_DIRS" ${ARGN})
    get_filename_component(_script_dir "${ui_script_path}" DIRECTORY)
    if(NOT DEFINED UI_THEME)
        set(UI_THEME "")
        if(EXISTS "${_script_dir}/theme.json")
            set(UI_THEME "${_script_dir}/theme.json")
        endif()
    endif()
    if(UI_THEME AND NOT EXISTS "${UI_THEME}")
        set(UI_THEME "")
    endif()

    # A script that does not exist yet cannot be encoded; pulp_add_plugin has
    # already warned that the editor falls back to AutoUi until it does.
    if(EXISTS "${ui_script_path}")
        _pulp_ui_embed_sibling_files(_file_pairs "${ui_script_path}" "${UI_THEME}")
        foreach(_asset_dir IN LISTS UI_ASSET_DIRS)
            if(IS_DIRECTORY "${_asset_dir}")
                _pulp_ui_embed_files_under(_more "${_asset_dir}" "")
                list(APPEND _file_pairs ${_more})
            endif()
        endforeach()
        _pulp_ui_script_embed_target(_embed_lib _embed_header
            "${ui_script_path}" "${UI_THEME}" "${_file_pairs}")
        set_property(TARGET "${target}" PROPERTY PULP_UI_EMBED_HEADER "${_embed_header}")
        set_property(TARGET "${target}" PROPERTY PULP_UI_EMBED_NAMESPACE "${_embed_lib}")
        target_link_libraries(${target} PRIVATE ${_embed_lib})
    endif()

    _pulp_ui_script_dev_path_enabled(_dev_path)
    if(_dev_path)
        set_property(TARGET "${target}" PROPERTY PULP_UI_SCRIPT_PATH "${ui_script_path}")
    endif()
    _pulp_attach_editor_assets_source("${target}")
endfunction()

function(_pulp_apply_ui_theme_definition target theme_path)
    if(NOT theme_path)
        return()
    endif()

    # The on-disk theme path belongs with the on-disk script; an embedded
    # document reads the embedded theme bytes instead.
    _pulp_ui_script_dev_path_enabled(_dev_path)
    if(NOT _dev_path)
        return()
    endif()
    set_property(TARGET "${target}" PROPERTY PULP_UI_THEME_PATH "${theme_path}")
    _pulp_attach_editor_assets_source("${target}")
endfunction()

function(_pulp_apply_ui_asset_roots_definition target asset_roots)
    if(NOT asset_roots)
        return()
    endif()

    # Asset roots are absolute source paths too: a shipped image must not name
    # or read them. The embedded editor carries the files from those roots
    # instead (see _pulp_apply_ui_script_definition ASSET_DIRS).
    _pulp_ui_script_dev_path_enabled(_dev_path)
    if(NOT _dev_path)
        return()
    endif()
    set_property(TARGET "${target}" PROPERTY PULP_UI_ASSET_ROOTS "${asset_roots}")
    _pulp_attach_editor_assets_source("${target}")
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
            _pulp_apply_ui_script_definition("${target}_${_fmt}" "${_selected_script_abs}"
                THEME "${_selected_tokens_abs}" ASSET_DIRS ${_selected_assets_abs})
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
