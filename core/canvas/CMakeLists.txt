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
    # Lottie playback. Always compiled; the body is a no-op unless PULP_LOTTIE
    # is on AND the active Skia build ships the skottie module (detected below).
    src/lottie_animation.cpp
    # VectorScene + SceneNode are built on top of core/canvas::Path and
    # SkiaCanvas so custom-paint widgets and SVG imports can mutate one
    # child while repainting only that subtree's bounding box.
    src/scene.cpp
    src/effects.cpp
    src/text_layout.cpp
    src/attributed_string.cpp
    src/text_shaper.cpp
    # BidiAnalyzer wraps SheenBidi (Apache-2.0) to give
    # core/canvas::TextShaper a paragraph-level UBA pass that doesn't
    # depend on system ICU — the canonical fallback for iOS / Android /
    # headless builds. See core/canvas/include/pulp/canvas/bidi.hpp.
    src/bidi.cpp
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
    # font-registration API. The real implementations live in
    # bundled_fonts.cpp, which is only compiled under PULP_HAS_SKIA. This
    # stub TU lets plugin startup code call register_font(...) on non-GPU
    # builds without guarding every call site.
    src/font_registry_stubs.cpp
    # FontOptions is Skia-free; FontScope manages scope-level registrations
    # and a monotonic generation counter; FontResolver is the canonical
    # resolution path shared by skia_canvas.cpp, text_shaper.cpp,
    # bundled_fonts.cpp, and sdf_atlas.cpp.
    src/font_options.cpp
    src/font_scope.cpp
    src/font_resolver.cpp
    # ShapedText is the canonical artifact produced by TextRunPlanner; both
    # measurement (TextShaper) and paint (SkiaCanvas) consume the same
    # instance so they cannot diverge.
    src/text_run_planner.cpp
    # FontFlightRecorder is the bounded process-global trace buffer that
    # callers such as the --font-trace CLI flag, the missing-font advisor,
    # and debug overlays drain to inspect typeface resolution.
    src/font_flight_recorder.cpp
    # GIF89a and Baseline TIFF 6.0 readers/writers are Skia-free, so they
    # compile on every config (headless / GPU-off / no Skia).
    # See include/pulp/canvas/image_codecs.hpp for the public API.
    src/image_codecs_gif.cpp
    src/image_codecs_tiff.cpp
)

# Text shaping via SkParagraph (default ON when Skia available, can be disabled)
if(PULP_HAS_TEXT_SHAPING)
    target_compile_definitions(pulp-canvas PRIVATE PULP_HAS_TEXT_SHAPING=1)
endif()

# SheenBidi-backed BidiAnalyzer. PULP_HAS_SHEENBIDI is set by
# tools/cmake/PulpDependencies.cmake when the FetchContent step
# succeeds. When undefined, bidi.cpp compiles to a single-run pass-
# through that infers level from BidiBaseDirection only (LTR-correct).
if(PULP_HAS_SHEENBIDI AND TARGET SheenBidi::SheenBidi)
    target_link_libraries(pulp-canvas PRIVATE SheenBidi::SheenBidi)
    target_compile_definitions(pulp-canvas PRIVATE PULP_HAS_SHEENBIDI=1)
endif()

