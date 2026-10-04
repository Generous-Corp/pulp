#pragma once

// What a host window showed while a plug-in editor opened: classify every
// distinct image of the host's own window, from the moment the host asked for
// the editor until it settled, into what a user sees. Pure C++ (no AppKit), so
// the classification is unit-tested on synthetic images; the out-of-process
// probe (editor_open_oop_probe.mm) feeds it real read-backs.
//
// The host window carries a loud backdrop (magenta) so "nothing composited
// yet" can never pass as a dark editor background.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pulp::tools::editor_open {

inline constexpr std::uint32_t kHostBackdrop = 0xFF00FF;
// Pulp's former framework default editor background; an editor that shows it
// never declared its own (Processor::editor_background()).
inline constexpr std::uint32_t kFrameworkNavy = 0x1E1E2E;

struct Frame {
    double t_ms = 0;       // since the host asked for the editor
    std::size_t w = 0, h = 0;
    std::vector<std::uint8_t> rgb;  // w * h * 3
};

inline bool near_rgb(const std::uint8_t* p, std::uint32_t c, int tol) {
    return std::abs(int(p[0]) - int(c >> 16)) <= tol &&
           std::abs(int(p[1]) - int((c >> 8) & 255)) <= tol &&
           std::abs(int(p[2]) - int(c & 255)) <= tol;
}

inline bool near_px(const std::uint8_t* p, const std::uint8_t* q, int tol) {
    return std::abs(p[0] - q[0]) <= tol && std::abs(p[1] - q[1]) <= tol &&
           std::abs(p[2] - q[2]) <= tol;
}

// Pixel of `b` at the proportional position of pixel p of `a`.
inline const std::uint8_t* sample_at(const Frame& a, const Frame& b, std::size_t p) {
    const std::size_t x = (p % a.w) * b.w / a.w, y = (p / a.w) * b.h / a.h;
    return &b.rgb[(y * b.w + x) * 3];
}

// Mean absolute RGB difference of 24x16 block means, each image sampled at its
// own scale, so a frame shown mid window-animation compares by content rather
// than by pixel position. `uniform` replaces `s` by a flat colour.
inline double block_distance(const Frame& s, const Frame& last, const std::uint8_t* uniform = nullptr) {
    const int BX = 24, BY = 16;
    double total = 0;
    for (int by = 0; by < BY; ++by)
        for (int bx = 0; bx < BX; ++bx) {
            double a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
            std::size_t na = 0, nb = 0;
            for (std::size_t y = by * s.h / BY; y < (by + 1) * s.h / BY; ++y)
                for (std::size_t x = bx * s.w / BX; x < (bx + 1) * s.w / BX; ++x, ++na)
                    for (int c = 0; c < 3; ++c) a[c] += uniform ? uniform[c] : s.rgb[(y * s.w + x) * 3 + c];
            for (std::size_t y = by * last.h / BY; y < (by + 1) * last.h / BY; ++y)
                for (std::size_t x = bx * last.w / BX; x < (bx + 1) * last.w / BX; ++x, ++nb)
                    for (int c = 0; c < 3; ++c) b[c] += last.rgb[(y * last.w + x) * 3 + c];
            if (!na || !nb) continue;
            for (int c = 0; c < 3; ++c) total += std::fabs(a[c] / double(na) - b[c] / double(nb));
        }
    return total / (BX * BY);
}

// What a user sees in one image, before the editor's settled look:
//   host-empty         the host backdrop (nothing composited yet)
//   empty-bg           the editor's own background with no UI on it
//   partial            some UI, not yet the settled look
//   wrong-size         neither, at a size other than the final one
//   ui-converging      the UI, a pixel or two off the final size
//   ui-host-animating  the UI scaled by the host window's open animation
//   ui                 the settled look
struct OpenStages {
    std::vector<std::string> per_frame;
    std::string stages;              // "empty-bg@52ms(990x645) > ui@258ms(990x645)"
    std::size_t ready = 0;           // index of the first settled image
    int non_ui_content_frames = 0;   // plug-in images before the UI that are not the UI
    int navy_frames = 0;             // images showing the framework default background
    // The last image is still the host backdrop: the read-back never saw the
    // editor (or the editor never drew). The stages are then meaningless —
    // pair the run with a trace from the plug-in process before believing it.
    bool blind = false;
};

