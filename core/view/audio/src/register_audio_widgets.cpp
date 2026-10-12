#include <pulp/view/data_display.hpp>
#include <pulp/view/register_audio_widgets.hpp>
#include <pulp/view/widgets.hpp>

namespace pulp::view {
#if defined(PULP_VIEW_AUDIO_HAS_DESIGN_IMPORT)
void register_audio_import_binders();
#endif
namespace {
std::unique_ptr<View> make_level() {
    return std::make_unique<Meter>();
}
std::unique_ptr<View> make_series() {
    return std::make_unique<WaveformView>();
}
std::unique_ptr<View> make_bins() {
    return std::make_unique<SpectrumView>();
}
} // namespace
void register_audio_widgets() {
#if defined(PULP_VIEW_AUDIO_HAS_DESIGN_IMPORT)
    register_audio_import_binders();
#endif
    register_data_display({"pulp.level",
                           &make_level,
                           {{"pulp.level", "pulp::view::Meter", "pulp/view/widgets.hpp"}}});
    register_data_display({"pulp.series",
                           &make_series,
                           {{"pulp.series", "pulp::view::WaveformView", "pulp/view/widgets.hpp"}}});
    register_data_display({"pulp.bins",
                           &make_bins,
                           {{"pulp.bins", "pulp::view::SpectrumView", "pulp/view/widgets.hpp"}}});
}
} // namespace pulp::view
