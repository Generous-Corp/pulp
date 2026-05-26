// VectorScene + SceneNode subtree implementation. Item 6.1 of
// `planning/2026-05-24-macos-plugin-authoring-plan.md` — see
// `core/canvas/include/pulp/canvas/scene/scene.hpp` for the
// license-lineage note and design rationale.

#include <pulp/canvas/scene/scene.hpp>

// Bring in the nanosvg parser declarations only. The existing `svg.cpp`
// translation unit owns the `#define NANOSVG_IMPLEMENTATION` so we do
// NOT redefine it here — that would produce duplicate-symbol link
// errors. We get the function prototypes (`nsvgParse`, `nsvgDelete`)
// and the data layout (`NSVGimage`, `NSVGshape`, `NSVGpath`).
#include <nanosvg.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pulp::canvas {

// ── SceneRect helpers ────────────────────────────────────────────────────

SceneRect SceneTransform::map_rect(const SceneRect& r) const {
    if (r.empty()) return r;
    // Map four corners and AABB them — handles rotation, shear, scale, translation.
    const float xs[4] = {r.x, r.x + r.w, r.x,        r.x + r.w};
    const float ys[4] = {r.y, r.y,        r.y + r.h, r.y + r.h};
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool first = true;
    for (int i = 0; i < 4; ++i) {
        const float tx = a * xs[i] + c * ys[i] + e;
        const float ty = b * xs[i] + d * ys[i] + f;
        if (first) {
            minx = maxx = tx;
            miny = maxy = ty;
            first = false;
        } else {
            minx = std::min(minx, tx); maxx = std::max(maxx, tx);
            miny = std::min(miny, ty); maxy = std::max(maxy, ty);
        }
    }
    return {minx, miny, maxx - minx, maxy - miny};
}

// ── SceneNode::mark_dirty walks up to the root scene and unions bounds ──

void SceneNode::mark_dirty() {
    dirty_ = true;
    // Propagate to ancestors so a group containing the mutated node
    // also reports dirty for its own paint-bounds union.
    SceneGroup* p = parent_;
    while (p != nullptr) {
        p->dirty_ = true;
        p = p->parent_;
    }
}

// ── SceneGroup ───────────────────────────────────────────────────────────

SceneRect SceneGroup::local_bounds() const {
    SceneRect acc{};
    for (const auto& c : children_) {
        if (!c->visible()) continue;
        acc = acc.united(c->transformed_local_bounds());
    }
    return acc;
}

void SceneGroup::paint_geometry(Canvas& canvas) const {
    // The walker (VectorScene::paint) handles save/restore + transform +
    // opacity for every node, INCLUDING this group. We only need to walk
    // children in order, recursing through nested groups.
    for (const auto& c : children_) {
        if (!c->visible()) continue;
        const bool needs_alpha = c->opacity() < 1.0f;
        const bool needs_xform = !c->transform().is_identity();
        if (needs_alpha || needs_xform) {
            canvas.save();
            if (needs_xform) {
                const auto& t = c->transform();
                canvas.concat_transform(t.a, t.b, t.c, t.d, t.e, t.f);
            }
            if (needs_alpha) {
                canvas.set_opacity(c->opacity());
            }
            c->paint_geometry(canvas);
            canvas.restore();
        } else {
            c->paint_geometry(canvas);
        }
    }
}

// ── ScenePath ────────────────────────────────────────────────────────────

