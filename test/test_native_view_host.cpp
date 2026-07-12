// R1 (Valdi study): NativeViewHost — a Yoga-laid-out widget that embeds a
// platform-native child view. These headless tests exercise the pure geometry
// (absolute frame + scroll tracking + clip-to-ancestor intersection) and the
// attach / bounds / clip / detach lifecycle against fake hosts that record the
// primitive traffic — no window, no AppKit. The CALayer-mask application itself
// is platform code verified by an on-device smoke; here we assert the widget
// computes and pushes the correct rectangles.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <utility>
#include <vector>

using namespace pulp::view;

namespace {

struct Recorded {
    bool attached = false;
    int attach_count = 0;
    int detach_count = 0;
    Rect bounds{};
    int bounds_count = 0;  // set_native_child_view_bounds calls (re-push proof)
    bool has_clip = false;
    Rect clip{};
    int clip_count = 0;
    void* child = nullptr;
};

// Fake PluginViewHost recording the native-child primitive traffic, including
// the new set_native_child_view_clip.
class RecordingPluginHost final : public PluginViewHost {
public:
    explicit RecordingPluginHost(bool support = true) : support_(support) {}

    NativeViewHandle native_handle() override { return &host_sentinel_; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t w, uint32_t h) override { size_ = {w, h}; }
    Size get_size() const override { return size_; }

    bool attach_native_child_view(NativeViewHandle child, float x, float y,
                                  float w, float h) override {
        if (!support_ || !child) return false;
        rec_.child = child;
        rec_.bounds = {x, y, w, h};
        rec_.attached = true;
        ++rec_.attach_count;
        return true;
    }
    bool set_native_child_view_bounds(NativeViewHandle child, float x, float y,
                                      float w, float h) override {
        if (!rec_.attached || child != rec_.child) return false;
        rec_.bounds = {x, y, w, h};
        ++rec_.bounds_count;
        return true;
    }
    bool set_native_child_view_clip(NativeViewHandle child, bool has_clip,
                                    float x, float y, float w, float h) override {
        if (!rec_.attached || child != rec_.child) return false;
        rec_.has_clip = has_clip;
        rec_.clip = {x, y, w, h};
        ++rec_.clip_count;
        return true;
    }
    void detach_native_child_view(NativeViewHandle child) override {
        if (child == rec_.child) {
            rec_.attached = false;
            rec_.child = nullptr;
            ++rec_.detach_count;
        }
    }

    // Install a design->host viewport transform (mirrors the mac hosts'
    // design_viewport_transform). Inactive by default → the base false
    // (identity) is returned, so pre-existing tests observe raw coords.
    void set_viewport(float sx, float sy, float tx, float ty) {
        vp_active_ = true;
        vp_sx_ = sx; vp_sy_ = sy; vp_tx_ = tx; vp_ty_ = ty;
    }
    bool design_viewport_transform(float& sx, float& sy, float& tx,
                                   float& ty) const override {
        if (!vp_active_) return false;
        sx = vp_sx_; sy = vp_sy_; tx = vp_tx_; ty = vp_ty_;
        return true;
    }

    const Recorded& rec() const { return rec_; }

private:
    bool support_ = true;
    int host_sentinel_ = 0;
    Size size_{};
    Recorded rec_{};
    bool vp_active_ = false;
    float vp_sx_ = 1, vp_sy_ = 1, vp_tx_ = 0, vp_ty_ = 0;
};

// Fake WindowHost twin (void* signatures).
class RecordingWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
    void* native_window_handle() const override { return nullptr; }

    bool attach_native_child_view(void* child, float x, float y, float w,
                                  float h) override {
        if (!child) return false;
        rec_.child = child;
        rec_.bounds = {x, y, w, h};
        rec_.attached = true;
        ++rec_.attach_count;
        return true;
    }
    bool set_native_child_view_bounds(void* child, float x, float y, float w,
                                      float h) override {
        if (!rec_.attached || child != rec_.child) return false;
        rec_.bounds = {x, y, w, h};
        return true;
    }
    bool set_native_child_view_clip(void* child, bool has_clip, float x, float y,
                                    float w, float h) override {
        if (!rec_.attached || child != rec_.child) return false;
        rec_.has_clip = has_clip;
        rec_.clip = {x, y, w, h};
        ++rec_.clip_count;
        return true;
    }
    void detach_native_child_view(void* child) override {
        if (child == rec_.child) {
            rec_.attached = false;
            rec_.child = nullptr;
            ++rec_.detach_count;
        }
    }

