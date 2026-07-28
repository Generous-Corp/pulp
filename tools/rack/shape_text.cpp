// shape_text — real text shaping for Eurorack panel lettering.
//
// nanosvg (and therefore Rack) has no text support at all, so every label must
// ship as outlines. That is usually where panel typography goes wrong: naive
// emitters advance each glyph by a fixed box, so narrow letters like I and 1
// float in a gap the same width as an M.
//
// This uses the same stack Pulp's own TextShaper is built on — SkShaper, which
// is HarfBuzz — so glyph positions come from the font's real advances and GPOS
// kerning, then SkFont::getPath turns each positioned glyph into outlines. One
// flat SVG path comes out, kerned exactly as the font intends.
//
// Usage: shape_text <text> <font.ttf> <cap-height-mm> [center|left]
// Writes the path `d` to stdout and "<advance-width-mm> <cap-height-mm>" to
// stderr so the caller can position it.

#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathTypes.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_mac_ct.h"
#include "include/utils/SkParsePath.h"
#include "modules/skshaper/include/SkShaper.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

/// Collects positioned glyphs from the shaper and converts each to outlines.
class PathRunHandler final : public SkShaper::RunHandler {
public:
    SkPathBuilder combined;
    SkScalar advance = 0;

    void beginLine() override { x_ = 0; }
    void runInfo(const RunInfo&) override {}
    void commitRunInfo() override {}

    Buffer runBuffer(const RunInfo& info) override {
        glyphs_.resize(info.glyphCount);
        positions_.resize(info.glyphCount);
        font_ = info.fFont;
        return Buffer{glyphs_.data(), positions_.data(), nullptr, nullptr, {x_, 0}};
    }

    void commitRunBuffer(const RunInfo& info) override {
        // Each glyph is placed where HarfBuzz says it goes -- real advances,
        // real kerning pairs -- and only then turned into an outline.
        for (size_t i = 0; i < glyphs_.size(); ++i) {
            if (auto p = font_.getPath(glyphs_[i])) {
                combined.addPath(p->makeOffset(positions_[i].x(), positions_[i].y()));
            }
        }
        x_ += info.fAdvance.x();
        advance = x_;
    }

    void commitLine() override {}

private:
    std::vector<SkGlyphID> glyphs_;
    std::vector<SkPoint> positions_;
    SkFont font_;
    SkScalar x_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: shape_text <text> <font.ttf> <cap-mm> [center|left]\n");
        return 2;
    }
    const std::string text = argv[1];
    const char* fontPath = argv[2];
    const double capMm = std::atof(argv[3]);
    const bool center = argc < 5 || std::strcmp(argv[4], "center") == 0;

    auto mgr = SkFontMgr_New_CoreText(nullptr);
    sk_sp<SkTypeface> tf;
    if (auto data = SkData::MakeFromFileName(fontPath)) {
        tf = mgr->makeFromData(data);
    }
    if (!tf) {
        std::fprintf(stderr, "shape_text: could not load font %s\n", fontPath);
        return 1;
    }

    // Size the font so its CAP HEIGHT is exactly capMm. Panel lettering is
    // specified by cap height, not em size -- two fonts at the same em size
    // have visibly different cap heights, and the whole family must share one.
    SkFont probe(tf, 100.0f);
    SkFontMetrics fm;
    probe.getMetrics(&fm);
    const float capAtProbe = fm.fCapHeight > 0 ? fm.fCapHeight : 70.0f;
    SkFont font(tf, static_cast<float>(100.0 * capMm / capAtProbe));
    font.setSubpixel(true);
    font.setHinting(SkFontHinting::kNone);

    auto shaper = SkShaper::Make(mgr);
    if (!shaper) {
        std::fprintf(stderr, "shape_text: no shaper available\n");
        return 1;
    }

    PathRunHandler handler;
    shaper->shape(text.c_str(), text.size(), font, true, 1e9f, &handler);

    SkPath path = handler.combined.detach();
    // Baseline at y=0, so the caller places by baseline; centre horizontally on
    // the shaped advance rather than on the ink bounds, which keeps optically
    // centred labels stable when the last glyph has a side bearing.
    if (center) path = path.makeOffset(-handler.advance / 2.0f, 0);

    SkString d = SkParsePath::ToSVGString(path);
    std::printf("%s\n", d.c_str());
    std::fprintf(stderr, "%.4f %.4f\n", static_cast<double>(handler.advance), capMm);
    return 0;
}
