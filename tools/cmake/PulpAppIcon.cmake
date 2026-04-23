# PulpAppIcon.cmake — target-level app icon helper
#
# Provides:
#   pulp_app_icon(<target> SOURCE icon.png [...])
#
# Current scope:
# - macOS: generates a bundled .icns via sips + iconutil
# - Windows: generates an .ico + .rc via PowerShell/.NET
# - Android: writes generated launcher PNGs under android/app/src/main/res-generated
#
# Linux currently keeps the source PNG as the canonical icon asset; desktop-file
# packaging is still handled by downstream packaging flows.

set(_PULP_APP_ICON_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_pulp_icon_abs_path out_var base_dir value)
    if(NOT value)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    if(IS_ABSOLUTE "${value}")
        set(_abs "${value}")
    else()
        set(_abs "${base_dir}/${value}")
    endif()

    get_filename_component(_abs "${_abs}" ABSOLUTE)
    set(${out_var} "${_abs}" PARENT_SCOPE)
endfunction()

function(_pulp_icon_read_png_size out_width out_height path)
    file(READ "${path}" _png_header HEX OFFSET 0 LIMIT 24)
    string(TOUPPER "${_png_header}" _png_header)

    if(NOT _png_header MATCHES "^89504E470D0A1A0A0000000D49484452")
        message(FATAL_ERROR
            "pulp_app_icon(): expected a PNG with a valid IHDR header: ${path}")
    endif()

    string(SUBSTRING "${_png_header}" 32 8 _width_hex)
    string(SUBSTRING "${_png_header}" 40 8 _height_hex)
    math(EXPR _width "0x${_width_hex}")
    math(EXPR _height "0x${_height_hex}")

    set(${out_width} "${_width}" PARENT_SCOPE)
    set(${out_height} "${_height}" PARENT_SCOPE)
endfunction()

function(_pulp_icon_pick_variant out_var)
    cmake_parse_arguments(ARG "" "SOURCE;PLATFORM;DEBUG_ICON;RELEASE_ICON" "" ${ARGN})

    set(_picked "${ARG_SOURCE}")

    if(ARG_PLATFORM)
        set(_picked "${ARG_PLATFORM}")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        if(DEFINED CMAKE_DEFAULT_BUILD_TYPE AND CMAKE_DEFAULT_BUILD_TYPE STREQUAL "Debug" AND ARG_DEBUG_ICON)
            set(_picked "${ARG_DEBUG_ICON}")
        elseif(ARG_RELEASE_ICON)
            set(_picked "${ARG_RELEASE_ICON}")
        endif()
    else()
        string(TOLOWER "${CMAKE_BUILD_TYPE}" _cfg_lower)
        if(_cfg_lower STREQUAL "debug" AND ARG_DEBUG_ICON)
            set(_picked "${ARG_DEBUG_ICON}")
        elseif(ARG_RELEASE_ICON)
            set(_picked "${ARG_RELEASE_ICON}")
        endif()
    endif()

    set(${out_var} "${_picked}" PARENT_SCOPE)
endfunction()

function(_pulp_icon_target_platform_override out_var)
    cmake_parse_arguments(ARG "" "MACOS;WINDOWS;IOS;ANDROID;LINUX" "" ${ARGN})

    set(_override "")
    if(PULP_IOS OR IOS)
        set(_override "${ARG_IOS}")
    elseif(APPLE)
        set(_override "${ARG_MACOS}")
    elseif(WIN32)
        set(_override "${ARG_WINDOWS}")
    elseif(ANDROID)
        set(_override "${ARG_ANDROID}")
    elseif(UNIX)
        set(_override "${ARG_LINUX}")
    endif()

    set(${out_var} "${_override}" PARENT_SCOPE)
endfunction()

function(_pulp_icon_validate_source target source_path)
    if(NOT source_path)
        message(FATAL_ERROR
            "pulp_app_icon(${target}): no icon source selected. "
            "Provide SOURCE or a platform override for this platform.")
    endif()
    if(NOT EXISTS "${source_path}")
        message(FATAL_ERROR
            "pulp_app_icon(${target}): icon source does not exist: ${source_path}")
    endif()

    get_filename_component(_ext "${source_path}" EXT)
    string(TOLOWER "${_ext}" _ext_lower)
    if(NOT _ext_lower STREQUAL ".png")
        message(FATAL_ERROR
            "pulp_app_icon(${target}): icon source must be a PNG: ${source_path}")
    endif()

    _pulp_icon_read_png_size(_width _height "${source_path}")
    if(_width LESS 1024 OR _height LESS 1024)
        message(FATAL_ERROR
            "pulp_app_icon(${target}): icon source must be at least 1024x1024; "
            "got ${_width}x${_height}: ${source_path}")
    endif()
endfunction()