    const Recorded& rec() const { return rec_; }

private:
    Recorded rec_{};
};

int g_fake_native_view = 0;
NativeViewHandle fake_handle() { return &g_fake_native_view; }

// Hosts that do NOT override set_native_child_view_clip — they exercise the
// base-class default (unsupported → false), the path a Win/Linux/Android host
// takes today.
class BarePluginHost final : public PluginViewHost {
public:
    NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t, uint32_t) override {}
    Size get_size() const override { return {}; }
};

class BareWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
    void* native_window_handle() const override { return nullptr; }
};

} // namespace

TEST_CASE("Host set_native_child_view_clip defaults to unsupported (false)",
          "[view][native-view-host]") {
    BarePluginHost plugin;
    REQUIRE_FALSE(plugin.set_native_child_view_clip(fake_handle(), true, 0, 0,
                                                    10, 10));
    REQUIRE_FALSE(plugin.set_native_child_view_clip(fake_handle(), false, 0, 0,
                                                    0, 0));
    BareWindowHost window;
    REQUIRE_FALSE(window.set_native_child_view_clip(fake_handle(), true, 0, 0,
                                                    10, 10));
}

TEST_CASE("NativeViewHost attaches once a host and handle are present",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({40, 30, 120, 80});
    root.add_child(std::move(owned));

    // No handle yet → nothing attaches even after a layout pump.
    nvh->update_native_layout();
    REQUIRE_FALSE(nvh->is_native_attached());
    REQUIRE(host.rec().attach_count == 0);

    // Supply a handle → attaches on the next pump with the computed frame.
    nvh->set_native_child(fake_handle());
    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());
    REQUIRE(host.rec().attach_count == 1);
    REQUIRE(host.rec().child == fake_handle());
    REQUIRE(host.rec().bounds == Rect{40, 30, 120, 80});
    // Unclipped: fully inside the root, so no clip is applied.
    REQUIRE_FALSE(host.rec().has_clip);
}

TEST_CASE("NativeViewHost frame tracks nested offset and scroll",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto sv_owned = std::make_unique<ScrollView>();
    auto* sv = sv_owned.get();
    sv->set_bounds({10, 20, 200, 200});
    sv->set_content_size({200, 600});
    root.add_child(std::move(sv_owned));

    auto nvh_owned = std::make_unique<NativeViewHost>();
    auto* nvh = nvh_owned.get();
    nvh->set_bounds({5, 5, 100, 100});
    nvh->set_native_child(fake_handle());
    sv->add_child(std::move(nvh_owned));

    // Unscrolled: frame = sv origin (10,20) + child (5,5) = (15,25).
    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());
    REQUIRE(nvh->computed_child_frame() == Rect{15, 25, 100, 100});
    REQUIRE_FALSE(nvh->is_clipped());  // fully within the 200x200 viewport

    // Scroll down 50 → the child rides up with the content (y = 25 - 50 = -25)
    // and now clips against the viewport's top edge (y = 20).
    sv->set_scroll(0, 50);
    REQUIRE(sv->scroll_y() == 50.0f);
    nvh->update_native_layout();
    REQUIRE(nvh->computed_child_frame() == Rect{15, -25, 100, 100});
    REQUIRE(nvh->is_clipped());
    REQUIRE(nvh->computed_visible_rect() == Rect{15, 20, 100, 55});
    REQUIRE(host.rec().bounds == Rect{15, -25, 100, 100});
    REQUIRE(host.rec().has_clip);
    // Clip is expressed in the child's own box: visible - frame origin.
    REQUIRE(host.rec().clip == Rect{0, 45, 100, 55});
}

