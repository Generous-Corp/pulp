add_library(pulp-canvas)
add_library(pulp::canvas ALIAS pulp-canvas)

target_include_directories(pulp-canvas
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-canvas PRIVATE
    src/color.cpp
    src/recording_canvas.cpp
    src/svg.cpp
    src/effects.cpp
    src/text_layout.cpp
    src/attributed_string.cpp
    src/text_shaper.cpp
    src/sdf_atlas.cpp
    src/msdf_atlas.cpp
    src/sdf_atlas_cache.cpp
    src/path_to_sdf.cpp
    # font_registry_stubs.cpp provides non-Skia fallbacks for the public
    # font-registration API (pulp #1150). The real implementations live in
    # bundled_fonts.cpp, which is only compiled under PULP_HAS_SKIA. This
    # stub TU lets plugin startup code call register_font(...) on non-GPU
    # builds without guarding every call site.
    src/font_registry_stubs.cpp
)

# Text shaping via SkParagraph (default ON when Skia available, can be disabled)
if(PULP_HAS_TEXT_SHAPING)
    target_compile_definitions(pulp-canvas PRIVATE PULP_HAS_TEXT_SHAPING=1)
endif()

# nanosvg (zlib license) for SVG loading
target_include_directories(pulp-canvas PRIVATE
    ${CMAKE_SOURCE_DIR}/external/nanosvg
)

# Platform-specific rendering backends
if(APPLE)
    target_sources(pulp-canvas PRIVATE
        platform/mac/cg_canvas.mm
    )
    target_link_libraries(pulp-canvas PRIVATE
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Foundation"
        "-framework ImageIO"
        "-framework CoreServices"
    )
endif()

target_link_libraries(pulp-canvas PUBLIC pulp::runtime)

# Skia Graphite GPU rendering backend
if(PULP_HAS_SKIA)
    target_sources(pulp-canvas PRIVATE
        src/skia_canvas.cpp
        src/bundled_fonts.cpp
    )

    # Embed external/fonts/*.ttf into a static lib that pulp-canvas links
    # privately. The Skia prebuilt does not export SkFontMgr_New_Custom_Data
    # (FreeType is not linked in), so we materialise these into SkTypefaces
    # via the platform font manager at runtime — see bundled_fonts.cpp.
    # Keep the SOURCES list aligned with the bundled_blobs() table in
    # bundled_fonts.cpp; the C++ side names the symbols directly. Issue #932.
    #
    # PulpEmbedData provides pulp_add_binary_data() in isolation from the
    # rest of PulpUtils.cmake (which fails fast unless format/view targets
    # already exist). Pulled in directly so the canvas subdir doesn't have
    # to wait for the parent to inject the helper.
    include("${CMAKE_SOURCE_DIR}/tools/cmake/PulpEmbedData.cmake")
    pulp_add_binary_data(pulp-bundled-fonts
        SOURCES
            ${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf
            ${CMAKE_SOURCE_DIR}/external/fonts/JetBrainsMono-Regular.ttf
        NAMESPACE pulp_bundled_fonts
    )
    target_link_libraries(pulp-canvas PRIVATE pulp-bundled-fonts)
    # Mark for the parent CMakeLists.txt install() loop. pulp-canvas exports
    # through PulpTargets and CMake refuses to export a target that links
    # PRIVATEly to a static lib outside the export set, so the parent has
    # to add pulp-bundled-fonts to PULP_SDK_TARGETS for installs.
    set_target_properties(pulp-bundled-fonts PROPERTIES
        EXPORT_NAME bundled-fonts)

    target_link_libraries(pulp-canvas PUBLIC skia::skia)
    if(TARGET skia::dawn)
        target_link_libraries(pulp-canvas PUBLIC skia::dawn)
    endif()
    target_compile_definitions(pulp-canvas PUBLIC PULP_HAS_SKIA=1)
    target_include_directories(pulp-canvas PRIVATE ${SKIA_INCLUDE_DIRS})

    # Platform font managers for Skia text rendering
    if(WIN32)
        target_link_libraries(pulp-canvas PRIVATE dwrite)
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            # IMPORTED_TARGET makes pkg-config build a proper INTERFACE
            # `PkgConfig::FONTCONFIG` target that CMake transitively
            # propagates through INTERFACE_LINK_LIBRARIES — including
            # the actual `-lfontconfig` flag and link directories.
            # Without this, the bare `FONTCONFIG_LIBRARIES=fontconfig`
            # string can fall through CMake's static-lib transitive
            # propagation without producing a `-lfontconfig` on the
            # consumer's final ld line, and the chrome/m144 Skia
            # archive's `SkFontMgr_New_FontConfig` (FcInit*, FcConfig*,
            # FcPattern*, FcGetVersion, FcFontSet*) fails to resolve at
            # pulp-cpp link time. Closes the linux-x64 leg of release-cli's
            # backfill matrix (#1962, #1970 follow-up).
            pkg_check_modules(FONTCONFIG IMPORTED_TARGET fontconfig)
            if(FONTCONFIG_FOUND)
                target_link_libraries(pulp-canvas PUBLIC PkgConfig::FONTCONFIG)
            endif()
        endif()
    # Android: Skia bundles FreeType + HarfBuzz, no system fontconfig needed
    endif()
endif()
