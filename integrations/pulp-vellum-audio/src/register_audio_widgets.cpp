#include <pulp/vellum_audio/audio_widgets.hpp>

#include <cstddef>
#include <memory>
#include <optional>

namespace pulp::vellum_audio {
namespace {

std::unique_ptr<::vellum::view::View> make_level() {
    return std::make_unique<LevelMeter>();
}

std::unique_ptr<::vellum::view::View> make_scope() {
    return std::make_unique<AnalyzerScope>();
}

} // namespace

void register_audio_widgets() {
    static const bool registered = [] {
        ::vellum::view::register_data_display({"pulp.level",
                                               &make_level,
                                               {{"pulp.level", "pulp::vellum_audio::LevelMeter",
                                                 "pulp/vellum_audio/audio_widgets.hpp"}}});
        ::vellum::view::register_data_display({"pulp.bins",
                                               &make_scope,
                                               {{"pulp.bins", "pulp::vellum_audio::AnalyzerScope",
                                                 "pulp/vellum_audio/audio_widgets.hpp"}}});
        return true;
    }();
    (void)registered;
}

void detail::AudioWidgetSampler::mount() {
    if (feed_ == nullptr)
        return;

    widgets_.clear();
    using Shape = ::vellum::value::ChannelShape;
    std::optional<std::size_t> meter_channel, scope_channel;
    for (std::size_t i = 0; i < feed_->channel_count(); ++i) {
        const auto shape = feed_->channel(i).shape;
        if (shape == Shape::meter && !meter_channel)
            meter_channel = i;
        if (shape == Shape::vector && !scope_channel)
            scope_channel = i;
        if (meter_channel && scope_channel)
            break;
    }

    const auto mount_widget = [&]<typename Display>(Display& display, Shape shape,
                                                   std::optional<std::size_t> fallback) {
        if (display.feed_ == nullptr) {
            const auto& id = display.id();
            const auto named = id.empty() ? std::nullopt : feed_->find(id, shape);
            if (const auto channel = named ? named : fallback)
                display.set_feed(feed_, *channel);
        }
        auto& widget = widgets_.emplace_back(Widget{{}, [](::vellum::view::View& view) noexcept {
            static_cast<Display&>(view).sample();
        }});
        widget.view.set(&display);
    };

    const auto visit = [&](const auto& self, ::vellum::view::View& view) -> void {
        if (auto* meter = dynamic_cast<LevelMeter*>(&view))
            mount_widget(*meter, Shape::meter, meter_channel);
        else if (auto* scope = dynamic_cast<AnalyzerScope*>(&view))
            mount_widget(*scope, Shape::vector, scope_channel);
        for (std::size_t i = 0; i < view.child_count(); ++i)
            self(self, *view.child_at(i));
    };
    visit(visit, root_);
    mounted_generation_ = root_.root_structure_generation();
}

void detail::AudioWidgetSampler::sample() {
    if (feed_ == nullptr)
        return;
    if (mounted_generation_ != root_.root_structure_generation())
        mount();
    for (const auto& widget : widgets_) {
        if (auto* view = widget.view.live_in(root_))
            widget.sample(*view);
    }
}

void sample_audio_widgets(::vellum::view::View& root, ::vellum::value::ValueFeed* feed) {
    detail::AudioWidgetSampler sampler(root, feed);
    sampler.mount();
    sampler.sample();
}

} // namespace pulp::vellum_audio