TEST_CASE("NativeViewHost clips to an overflow:hidden ancestor",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto clip_owned = std::make_unique<View>();
    auto* clip = clip_owned.get();
    clip->set_bounds({0, 0, 100, 100});
    clip->set_overflow(View::Overflow::hidden);
    root.add_child(std::move(clip_owned));

    auto nvh_owned = std::make_unique<NativeViewHost>();
    auto* nvh = nvh_owned.get();
    nvh->set_bounds({50, 50, 100, 100});  // spills past the 100x100 clip
    nvh->set_native_child(fake_handle());
    clip->add_child(std::move(nvh_owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_clipped());
    REQUIRE(nvh->computed_visible_rect() == Rect{50, 50, 50, 50});
    REQUIRE(host.rec().clip == Rect{0, 0, 50, 50});
}

TEST_CASE("NativeViewHost clips to a plain View with overflow:scroll",
          "[view][native-view-host]") {
    // A plain <div style="overflow:scroll"> (NOT a ScrollView) carries
    // Overflow::scroll and clips its painted box; the native child must clip to
    // it too. Regression guard for is_clip_container missing scroll/auto.
    View root;
    root.set_bounds({0, 0, 400, 400});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto clip_owned = std::make_unique<View>();
    auto* clip = clip_owned.get();
    clip->set_bounds({0, 0, 80, 80});
    clip->set_overflow(View::Overflow::scroll);
    root.add_child(std::move(clip_owned));

    auto nvh_owned = std::make_unique<NativeViewHost>();
    auto* nvh = nvh_owned.get();
    nvh->set_bounds({40, 40, 100, 100});  // spills past the 80x80 clip
    nvh->set_native_child(fake_handle());
    clip->add_child(std::move(nvh_owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_clipped());
    REQUIRE(nvh->computed_visible_rect() == Rect{40, 40, 40, 40});
    REQUIRE(host.rec().clip == Rect{0, 0, 40, 40});
}

TEST_CASE("NativeViewHost fully scrolled out collapses to an empty clip",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto sv_owned = std::make_unique<ScrollView>();
    auto* sv = sv_owned.get();
    sv->set_bounds({0, 0, 100, 100});
    sv->set_content_size({100, 1000});
    root.add_child(std::move(sv_owned));

    auto nvh_owned = std::make_unique<NativeViewHost>();
    auto* nvh = nvh_owned.get();
    nvh->set_bounds({0, 0, 100, 100});
    nvh->set_native_child(fake_handle());
    sv->add_child(std::move(nvh_owned));

    sv->set_scroll(0, 500);  // child pushed entirely above the viewport
    nvh->update_native_layout();
    REQUIRE(nvh->is_clipped());
    REQUIRE(nvh->computed_visible_rect().is_empty());
    REQUIRE(host.rec().has_clip);
    REQUIRE(host.rec().clip.width == 0.0f);
    REQUIRE(host.rec().clip.height == 0.0f);
}

TEST_CASE("NativeViewHost detaches when the host is removed",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({0, 0, 50, 50});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());

    // Host teardown nulls the back-reference → the widget must detach.
    root.set_plugin_view_host(nullptr);
    REQUIRE_FALSE(nvh->is_native_attached());
    REQUIRE(host.rec().detach_count == 1);
}

TEST_CASE("NativeViewHost detaches in its destructor",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    {
        auto owned = std::make_unique<NativeViewHost>();
        auto* nvh = owned.get();
        nvh->set_bounds({0, 0, 50, 50});
        nvh->set_native_child(fake_handle());
        root.add_child(std::move(owned));
        nvh->update_native_layout();
        REQUIRE(nvh->is_native_attached());
        // Remove + drop → destructor runs while the host is still alive.
        auto removed = root.remove_child(nvh);
    }
    REQUIRE(host.rec().detach_count == 1);
}

TEST_CASE("NativeViewHost re-attaches when the handle changes",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({0, 0, 50, 50});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));
    nvh->update_native_layout();
    REQUIRE(host.rec().attach_count == 1);

    int other = 0;
    nvh->set_native_child(&other);  // detaches old handle
    REQUIRE(host.rec().detach_count == 1);
    nvh->update_native_layout();    // attaches the new one
    REQUIRE(host.rec().attach_count == 2);
    REQUIRE(host.rec().child == &other);

    nvh->clear_native_child();
    REQUIRE(host.rec().detach_count == 2);
    REQUIRE_FALSE(nvh->is_native_attached());
}

