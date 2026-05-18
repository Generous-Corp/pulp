// test_parity.cpp — Pulp #2163 follow-up, font v2 Slice 1.3.
//
// Measurement-paint parity harness. Asserts that
// `TextShaper::prepare(text, family, size).total_width()` matches
// what Skia's `SkFont::measureText` reports — the same metric the
// painter walks. Pre-resolver these two paths could drift because
// resolution went through different cascades; post-1.1.a both
// converge on `FontResolver::resolve_family_list`, so the harness
// asserts the architectural invariant the v2 plan calls "the
// killer" gap: measure equals paint, every frame, every script.
//
// V1 (this commit): width parity only, hand-picked sample set
// representative of the CHAIN INFO / CROSSOVER / MID-SIDE-WIDTH
// bugs that drove the v2 plan. The full multilingual torture
// corpus (60 entries in test/text_corpus/corpus.json) loader +
// per-TextAnchor bbox assertions arrive in the next iteration of
// this file once we have a JSON parser linked into the test
// target. This MVP proves the parity property is testable and
// guards the regressions Slice 1.3 was specifically created for.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/text_shaper.hpp>
#include <pulp/canvas/bundled_fonts.hpp>

#include <cmath>
#include <string>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRect.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#endif

namespace {

struct Sample {
    const char* text;
    const char* family;
    float size;
    const char* note;
};

constexpr Sample kSamples[] = {
    // Latin baselines — proportional + monospace.
    { "Hello world", "Inter", 14.0f, "Latin proportional regular" },
    { "Hello world", "IBM Plex Mono", 14.0f, "Latin monospace regular" },

    // The original #2163 callbacks — the sizes that drove the
    // empirical safety margin into TextShaper::measure_metrics.
    { "CROSSOVER",          "IBM Plex Mono", 7.0f,  "fontSize 7 section title (Chainer)" },
    { "MID / SIDE WIDTH",   "IBM Plex Mono", 7.0f,  "fontSize 7 section title (Chainer)" },
    { "polywave generator", "IBM Plex Mono", 8.0f,  "CHAIN INFO description text" },
    { "OSC",                "IBM Plex Mono", 9.0f,  "CHAIN INFO bold label" },
    { "ENV",                "IBM Plex Mono", 9.0f,  "CHAIN INFO bold label" },

    // Symbol coverage — the U+2192 arrow that landed in the
    // original Chainer tofu-box debugging session.
    { "res→",  "IBM Plex Mono", 7.0f,  "fontSize 7 axis label with U+2192" },
    { "OSC → freq", "IBM Plex Mono", 9.0f, "label with arrow" },
};

} // namespace

TEST_CASE("font v2 Slice 1.3 — measurement-paint width parity within 0.5px",
          "[font][parity][issue-2163]") {
#ifndef PULP_HAS_SKIA
    SUCCEED("Skia not compiled — parity harness needs SkFont::measureText");
    return;
#else
    auto& shaper = pulp::canvas::global_text_shaper();
    sk_sp<SkFontMgr> mgr = pulp::canvas::platform_font_manager();
    REQUIRE(mgr);

    constexpr float kTolerancePx = 0.5f;

    for (const auto& s : kSamples) {
        INFO("text='" << s.text
             << "' family='" << s.family
             << "' size="    << s.size
             << " (" << s.note << ")");

        // Skia reference: ask the font manager directly for the
        // requested family, then measure with raw Skia. This is
        // the same path the painter walks for advance-only metrics.
        sk_sp<SkTypeface> tf = mgr->matchFamilyStyle(s.family, SkFontStyle::Normal());
        if (!tf) {
            // Family not installed on this CI host (e.g. IBM Plex
            // Mono on a stock Ubuntu runner). Skip rather than
            // fail — bundled-font cascade is exercised separately.
            WARN("family '" << s.family << "' not resolvable on this host — skipped");
            continue;
        }
        SkFont font(tf, s.size);
        std::string text_owned = s.text;
        float painted_w = font.measureText(
            text_owned.data(), text_owned.size(), SkTextEncoding::kUTF8);

        // Pulp's prediction: TextShaper, the same path Yoga's
        // measure callback consults via Label::intrinsic_width.
        auto prepared = shaper.prepare(s.text, s.family, s.size);
        float predicted_w = prepared.total_width();

        const float delta = std::abs(predicted_w - painted_w);
        INFO("predicted=" << predicted_w
             << " painted=" << painted_w
             << " delta="   << delta);
        REQUIRE(delta <= kTolerancePx);
    }
#endif
}

TEST_CASE("font v2 Slice 1.3 — empty text reports zero width", "[font][parity]") {
    auto& shaper = pulp::canvas::global_text_shaper();
    auto prepared = shaper.prepare("", "Inter", 14.0f);
    REQUIRE(prepared.total_width() == 0.0f);
}

TEST_CASE("font v2 Slice 1.3 — ascent + descent are both positive", "[font][parity]") {
    // The TextShaper API contract documents that ascent/descent are
    // returned as POSITIVE distances above/below baseline (Skia's
    // fAscent is negative; we flip it). The painter and the Yoga
    // baseline callback both rely on this sign convention.
    auto& shaper = pulp::canvas::global_text_shaper();
    auto prepared = shaper.prepare("Aj", "Inter", 14.0f);
    if (prepared.metrics_are_real()) {
        REQUIRE(prepared.ascent()  > 0.0f);
        REQUIRE(prepared.descent() > 0.0f);
    }
}
