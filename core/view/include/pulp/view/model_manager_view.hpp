#pragma once

// ModelManagerView — the "you need a model" manager UI. Lists available +
// installed models with their status, shows per-row download progress + cancel, and
// exposes Download / Resume / Set-default / Remove actions via callbacks the host wires
// to runtime::install_model / activate_model / remove_model. A header "Done" closes back
// to the editor when there is one. Pure composition over Pulp widgets (View flex + Label
// + ToggleButton) so it renders on the Skia/GPU canvas and is headless-capturable.
//
// Layout: each model is a two-line block — line 1 is the name + status + action
// buttons; line 2 is a full-width progress bar (only while downloading/paused) — so the
// bar never crowds or overlaps the name.

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/buttons.hpp>  // TextButton (centered, momentary action buttons)
#include <pulp/runtime/model_store.hpp>
#include <pulp/canvas/canvas.hpp>

#include <functional>
#include <map>
#include <string>

namespace pulp::view {

class ModelManagerView : public View {
public:
    using ModelAction = std::function<void(const std::string& model_id)>;

    ModelManagerView() {
        flex().direction = FlexDirection::column;
        flex().padding = 20.0f;
        flex().gap = 12.0f;
        set_background_color(canvas::Color::rgba8(32, 33, 36, 255));  // GREY_900-ish
        rebuild();
    }

    /// Populate from runtime::list_models(registry, subsystem).
    void set_models(const runtime::ModelListResult& models) {
        models_ = models;
        rebuild();
    }

    /// Per-row download progress in [0,1]. Negative clears (not downloading).
    /// NOTE: this rebuilds the full row subtree (Yoga layout + Skia repaint), so
    /// callers driving live download progress should throttle updates (e.g. ~4 Hz)
    /// rather than calling it at the raw byte-callback rate.
    void set_download_progress(const std::string& model_id, float fraction) {
        if (fraction < 0.0f)
            progress_.erase(model_id);
        else
            progress_[model_id] = fraction;
        rebuild();
    }

    /// Show the header "Done" (return to the editor). Off when there is no editor yet.
    void set_can_close(bool can_close) {
        if (can_close_ == can_close) return;
        can_close_ = can_close;
        rebuild();
    }

    ModelAction on_download;          ///< Download / Resume
    ModelAction on_activate;          ///< Set default
    ModelAction on_remove;            ///< Remove / Delete
    ModelAction on_cancel;            ///< Cancel (mid-download)
    std::function<void()> on_done;    ///< Done (return to the editor)

private:
    static constexpr canvas::Color kMuted() { return canvas::Color::rgba8(170, 170, 175, 255); }
    static constexpr canvas::Color kWhite() { return canvas::Color::rgba8(235, 235, 240, 255); }
    static constexpr canvas::Color kTeal()  { return canvas::Color::rgba8(132, 243, 237, 255); }
    static constexpr canvas::Color kBlue()  { return canvas::Color::rgba8(127, 178, 255, 255); }

    static std::unique_ptr<Label> make_label(const std::string& text, float size, canvas::Color color,
                                             float width = 0.0f) {
        auto l = std::make_unique<Label>(text);
        l->set_font_size(size);
        l->set_text_color(color);
        if (width > 0.0f) l->flex().preferred_width = width;
        return l;
    }

    std::unique_ptr<TextButton> make_action(const std::string& label, const std::string& id,
                                            ModelAction ModelManagerView::* member) {
        // Momentary TextButton (centered label, proper hover/press) rather than a latched
        // ToggleButton — these are one-shot actions, and the label should read centered.
        auto b = std::make_unique<TextButton>(label);
        b->flex().preferred_width = 96.0f;
        b->flex().preferred_height = 28.0f;
        b->on_click = [this, id, member] {
            if (this->*member) (this->*member)(id);
        };
        return b;
    }

    // Full-width determinate bar: a dark track with a constant-accent fill sized
    // proportionally via flex-grow (NOT the audio Meter, which colors by level).
    static std::unique_ptr<View> make_progress_bar(float fraction) {
        const float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        auto track = std::make_unique<View>();
        track->flex().direction = FlexDirection::row;
        track->flex().flex_grow = 1.0f;
        track->flex().preferred_height = 6.0f;
        track->set_background_color(canvas::Color::rgba8(55, 56, 60, 255));
        auto fill = std::make_unique<View>();
        fill->flex().flex_grow = f < 0.001f ? 0.001f : f;
        fill->set_background_color(canvas::Color::rgba8(127, 178, 255, 255));
        auto rest = std::make_unique<View>();
        rest->flex().flex_grow = (1.0f - f) < 0.001f ? 0.001f : (1.0f - f);
        track->add_child(std::move(fill));
        track->add_child(std::move(rest));
        return track;
    }