# nanosvg (zlib license) for SVG loading
target_include_directories(pulp-canvas PRIVATE
    ${PULP_ROOT_DIR}/external/nanosvg
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
        src/skia_canvas_fonts.cpp
        src/skia_canvas_text.cpp
        src/skia_canvas_filter.cpp
        src/skia_canvas_mask.cpp
        src/skia_canvas_gradients.cpp
        src/skia_canvas_box_shadow.cpp
        src/box_shadow_cache.cpp
        src/svg_dom_cache.cpp
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
    # bundled_fonts.cpp; the C++ side names the symbols directly.
    #
    # PulpEmbedData provides pulp_add_binary_data() in isolation from the
    # rest of PulpUtils.cmake (which fails fast unless format/view targets
    # already exist). Pulled in directly so the canvas subdir doesn't have
    # to wait for the parent to inject the helper.
    include("${PULP_ROOT_DIR}/tools/cmake/PulpEmbedData.cmake")
    pulp_add_binary_data(pulp-bundled-fonts
        SOURCES
            ${PULP_ROOT_DIR}/external/fonts/Inter-Regular.ttf
            ${PULP_ROOT_DIR}/external/fonts/JetBrainsMono-Regular.ttf
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
                ${PULP_ROOT_DIR}/external/fonts/NotoColorEmoji.ttf
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

    # skresources — the module that owns skresources::ResourceProvider and its
    # DataURIResourceProviderProxy. SkSVGDOM resolves <image href="..."> ONLY
    # through a resource provider (SkSVGImage::LoadImage returns early on a null
    # one), so without it every <image> element — including the base64 data URIs
    # a design-tool SVG export uses for its bitmap assets — renders BLANK.
    # src/svg_dom_cache.cpp installs the data-URI proxy when this is available.
    #
    # Detection is a real try-link, not a header or archive check: where the
    # symbols live varies by slice. The pinned mac/linux/windows bundles compile
    # the skresources objects INTO libsvg.a (there is no libskresources.a at
    # all), while other slices ship a standalone libskresources.a — and the wasm
    # slice ships neither (same missing-skjson/skresources gap that disables
    # skottie there, see tools/cmake/FindSkia.cmake). A try-link is the only
    # probe that gets all three right.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES skia::skia)
    set(CMAKE_REQUIRED_INCLUDES ${SKIA_INCLUDE_DIRS})
    check_cxx_source_compiles("
        #include \"include/core/SkFontMgr.h\"
        #include \"modules/skresources/include/SkResources.h\"
        int main() {
            auto rp = skresources::DataURIResourceProviderProxy::Make(
                nullptr, skresources::ImageDecodeStrategy::kPreDecode);
            return rp ? 0 : 1;
        }
    " PULP_SKRESOURCES_LINKS)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_INCLUDES)
    if(PULP_SKRESOURCES_LINKS)
        target_compile_definitions(pulp-canvas PUBLIC PULP_HAS_SKRESOURCES=1)
        message(STATUS "Pulp: SVG <image> data-URI decoding enabled (skresources links)")
    else()
        message(STATUS "Pulp: this Skia bundle cannot link skresources — SVG "
                       "<image> elements (incl. base64 data URIs) render blank")
    endif()

    # Opt-in Lottie playback (PULP_LOTTIE). skottie/sksg ship inside the Skia
    # bundle and are already linked via skia::skia's glob, and the skottie
    # include dir is already in SKIA_INCLUDE_DIRS — so enabling Lottie only adds
    # a compile definition. Detection is a real try-link, not just a header
    # check: some Skia bundles ship the skottie headers + archive but omit its
    # SkJSON/skresources dependency, so skottie compiles but does not link. The
    # try-link guarantees turning PULP_LOTTIE on can never break the build — it
    # auto-disables (LottieView degrades to a no-op) on a bundle that can't link
    # skottie, and lights up on one that can.
    if(PULP_LOTTIE)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_LIBRARIES skia::skia)
        check_cxx_source_compiles("
            #include \"modules/skottie/include/Skottie.h\"
            int main() {
                auto a = skottie::Animation::Builder().make(\"{}\", 2);
                return a ? 0 : 1;
            }
        " PULP_SKOTTIE_LINKS)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(PULP_SKOTTIE_LINKS)
            target_compile_definitions(pulp-canvas PUBLIC PULP_HAS_LOTTIE=1)
            message(STATUS "Pulp: Lottie playback enabled (skottie links)")
        else()
            message(STATUS "Pulp: PULP_LOTTIE requested but this Skia bundle cannot "
                           "link skottie (missing skjson/skresources) — Lottie disabled")
        endif()
    endif()

    # font_registry_stubs.cpp compiles direct icu::Locale / BreakIterator calls
    # when PULP_HAS_SKIA and ICU public headers are on the include path (true on
    # Linux with libicu-dev). The bundled Skia statically links SkUnicode but does
    # NOT export ICU's own symbols, so link system ICU here or the Linux link
    # fails with undefined `icu_NN::Locale::...`. macOS links libicucore
    # implicitly; Linux must be explicit. (Surfaced by the local Linux VM CI lane.)
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(ICU COMPONENTS uc i18n data QUIET)
        if(ICU_FOUND)
            target_link_libraries(pulp-canvas PRIVATE ICU::uc ICU::i18n ICU::data)
        endif()
    endif()

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
            # consumer's final ld line, and the chrome/m144 Skia archive's
            # `SkFontMgr_New_FontConfig` (FcInit*, FcConfig*, FcPattern*,
            # FcGetVersion, FcFontSet*) fails to resolve at pulp-cpp link time.
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
