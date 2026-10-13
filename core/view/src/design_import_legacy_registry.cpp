#include <pulp/view/design_import.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace pulp::view {
namespace {
// Written on every editor creation (register_audio_widgets) and read while a
// tree is bound, possibly on another thread, so access is serialised.
std::unordered_map<std::string, LegacyImportBinder>& binders() {
    static std::unordered_map<std::string, LegacyImportBinder> value;
    return value;
}
std::mutex& binders_mutex() {
    static std::mutex value;
    return value;
}
} // namespace

void register_legacy_import_binder(std::string id, LegacyImportBinder binder) {
    if (id.empty() || !binder)
        return;
    std::lock_guard lock(binders_mutex());
    auto& entries = binders();
    if (const auto it = entries.find(id); it != entries.end() && it->second == binder)
        return;
    entries[std::move(id)] = binder;
}

void invoke_legacy_import_binder(std::string_view id, NativeImportBindingContext& ctx, View& view,
                                 const NativeImportMeterBindingDescriptor* meter,
                                 const NativeImportWaveformBindingDescriptor* waveform) {
    LegacyImportBinder binder = nullptr;
    {
        std::lock_guard lock(binders_mutex());
        if (const auto it = binders().find(std::string(id)); it != binders().end())
            binder = it->second;
    }
    if (binder)
        binder(ctx, view, meter, waveform);
}

void bind_imported_level_display(NativeImportBindingContext& ctx, View& view, LevelDisplay& display,
                                 const NativeImportMeterBindingDescriptor& descriptor) {
    ctx.bind_level_display(view, display, descriptor);
    invoke_legacy_import_binder("pulp.level", ctx, view, &descriptor, nullptr);
}

void bind_imported_series_display(NativeImportBindingContext& ctx, View& view,
                                  SeriesDisplay& display,
                                  const NativeImportWaveformBindingDescriptor& descriptor) {
    ctx.bind_series_display(view, display, descriptor);
    invoke_legacy_import_binder("pulp.series", ctx, view, nullptr, &descriptor);
}
} // namespace pulp::view