// `settled_bg` is the editor's declared background (0xRRGGBB). The last frame
// is taken as the settled look.
inline OpenStages classify_open(const std::vector<Frame>& frames, std::uint32_t settled_bg) {
    OpenStages out;
    if (frames.empty()) return out;
    const Frame& last = frames.back();
    {
        std::size_t backdrop = 0;
        for (std::size_t p = 0; p < last.w * last.h; ++p)
            backdrop += near_rgb(&last.rgb[p * 3], kHostBackdrop, 8);
        if (backdrop * 2 >= last.w * last.h) {
            out.blind = true;
            out.ready = frames.size();
            for (const auto& f : frames) {
                out.per_frame.push_back("host-empty");
                (void)f;
            }
            char buf[96];
            std::snprintf(buf, sizeof buf, "host-empty@%.0fms(%zux%zu)", frames.front().t_ms,
                          frames.front().w, frames.front().h);
            out.stages = buf;
            return out;
        }
    }
    // ready: of the pixels where the last image is not the background, >= 95%
    // already match it. Comparing every pixel would call a background-only
    // frame ready, because a dark editor is mostly background.
    out.ready = frames.size() - 1;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::size_t match = 0, fg = 0;
        for (std::size_t p = 0; p < last.w * last.h; ++p) {
            if (near_rgb(&last.rgb[p * 3], settled_bg, 8)) continue;
            ++fg;
            match += near_px(&last.rgb[p * 3], sample_at(last, frames[i], p), 24);
        }
        if (fg == 0 || match * 20 >= fg * 19) {
            out.ready = i;
            break;
        }
    }
    const std::uint8_t bgc[3] = {std::uint8_t(settled_bg >> 16), std::uint8_t((settled_bg >> 8) & 255),
                                 std::uint8_t(settled_bg & 255)};
    std::string prev;
    int plugin_before_ui = 0, ui_like_before = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const Frame& s = frames[i];
        const std::size_t n = s.w * s.h;
        std::size_t magenta = 0, off_bg = 0, navy = 0;
        for (std::size_t p = 0; p < n; ++p) {
            magenta += near_rgb(&s.rgb[p * 3], kHostBackdrop, 8);
            off_bg += !near_rgb(&s.rgb[p * 3], settled_bg, 8);
            navy += near_rgb(&s.rgb[p * 3], kFrameworkNavy, 2);
        }
        if (navy * 4 >= n) ++out.navy_frames;
        const bool size_ok = std::labs(long(s.w) - long(last.w)) <= 2 && std::labs(long(s.h) - long(last.h)) <= 2;
        const bool ui_like = block_distance(s, last) < 0.35 * block_distance(s, last, bgc);
        std::string st = i >= out.ready        ? "ui"
                         : magenta * 2 >= n    ? "host-empty"
                         : ui_like             ? (size_ok ? "ui-converging" : "ui-host-animating")
                         : !size_ok            ? "wrong-size"
                         : off_bg * 200 <= n   ? "empty-bg"
                                               : "partial";
        if (i < out.ready && (st == "ui-converging" || st == "ui-host-animating")) ++ui_like_before;
        if (i < out.ready && st != "host-empty") ++plugin_before_ui;
        out.per_frame.push_back(st);
        if (st != prev) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s@%.0fms(%zux%zu)", st.c_str(), s.t_ms, s.w, s.h);
            if (!out.stages.empty()) out.stages += " > ";
            out.stages += buf;
            prev = st;
        }
    }
    out.non_ui_content_frames = plugin_before_ui - ui_like_before;
    return out;
}

}  // namespace pulp::tools::editor_open
