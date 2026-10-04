#pragma once

/// @file frame_cost_probe.hpp
/// Headless per-frame cost accounting for an animated view tree.
///
/// "Is this animation cheap?" has three answers a wall clock cannot give on its
/// own, and each one is a separate way an animated control goes wrong:
///
///   * DAMAGE -- did the frame ask to repaint only what moved, or the whole
///     surface? A control that calls the rect-less View::request_repaint() on
///     its value path re-composites every static pixel of the editor each
///     frame, and no partial-repaint host can ever engage.
///   * LAYOUT -- did the frame run a layout pass? A value tick that writes a
///     size or position re-lays the tree at the display rate.
///   * TIME -- how long the frame's own work took, as p50 / p95 / max. The
///     mean hides exactly the frames that drop.
///
/// FrameCostProbe attaches a recording PluginViewHost to the root for its
/// lifetime and measures each frame the caller drives (`measure(fn)`): the
/// damage the frame requested (bounded rect union, or whole surface), the
/// layout passes it ran (View::layout_pass_count) and its wall time.
/// `check()` holds a Summary to a Budget -- optionally relative to a baseline
/// Summary of the same tree without the animation -- and returns one line per
/// breach, so a test asserts the list is empty and prints it when it is not.
///
/// Damage is classified per REQUEST, not per frame: the recorder takes the
/// pending damage on every repaint() call, and View routes a bounded request
/// through mark_dirty_region() (rect already in the damage) and a rect-less
/// one through mark_dirty_full(). So a frame mixing both is reported full,
/// which is what a partial-repaint host would have to paint.
///
/// Headless only: the root must not have a WindowHost (its damage would go
/// there instead). Not thread-safe; drive it from the thread that owns the
/// tree. Pair every passing budget with a negative control that must fail it
/// (a plant that forces full invalidation) -- a probe that cannot see damage
/// passes every budget.

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

class FrameCostProbe {
  public:
    struct Frame {
        double ms = 0.0;                 ///< wall time of the frame's work
        bool full_damage = false;        ///< some request repainted the whole surface
        bool has_bounds = false;         ///< bounded damage was requested
        Rect damage{};                   ///< union of the bounded requests
        int repaint_requests = 0;        ///< damage-marking requests the frame caused
        int frame_requests = 0;          ///< frames requested without damage
        std::uint64_t layout_passes = 0; ///< layout_children() calls the frame ran
    };

    struct Summary {
        int frames = 0;
        double p50_ms = 0.0, p95_ms = 0.0, max_ms = 0.0;
        int full_damage_frames = 0;    ///< frames with any whole-surface request
        int layout_frames = 0;         ///< frames that ran at least one layout pass
        int painted_frames = 0;        ///< frames that requested any repaint at all
        double mean_damage_area = 0.0; ///< mean bounded area over painted frames
        Rect damage_union{};           ///< union of every frame's bounded damage
    };

    struct Budget {
        /// Absolute p95 ceiling, or (with a baseline) the most the p95 may grow
        /// over the baseline's p95.
        double max_p95_ms = std::numeric_limits<double>::infinity();
        int max_full_damage_frames = 0;
        int max_layout_frames = 0;
        double max_mean_damage_area = std::numeric_limits<double>::infinity();
        /// Positive control: a run that painted fewer frames than this measured
        /// nothing, and is reported as a breach rather than a pass.
        int min_painted_frames = 1;
    };

    /// Attach a recording host to @p root for the probe's lifetime.
    /// @p surface is the size the host reports (the editor's logical size).
    explicit FrameCostProbe(View& root, PluginViewHost::Size surface = {})
        : root_(root), recorder_(surface) {
        if (root.window_host() != nullptr)
            throw std::logic_error(
                "FrameCostProbe: the root has a WindowHost; its damage would not "
                "reach the probe");
        previous_ = root.plugin_view_host();
        root.set_plugin_view_host(&recorder_);
        // Attaching is not a frame: settle the layout it may have dirtied and
        // start from no damage (PendingDamage starts full, by design, for a
        // real host's first frame).
        root.layout_children_if_needed();
        recorder_.prime();
    }
    ~FrameCostProbe() {
        root_.set_plugin_view_host(previous_);
    }
    FrameCostProbe(const FrameCostProbe&) = delete;
    FrameCostProbe& operator=(const FrameCostProbe&) = delete;

    /// Run one frame's work and record what it cost.
    template <typename Fn> const Frame& measure(Fn&& frame) {
        recorder_.reset();
        const auto layouts = View::layout_pass_count();
        const auto t0 = std::chrono::steady_clock::now();
        std::forward<Fn>(frame)();
        const auto t1 = std::chrono::steady_clock::now();
        Frame f;
        f.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        f.full_damage = recorder_.full > 0;
        f.has_bounds = recorder_.have;
        f.damage = recorder_.uni;
        f.repaint_requests = recorder_.full + recorder_.bounded;
        f.frame_requests = recorder_.frames_only;
        f.layout_passes = View::layout_pass_count() - layouts;
        frames_.push_back(f);
        return frames_.back();
    }

