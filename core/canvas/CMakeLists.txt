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
    # emoji_segmenter is Skia-free so it builds on every config and
    # supports the SkShaper / SkParagraph callers as well as standalone
    # unit tests. Its job is to pre-segment text into emoji vs default
    # runs so HarfBuzz can shape multi-codepoint emoji clusters as one
    # cluster — see the header for the contract.
    src/emoji_segmenter.cpp
    # font_registry_stubs.cpp provides non-Skia fallbacks for the public
    # font-registration API (pulp #1150). The real implementations live in
    # bundled_fonts.cpp, which is only compiled under PULP_HAS_SKIA. This
    # stub TU lets plugin startup code call register_font(...) on non-GPU
    # builds without guarding every call site.
    src/font_registry_stubs.cpp
    # Pulp #2163 — Phase 1 / Slice 1.1.a of the font v2 roadmap. FontOptions
    # is Skia-free; FontScope manages scope-level registrations + a
    # monotonic generation counter; FontResolver is the canonical
    # resolution path that will progressively absorb the legacy parsers in
    # skia_canvas.cpp / text_shaper.cpp / bundled_fonts.cpp / sdf_atlas.cpp.
    src/font_options.cpp
    src/font_scope.cpp
    src/font_resolver.cpp
    # Pulp #2163 — Phase 1 / Slice 1.2.a. ShapedText is the canonical
    # artifact produced by TextRunPlanner; both measurement
    # (TextShaper) and paint (SkiaCanvas) consume the same instance
    # so they cannot diverge. Skeleton wraps existing TextShaper
    # internally; the real bidi/script iterator swap is the finish
    # half of 1.2.a.
    src/text_run_planner.cpp
    # Pulp #2163 — Phase 2 / Slice 2.2. FontFlightRecorder is the
    # bounded process-global trace buffer that callers (the --font-
    # trace CLI flag, the import-missing-font-advisor, debug overlays)
    # drain to see every typeface resolution the resolver performed.
    src/font_flight_recorder.cpp
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
        platform/mac/cg_canvas_gradients.mm
        platform/mac/cg_canvas_shadow.mm
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
        src/skia_canvas_text.cpp
        src/skia_canvas_filter.cpp
        src/skia_canvas_mask.cpp
        src/skia_canvas_gradients.cpp
        src/skia_canvas_box_shadow.cpp
        src/skia_canvas_opacity.cpp
        src/skia_canvas_shaders.cpp
        src/bundled_fonts.cpp
        # TextFontContext glues emoji_segmenter into the SkShaper +
        # SkParagraph paths and owns the per-context FontCollection used
        # for Label/measurement.
        src/text_font_context.cpp
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

    # Optional bundled Noto Color Emoji typeface — gated by the
    # PULP_BUNDLE_NOTO_COLOR_EMOJI option so macOS / Windows release
    # builds can drop the ~5 MB payload and delegate to the platform
    # color-emoji typeface instead. Defaults ON for Linux, Android,
    # headless / CI builds, and developer builds that want deterministic
    # emoji rendering across platforms.
    set(_pulp_default_noto ON)
    if(APPLE OR WIN32)
        # Platform color-emoji typeface ("Apple Color Emoji" / "Segoe UI
        # Emoji") covers the release path; default off to avoid bloat.
        set(_pulp_default_noto OFF)
    endif()
    option(PULP_BUNDLE_NOTO_COLOR_EMOJI
           "Embed Noto Color Emoji as the cross-platform color-emoji fallback"
           ${_pulp_default_noto})

    if(PULP_BUNDLE_NOTO_COLOR_EMOJI)
        pulp_add_binary_data(pulp-bundled-noto-color-emoji
            SOURCES
                ${CMAKE_SOURCE_DIR}/external/fonts/NotoColorEmoji.ttf
            NAMESPACE pulp_bundled_noto_color_emoji
        )
        set_target_properties(pulp-bundled-noto-color-emoji PROPERTIES
            EXPORT_NAME bundled-noto-color-emoji)
        target_link_libraries(pulp-canvas PRIVATE pulp-bundled-noto-color-emoji)
        target_sources(pulp-canvas PRIVATE src/noto_color_emoji.cpp)
        target_compile_definitions(pulp-canvas PRIVATE PULP_BUNDLE_NOTO_COLOR_EMOJI=1)
    else()
        target_sources(pulp-canvas PRIVATE src/noto_color_emoji_stub.cpp)
    endif()

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
                # Force `-lfontconfig` to appear at the END of every
                # downstream binary's link line, regardless of where
                # CMake places the transitive PkgConfig::FONTCONFIG dep.
                # GNU ld processes static archives left-to-right and
                # won't re-scan to resolve `libskia.a(SkFontMgr_fontconfig.o)`'s
                # `Fc*` references once it moves past them. The PUBLIC
                # link above gives consumers fontconfig in the right
                # position when only pulp::canvas is linked directly,
                # but breaks down for transitive consumers (e.g. an
                # executable linking only pulp::view → pulp::canvas) —
                # those see libskia.a get pulled in before -lfontconfig
                # appears on the line. Re-mentioning fontconfig as an
                # INTERFACE_LINK_OPTIONS entry guarantees it tails every
                # consumer's link, fixing the entire class of:
                #
                #   undefined reference to `FcGetVersion`, `FcPatternGetMatrix`,
                #   `FcConfigSubstitute`, `FcCharSetHasChar`, etc.
                #
                # This obsoletes the per-target `pulp_link_fontconfig_after_skia`
                # calls in pulp-cli / pulp-import-design / _pulp_add_standalone
                # for new code; existing calls remain harmless redundancy.
                target_link_options(pulp-canvas INTERFACE "-lfontconfig")
            endif()
        endif()
    # Android: Skia bundles FreeType + HarfBuzz, no system fontconfig needed
    endif()
endif()
