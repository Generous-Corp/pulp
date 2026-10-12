#include <pulp/view/design_import.hpp>
#include <pulp/view/widgets.hpp>

namespace pulp::view {
namespace {
// This binder exists to reach the deprecated concrete virtuals for contexts
// that still override them, so calling them here is intended.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
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
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
} // namespace
void register_audio_import_binders() {
    register_legacy_import_binder("pulp.level", &bind_legacy);
    register_legacy_import_binder("pulp.series", &bind_legacy);
}
} // namespace pulp::view