    void rebuild() {
        while (child_count() > 0) remove_child(child_at(0));

        // Header: title + (Done, right-aligned, only when there's an editor to return to).
        auto header = std::make_unique<View>();
        header->flex().direction = FlexDirection::row;
        header->flex().align_items = FlexAlign::center;
        header->flex().preferred_height = 30.0f;
        auto title = make_label("Models", 20.0f, kWhite());
        title->set_font_weight(700);
        title->flex().flex_grow = 1.0f;
        header->add_child(std::move(title));
        if (can_close_ && on_done) {
            auto done = std::make_unique<ToggleButton>();
            done->set_label("Done");
            done->flex().preferred_width = 88.0f;
            done->flex().preferred_height = 28.0f;
            done->on_toggle = [this](bool) { if (on_done) on_done(); };
            header->add_child(std::move(done));
        }
        add_child(std::move(header));

        add_child(make_label("Download a model to enable generation. Stored once, shared across plugins.",
                             12.0f, kMuted()));

        if (models_.models.empty()) {
            add_child(make_label("No models available yet.", 14.0f, kMuted()));
            return;
        }

        for (const auto& listed : models_.models) {
            const std::string id = listed.model.model_id;
            const auto it = progress_.find(id);
            const bool downloading = it != progress_.end();
            const bool paused = !downloading && listed.status == "partial";

            auto block = std::make_unique<View>();
            block->flex().direction = FlexDirection::column;
            block->flex().gap = 6.0f;

            // Line 1: name (grows, never shrinks) + status + action buttons.
            auto line = std::make_unique<View>();
            line->flex().direction = FlexDirection::row;
            line->flex().align_items = FlexAlign::center;
            line->flex().gap = 10.0f;
            line->flex().preferred_height = 30.0f;

            std::string name = listed.model.display_name.empty() ? id : listed.model.display_name;
            if (listed.model.is_recommended) name = "★ " + name;  // ★
            auto name_label = make_label(name, 14.0f, kWhite());
            name_label->flex().flex_grow = 1.0f;
            name_label->flex().flex_shrink = 0.0f;  // never let the name shrink/overlap
            line->add_child(std::move(name_label));

            if (downloading) {
                const int pct = static_cast<int>(it->second * 100.0f + 0.5f);
                line->add_child(make_label(std::to_string(pct) + "%", 12.0f, kTeal(), 44.0f));
                line->add_child(make_action("Cancel", id, &ModelManagerView::on_cancel));
            } else if (paused) {
                const float frac = listed.partial_fraction < 0.0f ? 0.0f : listed.partial_fraction;
                line->add_child(make_label("Paused " + std::to_string(static_cast<int>(frac * 100.0f + 0.5f)) + "%",
                                           12.0f, kMuted(), 90.0f));
                line->add_child(make_action("Resume", id, &ModelManagerView::on_download));
                line->add_child(make_action("Delete", id, &ModelManagerView::on_remove));
            } else if (listed.status == "installed") {
                line->add_child(make_label(listed.active ? "Active" : "Installed", 12.0f,
                                           listed.active ? kTeal() : kBlue(), 80.0f));
                if (!listed.active)
                    line->add_child(make_action("Set default", id, &ModelManagerView::on_activate));
                line->add_child(make_action("Remove", id, &ModelManagerView::on_remove));
            } else {
                line->add_child(make_label("Available", 12.0f, kMuted(), 80.0f));
                line->add_child(make_action("Download", id, &ModelManagerView::on_download));
            }
            block->add_child(std::move(line));

            // Line 2: a full-width progress bar, only while downloading or paused.
            if (downloading)
                block->add_child(make_progress_bar(it->second));
            else if (paused)
                block->add_child(make_progress_bar(listed.partial_fraction));

            add_child(std::move(block));
        }
    }

    runtime::ModelListResult models_;
    std::map<std::string, float> progress_;
    bool can_close_ = false;
};

}  // namespace pulp::view