function(_pulp_icon_configure_macos target source_path)
    find_program(_pulp_sips sips)
    find_program(_pulp_iconutil iconutil)
    if(NOT _pulp_sips OR NOT _pulp_iconutil)
        message(FATAL_ERROR
            "pulp_app_icon(${target}): macOS icon generation requires both "
            "`sips` and `iconutil`.")
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp_icons/${target}")
    set(_icns "${_out_dir}/AppIcon.icns")

    add_custom_command(
        OUTPUT "${_icns}"
        COMMAND "${CMAKE_COMMAND}"
            -DINPUT_PNG=${source_path}
            -DOUTPUT_ICNS=${_icns}
            -DSIPS_EXECUTABLE=${_pulp_sips}
            -DICONUTIL_EXECUTABLE=${_pulp_iconutil}
            -P "${_PULP_APP_ICON_CMAKE_DIR}/PulpGenerateMacIcns.cmake"
        DEPENDS "${source_path}" "${_PULP_APP_ICON_CMAKE_DIR}/PulpGenerateMacIcns.cmake"
        COMMENT "Generating app icon bundle for ${target}"
        VERBATIM
    )

    set_source_files_properties("${_icns}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
        GENERATED TRUE
    )
    target_sources(${target} PRIVATE "${_icns}")
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
    )
endfunction()

function(_pulp_icon_configure_windows target source_path)
    find_program(_pulp_powershell powershell)
    if(NOT _pulp_powershell)
        find_program(_pulp_powershell pwsh)
    endif()
    if(NOT _pulp_powershell)
        message(FATAL_ERROR
            "pulp_app_icon(${target}): Windows icon generation requires PowerShell.")
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp_icons/${target}")
    set(_ico "${_out_dir}/app.ico")
    set(_rc  "${_out_dir}/app_icon.rc")

    add_custom_command(
        OUTPUT "${_ico}" "${_rc}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
        COMMAND "${_pulp_powershell}" -NoProfile -ExecutionPolicy Bypass
            -File "${_PULP_APP_ICON_CMAKE_DIR}/PulpGenerateWindowsIcon.ps1"
            -InputPng "${source_path}"
            -OutputIco "${_ico}"
            -RcPath "${_rc}"
        DEPENDS "${source_path}" "${_PULP_APP_ICON_CMAKE_DIR}/PulpGenerateWindowsIcon.ps1"
        COMMENT "Generating Windows app icon for ${target}"
        VERBATIM
    )

    set_source_files_properties("${_ico}" "${_rc}" PROPERTIES GENERATED TRUE)
    target_sources(${target} PRIVATE "${_rc}")
endfunction()

function(_pulp_icon_configure_android target source_path)
    set(_android_main_dir "${CMAKE_SOURCE_DIR}/android/app/src/main")
    set(_android_res_dir "${_android_main_dir}/res-generated")
    if(NOT EXISTS "${_android_main_dir}")
        message(WARNING
            "pulp_app_icon(${target}): Android project not found at ${_android_main_dir}; "
            "skipping Android launcher icon generation.")
        return()
    endif()

    foreach(_density IN ITEMS mdpi hdpi xhdpi xxhdpi xxxhdpi)
        file(MAKE_DIRECTORY "${_android_res_dir}/mipmap-${_density}")
        file(COPY_FILE "${source_path}" "${_android_res_dir}/mipmap-${_density}/ic_launcher.png" ONLY_IF_DIFFERENT)
        file(COPY_FILE "${source_path}" "${_android_res_dir}/mipmap-${_density}/ic_launcher_round.png" ONLY_IF_DIFFERENT)
    endforeach()
endfunction()

function(pulp_app_icon target)
    cmake_parse_arguments(ICON
        ""
        "SOURCE;MACOS;WINDOWS;IOS;ANDROID;LINUX;DEBUG_ICON;RELEASE_ICON"
        ""
        ${ARGN}
    )

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "pulp_app_icon(${target}): target does not exist")
    endif()

    _pulp_icon_target_platform_override(_platform_override
        MACOS "${ICON_MACOS}"
        WINDOWS "${ICON_WINDOWS}"
        IOS "${ICON_IOS}"
        ANDROID "${ICON_ANDROID}"
        LINUX "${ICON_LINUX}"
    )

    _pulp_icon_pick_variant(_selected_rel
        SOURCE "${ICON_SOURCE}"
        PLATFORM "${_platform_override}"
        DEBUG_ICON "${ICON_DEBUG_ICON}"
        RELEASE_ICON "${ICON_RELEASE_ICON}"
    )
    _pulp_icon_abs_path(_selected_abs "${CMAKE_CURRENT_SOURCE_DIR}" "${_selected_rel}")
    _pulp_icon_validate_source(${target} "${_selected_abs}")

    if(APPLE AND NOT (PULP_IOS OR IOS))
        _pulp_icon_configure_macos(${target} "${_selected_abs}")
    elseif(PULP_IOS OR IOS)
        message(WARNING
            "pulp_app_icon(${target}): iOS app-icon catalog generation is not "
            "implemented yet; keeping the selected source recorded for downstream packaging.")
        set_property(TARGET ${target} PROPERTY PULP_IOS_APP_ICON "${_selected_abs}")
    elseif(WIN32)
        _pulp_icon_configure_windows(${target} "${_selected_abs}")
    elseif(ANDROID)
        _pulp_icon_configure_android(${target} "${_selected_abs}")
    elseif(UNIX AND NOT APPLE)
        set_property(TARGET ${target} PROPERTY PULP_LINUX_APP_ICON "${_selected_abs}")
    endif()

    set_property(TARGET ${target} PROPERTY PULP_APP_ICON_SOURCE "${_selected_abs}")
endfunction()