TEST_CASE("NativeViewHost stays unattached on an unsupported host but computes geometry",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 400, 400});
    RecordingPluginHost host(/*support=*/false);  // attach returns false
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({10, 20, 100, 100});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE_FALSE(nvh->is_native_attached());
    REQUIRE(host.rec().attach_count == 0);
    // Geometry is still computed (introspection stays honest) and the subtree is
    // flagged as containing a native overlay so headless capture can react.
    REQUIRE(nvh->computed_child_frame() == Rect{10, 20, 100, 100});
    REQUIRE(nvh->contains_native_overlay());
}

TEST_CASE("NativeViewHost hides its native child when made invisible",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({0, 0, 50, 50});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());

    // Hiding the widget detaches the OS child (it would otherwise linger above
    // the GPU layer).
    nvh->set_visible(false);
    nvh->update_native_layout();
    REQUIRE_FALSE(nvh->is_native_attached());
    REQUIRE(host.rec().detach_count == 1);

    // Showing it again re-attaches.
    nvh->set_visible(true);
    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());
    REQUIRE(host.rec().attach_count == 2);
}

TEST_CASE("NativeViewHost attaches + positions via paint_all",
          "[view][native-view-host]") {
    // paint_all is the real per-frame driver (update_native_layout is the
    // testable seam it calls). Drive it through a RecordingCanvas to cover the
    // paint path end to end.
    View root;
    root.set_bounds({0, 0, 200, 200});
    RecordingPluginHost host;
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({20, 30, 80, 60});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    pulp::canvas::RecordingCanvas rc;
    root.paint_all(rc);
    REQUIRE(nvh->is_native_attached());
    REQUIRE(host.rec().bounds == Rect{20, 30, 80, 60});
}

TEST_CASE("NativeViewHost embeds through a WindowHost too",
          "[view][native-view-host]") {
    View root;
    root.set_bounds({0, 0, 300, 300});
    RecordingWindowHost host;
    root.set_window_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({25, 35, 60, 40});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());
    REQUIRE(host.rec().attach_count == 1);
    REQUIRE(host.rec().bounds == Rect{25, 35, 60, 40});
}

TEST_CASE("NativeViewHost forwards headless capture to its snapshot callback",
          "[view][native-view-host]") {
    NativeViewHost nvh;
    REQUIRE_FALSE(nvh.contains_native_overlay());
    // No snapshot yet → capture honestly returns empty.
    REQUIRE(nvh.capture_native_overlay_png(10, 10).empty());

    std::vector<uint8_t> fake_png = {0x89, 'P', 'N', 'G'};
    uint32_t seen_w = 0, seen_h = 0;
    nvh.set_native_child(fake_handle(),
                         [&](uint32_t w, uint32_t h) {
                             seen_w = w;
                             seen_h = h;
                             return fake_png;
                         });
    REQUIRE(nvh.contains_native_overlay());
    auto png = nvh.capture_native_overlay_png(64, 48);
    REQUIRE(png == fake_png);
    REQUIRE(seen_w == 64);
    REQUIRE(seen_h == 48);

    nvh.clear_native_child();
    REQUIRE_FALSE(nvh.contains_native_overlay());
}

// ── G1: design-viewport transform for native children ────────────────────────
// Under an active design viewport the host paints Pulp widgets letterbox-scaled;
// the embedded native child's frame must go through the SAME transform so a
// mixed Pulp+native tree stays pixel-aligned. Introspection stays design-space;
// computed_child_frame_host() and the pushed host coords are transformed.

TEST_CASE("NativeViewHost identity when no viewport is active",
          "[view][native-view-host][viewport]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    RecordingPluginHost host;  // no viewport installed → identity
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({40, 30, 120, 80});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());
    // Design-space and host-space frames coincide; pushed coords are raw.
    REQUIRE(nvh->computed_child_frame() == Rect{40, 30, 120, 80});
    REQUIRE(nvh->computed_child_frame_host() == Rect{40, 30, 120, 80});
    REQUIRE(host.rec().bounds == Rect{40, 30, 120, 80});
}