    const std::vector<Frame>& frames() const {
        return frames_;
    }
    void clear() {
        frames_.clear();
    }

    Summary summary() const {
        Summary s;
        s.frames = static_cast<int>(frames_.size());
        if (frames_.empty())
            return s;
        std::vector<double> ms;
        ms.reserve(frames_.size());
        double area = 0.0;
        int area_frames = 0;
        bool have_union = false;
        for (const auto& f : frames_) {
            ms.push_back(f.ms);
            if (f.full_damage)
                ++s.full_damage_frames;
            if (f.layout_passes > 0)
                ++s.layout_frames;
            if (f.repaint_requests > 0)
                ++s.painted_frames;
            if (f.has_bounds) {
                area += static_cast<double>(f.damage.width) * f.damage.height;
                ++area_frames;
                s.damage_union = have_union ? unite(s.damage_union, f.damage) : f.damage;
                have_union = true;
            }
        }
        std::sort(ms.begin(), ms.end());
        s.p50_ms = ms[ms.size() / 2];
        s.p95_ms = ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
        s.max_ms = ms.back();
        s.mean_damage_area = area_frames > 0 ? area / area_frames : 0.0;
        return s;
    }

    /// One human-readable line per breach of @p budget; empty when within it.
    static std::vector<std::string> check(const Summary& s, const Budget& budget,
                                          const Summary* baseline = nullptr) {
        std::vector<std::string> breaches;
        if (s.painted_frames < budget.min_painted_frames)
            breaches.push_back("positive control: only " + std::to_string(s.painted_frames) +
                               " frames painted (need " +
                               std::to_string(budget.min_painted_frames) +
                               "); the run measured nothing");
        if (s.full_damage_frames > budget.max_full_damage_frames)
            breaches.push_back(std::to_string(s.full_damage_frames) + "/" +
                               std::to_string(s.frames) +
                               " frames requested a whole-surface repaint");
        if (s.layout_frames > budget.max_layout_frames)
            breaches.push_back(std::to_string(s.layout_frames) + "/" + std::to_string(s.frames) +
                               " frames ran a layout pass");
        if (s.mean_damage_area > budget.max_mean_damage_area)
            breaches.push_back("mean bounded damage " + std::to_string(s.mean_damage_area) +
                               " px2 exceeds " + std::to_string(budget.max_mean_damage_area));
        const double grown = baseline ? s.p95_ms - baseline->p95_ms : s.p95_ms;
        if (grown > budget.max_p95_ms)
            breaches.push_back(std::string(baseline ? "p95 grew " : "p95 ") +
                               std::to_string(grown) + " ms (budget " +
                               std::to_string(budget.max_p95_ms) + " ms)");
        return breaches;
    }

  private:
    static Rect unite(const Rect& a, const Rect& b) {
        const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        return {x0, y0, std::max(a.x + a.width, b.x + b.width) - x0,
                std::max(a.y + a.height, b.y + b.height) - y0};
    }

    class Recorder final : public PluginViewHost {
      public:
        explicit Recorder(Size size) : size_(size) {}
        int full = 0, bounded = 0, frames_only = 0;
        // A frame asked for without damage (View::request_frame()) is not a
        // repaint request: whatever that frame changes marks its own.
        void request_frame() override { ++frames_only; }
        bool have = false;
        Rect uni{};
        NativeViewHandle native_handle() override {
            return {};
        }
        void attach_to_parent(NativeViewHandle) override {}
        void detach() override {}
        // Every request ends in exactly one repaint() call, so taking the
        // damage here classifies each request on its own: a bounded mark
        // arrives with its rect in the damage, a whole-surface one as full.
        void repaint() override {
            const auto snap = damage_.take();
            if (snap.is_bounded()) {
                ++bounded;
                uni = have ? unite(uni, snap.bounds()) : snap.bounds();
                have = true;
            } else {
                ++full;
            }
        }
        void set_size(std::uint32_t w, std::uint32_t h) override {
            size_ = {w, h};
        }
        Size get_size() const override {
            return size_;
        }
        void prime() {
            damage_.clear();
            reset();
        }
        void reset() {
            full = bounded = frames_only = 0;
            have = false;
            uni = {};
        }

      private:
        Size size_;
    };

    View& root_;
    Recorder recorder_;
    PluginViewHost* previous_ = nullptr;
    std::vector<Frame> frames_;
};

} // namespace pulp::view
