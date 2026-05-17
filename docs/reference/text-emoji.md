# Color Emoji in Canvas2D Text

Pulp's `ctx.fillText` / `ctx.strokeText` / `ctx.measureText` resolve color
emoji clusters to a registered emoji typeface and render them as one
glyph cluster per Unicode grapheme — including ZWJ sequences, regional
flag pairs, keycap sequences, and skin-tone modifiers. The Pulp text
pipeline routes Canvas2D text through `SkParagraph`, which combines
HarfBuzz cluster shaping with per-codepoint font fallback against a
`FontCollection` whose default-family list includes the emoji typeface.

This page documents the discovery cascade, the public override API, and
the bundled-font story for cross-platform deterministic rendering.

## Discovery cascade

On the first construction of a `pulp::view::AssetManager` (which a
typical Pulp app instantiates exactly once at startup), the emoji
fallback is auto-discovered via
`pulp::canvas::register_best_available_emoji_fallback()`:

1. **Platform color-emoji typeface** —
   `pulp::canvas::register_platform_emoji_fallback()` asks the platform
   `SkFontMgr` for:
   - macOS: `Apple Color Emoji`
   - Windows: `Segoe UI Emoji`
   - Android: `Noto Color Emoji` (system)
2. **Bundled Noto Color Emoji** — if the platform path returns nothing,
   `pulp::canvas::register_bundled_noto_color_emoji()` loads the
   embedded `external/fonts/NotoColorEmoji.ttf` (SIL OFL 1.1, COLRv1
   variant) and registers it. This is the default Linux / Android /
   headless / CI path.

If no emoji typeface is registered (for example a custom embedded build
with `PULP_BUNDLE_NOTO_COLOR_EMOJI=OFF` on a Linux server without a
platform color-emoji font), Canvas2D text containing emoji codepoints
falls through the segmenter as `Default` runs and the primary font's
`.notdef` glyph is rendered.

## Overriding the emoji typeface

App authors can replace the registered emoji typeface at any point —
useful for ship-with-brand-emoji or for swapping Noto for Twemoji /
JoyPixels / your own emoji set.

### From C++ (canvas-level)

```cpp
#include <pulp/canvas/text_font_context.hpp>

// Option 1: register an already-loaded SkTypeface.
sk_sp<SkTypeface> custom = ...;
pulp::canvas::register_emoji_fallback(std::move(custom));

// Option 2: register by family name. Looks up the family via the
// registered-fonts → bundled-fonts → platform `SkFontMgr` cascade and
// registers the result.
pulp::canvas::register_emoji_fallback("MyBrand Emoji");
```

Both calls bump `pulp::canvas::font_registration_generation()` so the
typeface and segment caches in `skia_canvas.cpp` and `text_shaper.cpp`
flush on the next lookup.

### From a plugin's `register_font` flow

The standard `pulp::canvas::register_font(bytes, size, "MyBrand Emoji")`
entry point registers the bytes as a regular typeface. Pair it with
`pulp::canvas::register_emoji_fallback("MyBrand Emoji")` to also wire
that family as the emoji fallback.

## Bundling the cross-platform fallback

`external/fonts/NotoColorEmoji.ttf` ships under SIL OFL 1.1 and is
gated by the `PULP_BUNDLE_NOTO_COLOR_EMOJI` CMake option:

| Platform | Default | Reasoning |
|----------|---------|-----------|
| Linux    | ON      | No platform color-emoji typeface guaranteed |
| Android  | ON      | Older Android builds may lack a system emoji font |
| Headless / CI | ON | Deterministic emoji rendering for golden tests |
| macOS    | OFF     | "Apple Color Emoji" is always present |
| Windows  | OFF     | "Segoe UI Emoji" is always present |

Override at configure time with
`-DPULP_BUNDLE_NOTO_COLOR_EMOJI=ON` (forces inclusion of the ~5 MB
typeface even on macOS / Windows — useful when you want bit-identical
emoji rendering across all hosts).

## What works (and what doesn't)

The Skia + HarfBuzz + ICU stack handles:

- ✅ Single-codepoint emoji (`😀`, `🎵`, `❤️`)
- ✅ ZWJ sequences (`👨‍👩‍👧`, `🏳️‍🌈`, `👩‍⚕️`) — collapse into one
  composite glyph when the emoji typeface has the ligature, fall back
  to a sequence of base glyphs otherwise.
- ✅ Regional indicator flag pairs (`🇺🇸`, `🇯🇵`, `🇪🇺`)
- ✅ Keycap sequences (`1️⃣`, `#️⃣`, `*️⃣`)
- ✅ Skin-tone modifiers (`👍🏽`, `👩🏿`)
- ✅ Multi-person skin tones (`👩🏽‍🤝‍👨🏼`)
- ✅ FE0F (emoji presentation) selector — promotes a base glyph to
  color emoji.
- ✅ FE0E (text presentation) selector — keeps a base glyph in the
  primary font (`☔︎` stays monochrome even though `☔` defaults to
  color emoji).
- ✅ Tag sequences for subdivision flags (`🏴󠁧󠁢󠁳󠁣󠁴󠁿`)

The above also work with `letter-spacing` — tracking is applied between
grapheme boundaries (via `TextStyle::setLetterSpacing`), not between
glyphs, so an emoji cluster doesn't sprout gaps inside it.

`strokeText` with an emoji cluster effectively leaves the emoji glyph
unchanged: CBDT / COLR color-emoji typefaces typically have empty
outline tables, which matches the CSS canvas-2d behavior. Latin text
in the same call still receives the stroke outline.

## Test coverage

`test/test_canvas_emoji.cpp` covers each of the above categories with
both Catch2 measurement assertions and side-effect PNG renders (written
to `$PULP_EMOJI_TEST_PNG_DIR`, default `/tmp/pulp-emoji-validation/`)
for visual inspection. The PNGs are NOT part of the test pass/fail —
they are diagnostics for humans validating that the color emoji
actually paint. Set `PULP_EMOJI_TEST_WRITE_PNGS=0` to suppress.

## Related

- `core/canvas/include/pulp/canvas/emoji_segmenter.hpp` — Unicode-aware
  grapheme segmenter that the canvas uses to identify emoji runs.
- `core/canvas/include/pulp/canvas/text_font_context.hpp` — the
  `TextFontContext` that owns the `FontCollection` and the registered
  emoji typeface.
- `docs/reference/text-shaping.md` — broader HarfBuzz / SkParagraph
  contract that this builds on.
- `DEPENDENCIES.md` / `NOTICE.md` — Noto Color Emoji license rows.