TEST_CASE("NativeViewHost pushes a pillarboxed frame under an active viewport",
          "[view][native-view-host][viewport]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    RecordingPluginHost host;
    // Pillarbox: half scale, horizontal offset, no vertical offset.
    host.set_viewport(/*sx=*/0.5f, /*sy=*/0.5f, /*tx=*/100.0f, /*ty=*/0.0f);
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({40, 30, 120, 80});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_native_attached());
    // Introspection stays in design space (the pure layout frame).
    REQUIRE(nvh->computed_child_frame() == Rect{40, 30, 120, 80});
    // Host space = design frame through x'=x*0.5+100, y'=y*0.5, w'=w*0.5, h'=h*0.5.
    REQUIRE(nvh->computed_child_frame_host() == Rect{120, 15, 60, 40});
    // The value pushed to the host (the OS view's real frame) is host-space.
    REQUIRE(host.rec().bounds == Rect{120, 15, 60, 40});
    // Attach itself used the transformed frame (single attach, no drift).
    REQUIRE(host.rec().attach_count == 1);
}

TEST_CASE("NativeViewHost pushes a letterboxed (top-slack) frame under a viewport",
          "[view][native-view-host][viewport]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    RecordingPluginHost host;
    // Letterbox: half scale, vertical offset (bars top+bottom), no horizontal.
    host.set_viewport(0.5f, 0.5f, 0.0f, 60.0f);
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({20, 40, 100, 60});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(nvh->computed_child_frame() == Rect{20, 40, 100, 60});
    // x'=20*0.5=10, y'=40*0.5+60=80, w'=50, h'=30.
    REQUIRE(nvh->computed_child_frame_host() == Rect{10, 80, 50, 30});
    REQUIRE(host.rec().bounds == Rect{10, 80, 50, 30});
}

TEST_CASE("NativeViewHost scales the clip rect too under a viewport",
          "[view][native-view-host][viewport]") {
    // A clipped child: the visible sub-rect (in the child's own box) must scale
    // by the same factor as the frame, or the mask would be wrong on the scaled
    // OS view.
    View root;
    root.set_bounds({0, 0, 400, 400});
    RecordingPluginHost host;
    host.set_viewport(0.5f, 0.5f, 0.0f, 0.0f);  // pure half-scale
    root.set_plugin_view_host(&host);

    auto clip_owned = std::make_unique<View>();
    auto* clip = clip_owned.get();
    clip->set_bounds({0, 0, 100, 100});
    clip->set_overflow(View::Overflow::hidden);
    root.add_child(std::move(clip_owned));

    auto nvh_owned = std::make_unique<NativeViewHost>();
    auto* nvh = nvh_owned.get();
    nvh->set_bounds({50, 50, 100, 100});  // spills past the 100x100 clip
    nvh->set_native_child(fake_handle());
    clip->add_child(std::move(nvh_owned));

    nvh->update_native_layout();
    REQUIRE(nvh->is_clipped());
    // Design space: frame (50,50,100,100), visible (50,50,50,50).
    REQUIRE(nvh->computed_child_frame() == Rect{50, 50, 100, 100});
    REQUIRE(nvh->computed_visible_rect() == Rect{50, 50, 50, 50});
    // Host space: frame (25,25,50,50). Clip in the child's own box scales from
    // design (0,0,50,50) → host (0,0,25,25).
    REQUIRE(nvh->computed_child_frame_host() == Rect{25, 25, 50, 50});
    REQUIRE(host.rec().bounds == Rect{25, 25, 50, 50});
    REQUIRE(host.rec().has_clip);
    REQUIRE(host.rec().clip == Rect{0, 0, 25, 25});
}

