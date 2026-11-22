#include <pulp/vellum_audio/audio_widgets.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

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

void sample_audio_widgets(::vellum::view::View& root, ::vellum::value::ValueFeed* feed) {
    if (feed == nullptr)
        return;

    using Shape = ::vellum::value::ChannelShape;
    std::optional<std::size_t> meter_channel, scope_channel;
    for (std::size_t i = 0; i < feed->channel_count(); ++i) {
        const auto shape = feed->channel(i).shape;
        if (shape == Shape::meter && !meter_channel)
            meter_channel = i;
        if (shape == Shape::vector && !scope_channel)
            scope_channel = i;
    }

    const auto find_channel = [&](std::string_view name, Shape shape,
                                  std::optional<std::size_t> fallback) {
        for (std::size_t i = 0; i < feed->channel_count(); ++i) {
            const auto channel = feed->channel(i);
            if (channel.shape == shape && channel.name == name)
                return std::optional<std::size_t>{i};
        }
        return fallback;
    };

    const auto visit = [&](const auto& self, ::vellum::view::View& view) -> void {
        if (auto* meter = dynamic_cast<LevelMeter*>(&view); meter) {
            if (meter->feed_ == nullptr)
                if (const auto channel = find_channel(view.id(), Shape::meter, meter_channel))
                    meter->set_feed(feed, *channel);
            meter->sample();
        }
        if (auto* scope = dynamic_cast<AnalyzerScope*>(&view); scope) {
            if (scope->feed_ == nullptr)
                if (const auto channel = find_channel(view.id(), Shape::vector, scope_channel))
                    scope->set_feed(feed, *channel);
            scope->sample();
        }
        for (std::size_t i = 0; i < view.child_count(); ++i)
            self(self, *view.child_at(i));
    };
    visit(visit, root);
}

} // namespace pulp::vellum_audio