SceneRect ScenePath::local_bounds() const {
    if (!bounds_dirty_) return bounds_cache_;
    bool any = false;
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    auto add = [&](float x, float y) {
        if (!any) {
            minx = maxx = x; miny = maxy = y; any = true;
        } else {
            if (x < minx) minx = x; if (x > maxx) maxx = x;
            if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
    };
    // AABB over all path control points. Cubic / quad curves are bounded
    // by their hull, so this slightly over-estimates the *visual* bounds
    // but never under-estimates them — which is exactly what a repaint
    // rect needs.
    for (const auto& cmd : commands_) {
        switch (cmd.op) {
            case Op::move_to:
            case Op::line_to:
                add(cmd.f0, cmd.f1);
                break;
            case Op::quad_to:
                add(cmd.f0, cmd.f1);
                add(cmd.f2, cmd.f3);
                break;
            case Op::cubic_to:
                add(cmd.f0, cmd.f1);
                add(cmd.f2, cmd.f3);
                add(cmd.f4, cmd.f5);
                break;
            case Op::close:
                break;
        }
    }
    if (any && stroke_enabled_) {
        const float r = stroke_width_ * 0.5f;
        minx -= r; miny -= r; maxx += r; maxy += r;
    }
    bounds_cache_ = any ? SceneRect{minx, miny, maxx - minx, maxy - miny}
                        : SceneRect{};
    bounds_dirty_ = false;
    return bounds_cache_;
}

void ScenePath::paint_geometry(Canvas& canvas) const {
    if (commands_.empty()) return;
    if (fill_enabled_) canvas.set_fill_color(fill_color_);
    if (stroke_enabled_) {
        canvas.set_stroke_color(stroke_color_);
        canvas.set_line_width(stroke_width_);
    }
    canvas.begin_path();
    for (const auto& cmd : commands_) {
        switch (cmd.op) {
            case Op::move_to:  canvas.move_to(cmd.f0, cmd.f1); break;
            case Op::line_to:  canvas.line_to(cmd.f0, cmd.f1); break;
            case Op::quad_to:
                canvas.quad_to(cmd.f0, cmd.f1, cmd.f2, cmd.f3);
                break;
            case Op::cubic_to:
                canvas.cubic_to(cmd.f0, cmd.f1, cmd.f2, cmd.f3, cmd.f4, cmd.f5);
                break;
            case Op::close:    canvas.close_path(); break;
        }
    }
    if (fill_enabled_) canvas.fill_current_path(fill_rule_);
    if (stroke_enabled_) canvas.stroke_current_path();
}

// ── SceneShape ───────────────────────────────────────────────────────────

std::unique_ptr<SceneShape> SceneShape::make_rect(float x, float y,
                                                  float w, float h) {
    auto s = std::make_unique<SceneShape>();
    s->shape_kind_ = Kind::rect;
    s->set_rect(x, y, w, h);
    return s;
}

std::unique_ptr<SceneShape> SceneShape::make_rounded_rect(float x, float y,
                                                          float w, float h,
                                                          float radius) {
    auto s = std::make_unique<SceneShape>();
    s->shape_kind_ = Kind::rounded_rect;
    s->set_rounded_rect(x, y, w, h, radius);
    return s;
}

std::unique_ptr<SceneShape> SceneShape::make_circle(float cx, float cy,
                                                    float radius) {
    auto s = std::make_unique<SceneShape>();
    s->shape_kind_ = Kind::circle;
    s->set_circle(cx, cy, radius);
    return s;
}

std::unique_ptr<SceneShape> SceneShape::make_ellipse(float cx, float cy,
                                                     float rx, float ry) {
    auto s = std::make_unique<SceneShape>();
    s->shape_kind_ = Kind::ellipse;
    s->set_ellipse(cx, cy, rx, ry);
    return s;
}

std::unique_ptr<SceneShape> SceneShape::make_line(float x0, float y0,
                                                  float x1, float y1) {
    auto s = std::make_unique<SceneShape>();
    s->shape_kind_ = Kind::line;
    s->set_line(x0, y0, x1, y1);
    return s;
}

void SceneShape::set_rect(float x, float y, float w, float h) {
    shape_kind_ = Kind::rect;
    if (x == x_ && y == y_ && w == w_ && h == h_) return;
    x_ = x; y_ = y; w_ = w; h_ = h; radius_ = 0;
    mark_dirty();
}

void SceneShape::set_rounded_rect(float x, float y, float w, float h,
                                  float radius) {
    shape_kind_ = Kind::rounded_rect;
    if (x == x_ && y == y_ && w == w_ && h == h_ && radius == radius_) return;
    x_ = x; y_ = y; w_ = w; h_ = h; radius_ = radius;
    mark_dirty();
}

void SceneShape::set_circle(float cx, float cy, float radius) {
    shape_kind_ = Kind::circle;
    // Encode center / radius into x,y,w,h so local_bounds() stays uniform.
    const float new_x = cx - radius;
    const float new_y = cy - radius;
    const float new_w = radius * 2.0f;
    if (new_x == x_ && new_y == y_ && new_w == w_ && new_w == h_ &&
        radius == radius_) return;
    x_ = new_x; y_ = new_y; w_ = new_w; h_ = new_w;
    radius_ = radius;
    mark_dirty();
}

void SceneShape::set_ellipse(float cx, float cy, float rx, float ry) {
    shape_kind_ = Kind::ellipse;
    const float new_x = cx - rx;
    const float new_y = cy - ry;
    const float new_w = rx * 2.0f;
    const float new_h = ry * 2.0f;
    if (new_x == x_ && new_y == y_ && new_w == w_ && new_h == h_) return;
    x_ = new_x; y_ = new_y; w_ = new_w; h_ = new_h;
    // radius_ stores rx; ry is recovered from h_.
    radius_ = rx;
    mark_dirty();
}

void SceneShape::set_line(float x0, float y0, float x1, float y1) {
    shape_kind_ = Kind::line;
    if (x0 == x_ && y0 == y_ && x1 == w_ && y1 == h_) return;
    x_ = x0; y_ = y0; w_ = x1; h_ = y1;
    mark_dirty();
}

SceneRect SceneShape::local_bounds() const {
    SceneRect bounds{};
    switch (shape_kind_) {
        case Kind::rect:
        case Kind::rounded_rect:
        case Kind::circle:
        case Kind::ellipse:
            bounds = {x_, y_, w_, h_};
            break;
        case Kind::line: {
            const float minx = std::min(x_, w_);
            const float miny = std::min(y_, h_);
            const float maxx = std::max(x_, w_);
            const float maxy = std::max(y_, h_);
            bounds = {minx, miny, maxx - minx, maxy - miny};
            break;
        }
    }
    if (stroke_enabled_ && !bounds.empty()) {
        const float r = stroke_width_ * 0.5f;
        bounds.x -= r; bounds.y -= r;
        bounds.w += stroke_width_; bounds.h += stroke_width_;
    }
    return bounds;
}

void SceneShape::paint_geometry(Canvas& canvas) const {
    if (fill_enabled_) canvas.set_fill_color(fill_color_);
    if (stroke_enabled_) {
        canvas.set_stroke_color(stroke_color_);
        canvas.set_line_width(stroke_width_);
    }
    switch (shape_kind_) {
        case Kind::rect:
            if (fill_enabled_) canvas.fill_rect(x_, y_, w_, h_);
            if (stroke_enabled_) canvas.stroke_rect(x_, y_, w_, h_);
            break;
        case Kind::rounded_rect:
            if (fill_enabled_)
                canvas.fill_rounded_rect(x_, y_, w_, h_, radius_);
            if (stroke_enabled_)
                canvas.stroke_rounded_rect(x_, y_, w_, h_, radius_);
            break;
        case Kind::circle: {
            const float cx = x_ + w_ * 0.5f;
            const float cy = y_ + h_ * 0.5f;
            if (fill_enabled_) canvas.fill_circle(cx, cy, radius_);
            if (stroke_enabled_) canvas.stroke_circle(cx, cy, radius_);
            break;
        }
        case Kind::ellipse: {
            // Emulate ellipse via path: 4-cubic-bezier approximation.
            const float cx = x_ + w_ * 0.5f;
            const float cy = y_ + h_ * 0.5f;
            const float rx = w_ * 0.5f;
            const float ry = h_ * 0.5f;
            constexpr float kappa = 0.5522847498f;  // (4/3)*tan(pi/8)
            const float ox = rx * kappa;
            const float oy = ry * kappa;
            canvas.begin_path();
            canvas.move_to(cx - rx, cy);
            canvas.cubic_to(cx - rx, cy - oy, cx - ox, cy - ry, cx, cy - ry);
            canvas.cubic_to(cx + ox, cy - ry, cx + rx, cy - oy, cx + rx, cy);
            canvas.cubic_to(cx + rx, cy + oy, cx + ox, cy + ry, cx, cy + ry);
            canvas.cubic_to(cx - ox, cy + ry, cx - rx, cy + oy, cx - rx, cy);
            canvas.close_path();
            if (fill_enabled_) canvas.fill_current_path(FillRule::nonzero);
            if (stroke_enabled_) canvas.stroke_current_path();
            break;
        }
        case Kind::line:
            if (stroke_enabled_) canvas.stroke_line(x_, y_, w_, h_);
            break;
    }
}

// ── SceneImage ───────────────────────────────────────────────────────────

void SceneImage::paint_geometry(Canvas& canvas) const {
    if (w_ <= 0 || h_ <= 0) return;
    if (!rgba_pixels_.empty() && pixel_width_ > 0 && pixel_height_ > 0) {
        // write_pixels lays the buffer down 1:1; for arbitrary dst-rect
        // resampling the caller should hand us an encoded blob (PNG/JPEG)
        // and let the backend decode + scale. write_pixels keeps the
        // headless / RecordingCanvas test path simple.
        canvas.write_pixels(rgba_pixels_.data(), pixel_width_, pixel_height_,
                            static_cast<int>(x_), static_cast<int>(y_));
        return;
    }
    if (!encoded_bytes_.empty()) {
        canvas.draw_image_from_data(encoded_bytes_.data(), encoded_bytes_.size(),
                                    x_, y_, w_, h_);
        return;
    }
    if (!file_path_.empty()) {
        canvas.draw_image_from_file(file_path_, x_, y_, w_, h_);
        return;
    }
}

// ── SceneText ────────────────────────────────────────────────────────────

SceneRect SceneText::local_bounds() const {
    if (text_.empty()) return {x_, y_, 0, 0};
    const float w = measured_width_ > 0.0f
                        ? measured_width_
                        // Cheap estimate: 0.6 em per character. The exact
                        // pixel-accurate width is the TextShaper's job; the
                        // estimate is good enough to drive the dirty-rect
                        // walker when the caller hasn't measured.
                        : static_cast<float>(text_.size()) * font_size_ * 0.6f;
    const float h = font_size_ * 1.2f;
    return {x_, y_ - h * 0.8f, w, h};
}

void SceneText::paint_geometry(Canvas& canvas) const {
    if (text_.empty()) return;
    canvas.set_font(font_family_, font_size_);
    canvas.set_fill_color(fill_color_);
    canvas.fill_text(text_, x_, y_);
    if (stroke_enabled_) {
        canvas.set_stroke_color(stroke_color_);
        canvas.stroke_text(text_, x_, y_);
    }
}

// ── VectorScene ──────────────────────────────────────────────────────────

void VectorScene::note_node_dirtied(const SceneNode& node) {
    // Union both the previously-painted scene-space bounds (so the old
    // pixels get cleared) and the new transformed local bounds (so the
    // new pixels get drawn). When the node is fresh, last_painted_bounds
    // is empty and the union collapses to the new bounds — also correct.
    SceneRect old_bounds = node.last_painted_bounds();
    SceneRect new_bounds = node.transformed_local_bounds();
    // Walk up ancestor transforms to convert new_bounds to scene space.
    const SceneGroup* p = node.parent();
    while (p != nullptr) {
        new_bounds = p->transform().map_rect(new_bounds);
        p = p->parent();
    }
    dirty_rect_pending_ = dirty_rect_pending_.united(old_bounds);
    dirty_rect_pending_ = dirty_rect_pending_.united(new_bounds);
}

SceneRect VectorScene::take_dirty_rect() {
    // Two paths to populate dirty_rect_pending_:
    //   1. Callers that explicitly invoke `note_node_dirtied(n)` — those
    //      pre-populated `dirty_rect_pending_` with old + new bounds of
    //      the exact mutated node. We return that as-is.
    //   2. Callers who only call mutating setters — `mark_dirty()`
    //      propagated dirty bits up the tree. In that case
    //      `dirty_rect_pending_` is still empty and we fall back to
    //      walking dirty *leaf* (non-group) descendants. Group nodes
    //      are skipped because their `local_bounds()` is the union of
    //      all children — including the unchanged ones — which would
    //      overstate the repaint extent and defeat the whole point of
    //      the retained graph.
    if (dirty_rect_pending_.empty()) {
        root_->for_each_descendant([&](SceneNode& n) {
            if (!n.dirty()) return;
            if (n.kind() == SceneNodeKind::group) return;  // see above
            SceneRect b = n.transformed_local_bounds();
            const SceneGroup* p = n.parent();
            while (p != nullptr) {
                b = p->transform().map_rect(b);
                p = p->parent();
            }
            dirty_rect_pending_ = dirty_rect_pending_.united(b);
            dirty_rect_pending_ =
                dirty_rect_pending_.united(n.last_painted_bounds());
        });
    }
    SceneRect out = dirty_rect_pending_;
    dirty_rect_pending_ = SceneRect{};
    return out;
}

void VectorScene::paint(Canvas& canvas) {
    // Paint helper applies save/restore + transform + opacity uniformly
    // to every node, then captures the painted bounds for dirty-rect math.
    auto compose_scene_bounds = [](const SceneNode& n) {
        SceneRect b = n.transformed_local_bounds();
        const SceneGroup* p = n.parent();
        while (p != nullptr) {
            b = p->transform().map_rect(b);
            p = p->parent();
        }
        return b;
    };

    // Root group transform + opacity (rarely set, but support it).
    const bool needs_alpha = root_->opacity() < 1.0f;
    const bool needs_xform = !root_->transform().is_identity();
    if (needs_alpha || needs_xform) {
        canvas.save();
        if (needs_xform) {
            const auto& t = root_->transform();
            canvas.concat_transform(t.a, t.b, t.c, t.d, t.e, t.f);
        }
        if (needs_alpha) canvas.set_opacity(root_->opacity());
        root_->paint_geometry(canvas);
        canvas.restore();
    } else {
        root_->paint_geometry(canvas);
    }

    // Mark every node clean and capture last-painted scene bounds.
    auto mark_walk = [&](SceneNode& n) {
        n.mark_clean(compose_scene_bounds(n));
    };
    root_->mark_clean(compose_scene_bounds(*root_));
    root_->for_each_descendant(mark_walk);
    dirty_rect_pending_ = SceneRect{};
}

// ── SVG ingest (reuses the existing nanosvg parser) ──────────────────────

VectorScene VectorScene::from_svg_string(const std::string& svg_data) {
    VectorScene scene;
    if (svg_data.empty()) return scene;
    std::string copy = svg_data;
    NSVGimage* img = nsvgParse(copy.data(), "px", 96.0f);
    if (!img) return scene;

    // Walk the nanosvg shapes and build ScenePath children parented to
    // the scene's root group. Mirrors the cubic-Bezier decoding used by
    // `SvgImage::render` so the same SVG documents that already render
    // through the immediate-mode path produce identical command streams
    // when materialised into the retained graph.
    for (NSVGshape* shape = img->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        const bool has_fill = (shape->fill.type == NSVG_PAINT_COLOR);
        const bool has_stroke = (shape->stroke.type == NSVG_PAINT_COLOR);
        if (!has_fill && !has_stroke) continue;

        auto* path_node = scene.root().emplace_child<ScenePath>();
        path_node->set_fill_enabled(has_fill);
        path_node->set_stroke_enabled(has_stroke);
        if (has_fill) {
            const uint32_t c = shape->fill.color;
            path_node->set_fill_color(Color::rgba8(
                static_cast<uint8_t>(c & 0xFF),
                static_cast<uint8_t>((c >> 8) & 0xFF),
                static_cast<uint8_t>((c >> 16) & 0xFF),
                static_cast<uint8_t>(shape->opacity * 255.0f)));
            path_node->set_fill_rule(shape->fillRule == NSVG_FILLRULE_EVENODD
                                         ? FillRule::evenodd
                                         : FillRule::nonzero);
        }
        if (has_stroke) {
            const uint32_t c = shape->stroke.color;
            path_node->set_stroke_color(Color::rgba8(
                static_cast<uint8_t>(c & 0xFF),
                static_cast<uint8_t>((c >> 8) & 0xFF),
                static_cast<uint8_t>((c >> 16) & 0xFF),
                static_cast<uint8_t>(shape->opacity * 255.0f)));
            path_node->set_stroke_width(shape->strokeWidth);
        }

        // Optional id round-trip (nanosvg copies the SVG element id when
        // present); helps callers address shapes after import.
        if (shape->id[0] != '\0') {
            path_node->set_id(shape->id);
        }

        for (NSVGpath* path = shape->paths; path; path = path->next) {
            if (path->npts < 1) continue;
            const float* pts = path->pts;
            path_node->move_to(pts[0], pts[1]);
            for (int i = 1; i + 2 < path->npts; i += 3) {
                const float* g = &pts[i * 2];
                path_node->cubic_to(g[0], g[1], g[2], g[3], g[4], g[5]);
            }
            if (path->closed) path_node->close_path();
        }
    }

    nsvgDelete(img);
    return scene;
}

VectorScene VectorScene::from_svg_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return from_svg_string(ss.str());
}

}  // namespace pulp::canvas