TEST_CASE("NativeViewHost re-pushes on host resize with unchanged design layout",
          "[view][native-view-host][viewport]") {
    // A host resize changes the viewport scale/translate while the design-space
    // layout is untouched. The pushed-geometry cache stores HOST-space coords,
    // so the changed transform must invalidate it and re-push — otherwise the
    // native child would keep its pre-resize frame and drift.
    View root;
    root.set_bounds({0, 0, 400, 300});
    RecordingPluginHost host;
    host.set_viewport(1.0f, 1.0f, 0.0f, 0.0f);  // initial: identity-ish
    root.set_plugin_view_host(&host);

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_bounds({40, 30, 120, 80});
    nvh->set_native_child(fake_handle());
    root.add_child(std::move(owned));

    nvh->update_native_layout();
    REQUIRE(host.rec().bounds == Rect{40, 30, 120, 80});
    const int bounds_after_attach = host.rec().bounds_count;

    // Re-pump with the SAME design layout and SAME viewport → cache hit, no
    // redundant re-push.
    nvh->update_native_layout();
    REQUIRE(host.rec().bounds_count == bounds_after_attach);

    // Now the host "resizes": viewport scale + offset change. Design-space
    // layout (the widget bounds) is unchanged.
    host.set_viewport(0.5f, 0.5f, 100.0f, 60.0f);
    nvh->update_native_layout();
    // Cache invalidated by the new host-space frame → re-pushed.
    REQUIRE(host.rec().bounds_count == bounds_after_attach + 1);
    REQUIRE(nvh->computed_child_frame() == Rect{40, 30, 120, 80});  // design unchanged
    REQUIRE(host.rec().bounds == Rect{120, 75, 60, 40});  // 40*.5+100, 30*.5+60, 120*.5, 80*.5
}

// ── The host outliving nothing: a host destroyed BEFORE its view tree ────────
//
// A NativeViewHost keeps a RAW, non-owning pointer to the host it attached to and
// dereferences it in its destructor to detach. Nothing made that safe. Every
// built-in Apple host nulls the back-reference in its own destructor, so no
// shipped host dangles today — but that is a per-subclass convention, and
// PluginViewHost::set_factory / WindowHost::set_factory are PUBLIC API. A
// downstream host that embeds native children and forgets that one line got a
// use-after-free on editor close.
//
// The base host destructor now clears every still-attached view's back-pointer,
// which makes the invariant structural. These two cases are the proof: destroy
// the host first, with the view tree still alive, and the view must survive it —
// reporting itself detached rather than dereferencing a dead host.
//
// Under ASan (the sanitizer lane) a regression here is a hard stack-use-after-scope
// abort, not a soft assertion failure — which is exactly what it was doing before.

TEST_CASE("NativeViewHost survives a plugin host destroyed before the view tree",
          "[view][native-view-host][lifetime]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_native_child(reinterpret_cast<NativeViewHandle>(0x1234));
    nvh->set_bounds({10, 10, 100, 50});
    root.add_child(std::move(owned));

    {
        RecordingPluginHost host;
        root.set_plugin_view_host(&host);
        nvh->update_native_layout();
        REQUIRE(nvh->is_native_attached());
    }   // host dies HERE, while root (and nvh) are still very much alive

    // The base destructor cleared our back-pointer, so we are no longer attached
    // and hold nothing to dereference.
    REQUIRE_FALSE(nvh->is_native_attached());

    // And a detach now is a no-op rather than a deref of a dead host. Before the
    // fix this line — reached via ~NativeViewHost at scope end — is the crash.
    root.set_plugin_view_host(nullptr);
    REQUIRE_FALSE(nvh->is_native_attached());
}

TEST_CASE("NativeViewHost survives a window host destroyed before the view tree",
          "[view][native-view-host][lifetime]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto owned = std::make_unique<NativeViewHost>();
    auto* nvh = owned.get();
    nvh->set_native_child(reinterpret_cast<NativeViewHandle>(0x5678));
    nvh->set_bounds({20, 20, 80, 40});
    root.add_child(std::move(owned));

    {
        RecordingWindowHost host;
        root.set_window_host(&host);
        nvh->update_native_layout();
        REQUIRE(nvh->is_native_attached());
    }   // host dies first

    REQUIRE_FALSE(nvh->is_native_attached());
    root.set_window_host(nullptr);
    REQUIRE_FALSE(nvh->is_native_attached());
}
