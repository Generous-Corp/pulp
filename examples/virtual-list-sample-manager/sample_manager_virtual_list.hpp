#pragma once

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/virtual_list.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::examples::virtual_list_sample_manager {

constexpr uint32_t kWidth = 720;
constexpr uint32_t kHeight = 420;
constexpr std::size_t kSampleCount = 12000;
constexpr float kRowHeight = 38.0f;

struct SampleManagerState {
    std::size_t bind_calls = 0;
    std::vector<std::size_t> bound_indices;
};

struct SampleEntry {
    std::string name;
    std::string meta;
    std::string tag;
    bool favorite = false;
};

inline SampleEntry sample_at(std::size_t index) {
    static constexpr const char* kTags[] = {
        "Kick", "Snare", "Hat", "Loop", "Vox", "FX", "Bass", "Perc"
    };
    SampleEntry e;
    e.name = "Sample " + std::to_string(index + 1);
    e.meta = std::to_string(40 + (index * 7) % 180) + " BPM  " +
             std::to_string(1 + (index % 4)) + " ch";
    e.tag = kTags[index % std::size(kTags)];
    e.favorite = (index % 11) == 0;
    return e;
}

class MiniWaveformView final : public view::View {
public:
    void set_seed(std::size_t seed) {
        seed_ = seed;
        request_repaint();
    }

    void paint(canvas::Canvas& canvas) override {
        const auto b = local_bounds();
        canvas.set_fill_color(canvas::Color::rgba8(20, 24, 30));
        canvas.fill_rounded_rect(0, 0, b.width, b.height, 5.0f);
        canvas.set_fill_color(canvas::Color::rgba8(86, 190, 186, 210));
        const int bars = 28;
        const float gap = 1.5f;
        const float bar_w = std::max(1.0f, (b.width - gap * (bars - 1)) / bars);
        for (int i = 0; i < bars; ++i) {
            const float phase = static_cast<float>((seed_ * 17 + i * 23) % 97) / 97.0f;
            const float h = 3.0f + phase * std::max(1.0f, b.height - 5.0f);
            const float x = static_cast<float>(i) * (bar_w + gap);
            canvas.fill_rounded_rect(x, (b.height - h) * 0.5f, bar_w, h, 1.0f);
        }
    }

private:
    std::size_t seed_ = 0;
};

class SampleRowView final : public view::View {
public:
    SampleRowView() {
        set_overflow(Overflow::hidden);
        set_access_role(AccessRole::group);

        auto fav = std::make_unique<view::Label>();
        favorite_ = fav.get();
        favorite_->set_text_color(canvas::Color::rgba8(245, 184, 73));
        favorite_->set_font_size(13.0f);
        add_child(std::move(fav));

        auto name = std::make_unique<view::Label>();
        name_ = name.get();
        name_->set_text_color(canvas::Color::rgba8(236, 240, 245));
        name_->set_font_size(13.0f);
        add_child(std::move(name));

        auto meta = std::make_unique<view::Label>();
        meta_ = meta.get();
        meta_->set_text_color(canvas::Color::rgba8(148, 158, 172));
        meta_->set_font_size(10.5f);
        add_child(std::move(meta));

        auto wave = std::make_unique<MiniWaveformView>();
        waveform_ = wave.get();
        add_child(std::move(wave));

        auto tag = std::make_unique<view::Label>();
        tag_ = tag.get();
        tag_->set_text_color(canvas::Color::rgba8(204, 228, 226));
        tag_->set_font_size(10.5f);
        add_child(std::move(tag));

        auto preview = std::make_unique<view::Label>("Preview");
        preview_ = preview.get();
        preview_->set_text_color(canvas::Color::rgba8(36, 48, 58));
        preview_->set_font_size(10.5f);
        add_child(std::move(preview));
    }

    void bind(std::size_t index, bool selected) {
        bound_index = index;
        selected_ = selected;
        const auto entry = sample_at(index);
        favorite_->set_text(entry.favorite ? "*" : "");
        name_->set_text(entry.name);
        meta_->set_text(entry.meta);
        tag_->set_text(entry.tag);
        waveform_->set_seed(index);
        set_access_label(entry.name + ", " + entry.tag);
        request_repaint();
    }

    void layout_children() override {
        const auto b = local_bounds();
        favorite_->set_bounds({10.0f, 9.0f, 18.0f, 20.0f});
        name_->set_bounds({36.0f, 5.0f, 210.0f, 16.0f});
        meta_->set_bounds({36.0f, 21.0f, 210.0f, 13.0f});
        waveform_->set_bounds({270.0f, 9.0f, std::max(72.0f, b.width - 500.0f), 20.0f});
        tag_->set_bounds({std::max(360.0f, b.width - 210.0f), 10.0f, 68.0f, 18.0f});
        preview_->set_bounds({std::max(438.0f, b.width - 122.0f), 10.0f, 66.0f, 18.0f});
        clear_layout_dirty();
    }

    void paint(canvas::Canvas& canvas) override {
        const auto b = local_bounds();
        const bool even = (bound_index % 2) == 0;
        canvas.set_fill_color(selected_ ? canvas::Color::rgba8(57, 96, 118)
                                        : (even ? canvas::Color::rgba8(30, 35, 43)
                                                : canvas::Color::rgba8(25, 30, 37)));
        canvas.fill_rect(0, 0, b.width, b.height - 1.0f);
        if (selected_) {
            canvas.set_fill_color(canvas::Color::rgba8(86, 190, 186));
            canvas.fill_rounded_rect(0, 5.0f, 3.0f, b.height - 10.0f, 1.5f);
        }
        canvas.set_fill_color(canvas::Color::rgba8(45, 52, 62));
        canvas.fill_rect(0, b.height - 1.0f, b.width, 1.0f);
        canvas.set_fill_color(canvas::Color::rgba8(113, 216, 189));
        canvas.fill_rounded_rect(std::max(438.0f, b.width - 122.0f), 9.0f, 66.0f, 20.0f, 5.0f);
    }

    std::size_t bound_index = 0;

private:
    view::Label* favorite_ = nullptr;
    view::Label* name_ = nullptr;
    view::Label* meta_ = nullptr;
    MiniWaveformView* waveform_ = nullptr;
    view::Label* tag_ = nullptr;
    view::Label* preview_ = nullptr;
    bool selected_ = false;
};

class SampleManagerView final : public view::View {
public:
    explicit SampleManagerView(std::shared_ptr<SampleManagerState> state,
                               std::size_t row_count = kSampleCount)
        : state_(std::move(state)) {
        set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
        set_overflow(Overflow::hidden);

        auto title = std::make_unique<view::Label>("Sample Manager");
        title_ = title.get();
        title_->set_font_size(19.0f);
        title_->set_text_color(canvas::Color::rgba8(248, 250, 252));
        add_child(std::move(title));

        auto summary = std::make_unique<view::Label>(
            std::to_string(row_count) + " indexed samples");
        summary_ = summary.get();
        summary_->set_font_size(12.0f);
        summary_->set_text_color(canvas::Color::rgba8(154, 168, 180));
        add_child(std::move(summary));

        auto list = std::make_unique<view::VirtualList>();
        list_ = list.get();
        list_->set_row_height(kRowHeight);
        list_->set_overscan(4);
        list_->set_selection_mode(view::VirtualList::SelectionMode::multi);
        list_->set_row_factory([](std::size_t) {
            return std::make_unique<SampleRowView>();
        });
        list_->set_row_binder([this](view::View& row, std::size_t index) {
            if (state_) {
                ++state_->bind_calls;
                state_->bound_indices.push_back(index);
            }
            if (auto* sample_row = dynamic_cast<SampleRowView*>(&row)) {
                sample_row->bind(index, list_->is_selected(index));
            }
        });
        add_child(std::move(list));
        list_->set_row_count(row_count);
    }

    view::VirtualList& list() { return *list_; }
    const view::VirtualList& list() const { return *list_; }

    void layout_children() override {
        const auto b = local_bounds();
        title_->set_bounds({16.0f, 14.0f, 260.0f, 26.0f});
        summary_->set_bounds({std::max(280.0f, b.width - 220.0f), 18.0f, 200.0f, 18.0f});
        list_->set_bounds({16.0f, 52.0f, b.width - 32.0f, b.height - 68.0f});
        list_->layout_children();
        clear_layout_dirty();
    }

    void paint(canvas::Canvas& canvas) override {
        const auto b = local_bounds();
        canvas.set_fill_color(canvas::Color::rgba8(16, 19, 24));
        canvas.fill_rect(0, 0, b.width, b.height);
        canvas.set_fill_color(canvas::Color::rgba8(35, 42, 52));
        canvas.fill_rounded_rect(12.0f, 48.0f, b.width - 24.0f, b.height - 60.0f, 7.0f);
    }

private:
    std::shared_ptr<SampleManagerState> state_;
    view::Label* title_ = nullptr;
    view::Label* summary_ = nullptr;
    view::VirtualList* list_ = nullptr;
};

struct SampleManagerFixture {
    std::unique_ptr<SampleManagerView> root;
    std::shared_ptr<SampleManagerState> state;
};

inline SampleManagerFixture build_sample_manager(std::size_t row_count = kSampleCount) {
    auto state = std::make_shared<SampleManagerState>();
    auto root = std::make_unique<SampleManagerView>(state, row_count);
    return {std::move(root), std::move(state)};
}

}  // namespace pulp::examples::virtual_list_sample_manager
