#include <pulp/view/design_import.hpp>
#include <pulp/view/widgets.hpp>

namespace pulp::view {
namespace {
void bind_legacy(NativeImportBindingContext& ctx, View& view,
                 const NativeImportMeterBindingDescriptor* meter,
                 const NativeImportWaveformBindingDescriptor* waveform) {
    if (meter) {
        if (auto* concrete = dynamic_cast<Meter*>(&view))
            ctx.bind_meter(*concrete, *meter);
    } else if (waveform) {
        if (auto* concrete = dynamic_cast<WaveformView*>(&view))
            ctx.bind_waveform_display(*concrete, *waveform);
    }
}
} // namespace
void register_audio_import_binders() {
    register_legacy_import_binder("pulp.level", &bind_legacy);
    register_legacy_import_binder("pulp.series", &bind_legacy);
}
} // namespace pulp::view
