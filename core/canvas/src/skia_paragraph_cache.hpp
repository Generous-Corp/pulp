#pragma once

// Internal side of ParagraphCache (paragraph_cache.hpp): the prepared
// paragraph SkiaCanvas text paths share, the key it is cached under, and the
// locked handle every use goes through.

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/paragraph_cache.hpp>

#include "modules/skparagraph/include/Paragraph.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::canvas {

struct PreparedParagraph {
    std::unique_ptr<skia::textlayout::Paragraph> paragraph;
    float advance = 0;
    float alphabetic_baseline = 0;
    // Where paint() actually puts the first baseline below the paragraph
    // origin. With hinting on, SkParagraph snaps that baseline to a whole
    // unit in paragraph space while getAlphabeticBaseline() still reports
    // the face's unrounded ascent, so offsetting by the reported value lands
    // the glyphs up to half a unit away from the requested y.
    float painted_baseline = 0;
    // Space SkParagraph puts in front of the first glyph. It splits CSS
    // letter-spacing evenly around every glyph, while CSS (and Chromium) add
    // it after each glyph, so the run's leading edge sits half a spacing off
    // the requested x. The total advance is the same either way. Measured
    // for left-to-right text only, so right-to-left keeps zero.
    float leading_spacing = 0;
};

// Every input the paragraph build reads. `families` is the stack after CSS
// generic keywords resolve to platform families, so a change to that mapping
// is a different key; `primary_typeface` is the resolved primary face, whose
// optical size and AAT tracking the build applies.
// Float fields must be canonical (finite, and +0 rather than -0): equality
// and hashing compare them by value and by bits respectively.
struct ParagraphKey {
    std::string text;
    std::vector<std::string> families;
    std::uint32_t primary_typeface = 0;
    float size = 0;
    int weight = 0;
    int slant = 0;
    float letter_spacing = 0;
    bool ltr = true;
    // SkParagraph rounds the first baseline only with hinting on, so hinting
    // moves glyphs. SkiaCanvas never turns it off today.
    bool hinting = true;
    // Whether SkParagraph synthesizes a bold or oblique the resolved face
    // lacks, which changes the glyphs it lays out. Left out of the hash: it is
    // almost always true, and equal keys need only hash equally.
    bool fake_missing_font_styles = true;
    std::vector<Canvas::FontFeature> features;
    std::uint64_t font_generation = 0;

    bool operator==(const ParagraphKey&) const = default;
};

struct CachedParagraph {
    PreparedParagraph prepared;
    std::size_t bytes = 0;
    // SkParagraph is not safe to share without a lock: the first paint fills
    // each line's text-blob cache and queries walk mutable state.
    std::mutex use;
};

// A locked use of one paragraph. Holds the entry alive and its mutex for the
// handle's lifetime; empty when the build failed. Never hold two at once: a
// second use of the same key on the same thread would wait on itself.
class ParagraphUse {
  public:
    ParagraphUse() = default;
    explicit ParagraphUse(std::shared_ptr<CachedParagraph> entry)
        : entry_(std::move(entry)),
          lock_(entry_ ? std::unique_lock(entry_->use) : std::unique_lock<std::mutex>()) {}

    explicit operator bool() const {
        return entry_ && entry_->prepared.paragraph;
    }
    PreparedParagraph& operator*() const {
        return entry_->prepared;
    }
    PreparedParagraph* operator->() const {
        return &entry_->prepared;
    }

  private:
    std::shared_ptr<CachedParagraph> entry_;
    std::unique_lock<std::mutex> lock_;
};

// The cached paragraph for @p key, built by @p build(key) on a miss. A failed build
// (null paragraph) is returned but not stored. Disabled, this always builds.
ParagraphUse
find_or_build_paragraph(ParagraphKey key,
                        const std::function<PreparedParagraph(const ParagraphKey&)>& build);

} // namespace pulp::canvas
