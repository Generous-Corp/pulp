#include <pulp/vellum_audio/audio_display_provider.hpp>
#include <pulp/vellum_audio/audio_widgets.hpp>

#include <vellum/view/data_display.hpp>

#include <cstddef>
#include <memory>

namespace pulp::vellum_audio {
namespace {

::vellum::value::ValueFeed* registered_feed = nullptr;

std::unique_ptr<::vellum::view::View> make_level() {
    auto meter = std::make_unique<LevelMeter>(registered_feed);
    meter->set_channel(0);
    return meter;
}

std::unique_ptr<::vellum::view::View> make_scope() {
    auto scope = std::make_unique<AnalyzerScope>(registered_feed);
    if (registered_feed != nullptr && registered_feed->channel_count() > 1)
        scope->set_channel(1);
    return scope;
}

} // namespace

void register_audio_widgets(::vellum::value::ValueFeed* feed) {
    registered_feed = feed;
    ::vellum::view::register_data_display({
        "pulp.level", &make_level,
        {{"pulp.level", "pulp::vellum_audio::LevelMeter",
          "pulp/vellum_audio/audio_widgets.hpp"}}});
    ::vellum::view::register_data_display({
        "pulp.bins", &make_scope,
        {{"pulp.bins", "pulp::vellum_audio::AnalyzerScope",
          "pulp/vellum_audio/audio_widgets.hpp"}}});
}

void sample_audio_widgets(::vellum::view::View& root) {
    if (auto* meter = dynamic_cast<LevelMeter*>(&root))
        meter->sample();
    if (auto* scope = dynamic_cast<AnalyzerScope*>(&root))
        scope->sample();
    for (std::size_t i = 0; i < root.child_count(); ++i)
        sample_audio_widgets(*root.child_at(i));
}

} // namespace pulp::vellum_audio
