#pragma once

// Frame-identity stamp: paints an update index into a rendered frame as a row
// of bit cells plus a row holding their complement, so a capture can prove
// which committed update reached the screen instead of how similar two frames
// look. A persistent scene that silently stops refreshing passes every
// similarity score; it cannot pass this, because its frames keep decoding to
// the same index.
//
// Layout (logical pixels, before the capture scale):
//   row 0: `bits` square cells of side `cell`, most significant bit first;
//          white for 1, black for 0.
//   row 1: the same cells, complemented.
// The grid starts at (`x`, `y`). tools/motion/visual/frame_stamp_decode.py
// reads the same layout from PNG captures; the constants below are the
// defaults both sides share.
//
// Decoding never guesses. A cell whose sampled luminance sits between the
// black and white thresholds, or a bit whose two rows agree, makes the whole
// frame unreadable (-1) rather than a misread index.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>

namespace pulp::test_support {

struct FrameStampLayout {
    float x = 0.0f;
    float y = 0.0f;
    float cell = 8.0f;
    int bits = 16;
};

/// Paints `index` (taken modulo 2^bits) as the stamp grid.
void paint_frame_stamp(canvas::Canvas& canvas, uint32_t index, const FrameStampLayout& layout = {});

/// Decodes the stamp from tightly packed RGBA8 pixels captured at `scale`
/// device pixels per logical pixel. Returns -1 when any cell is ambiguous or
/// any bit agrees with its complement, or when the grid lies outside the image.
int decode_frame_stamp(const uint8_t* rgba, uint32_t width, uint32_t height, float scale,
                       const FrameStampLayout& layout = {});

/// A test-only view that paints the stamp for its current update index.
class FrameStampView : public view::View {
  public:
    explicit FrameStampView(FrameStampLayout layout = {}) : layout_(layout) {}

    void set_update_index(uint32_t index) {
        index_ = index;
        request_repaint();
    }
    uint32_t update_index() const {
        return index_;
    }

    void paint(canvas::Canvas& canvas) override {
        paint_frame_stamp(canvas, index_, layout_);
    }

  private:
    FrameStampLayout layout_;
    uint32_t index_ = 0;
};

} // namespace pulp::test_support
