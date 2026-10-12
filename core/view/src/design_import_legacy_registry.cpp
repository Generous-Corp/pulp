#include <pulp/view/design_import.hpp>
#include <unordered_map>

namespace pulp::view {
namespace {
std::unordered_map<std::string, LegacyImportBinder>& binders() {
    static std::unordered_map<std::string, LegacyImportBinder> value;
    return value;
}
} // namespace
void register_legacy_import_binder(std::string id, LegacyImportBinder binder) {
    if (!id.empty() && binder)
        binders()[std::move(id)] = binder;
}
void invoke_legacy_import_binder(std::string_view id, NativeImportBindingContext& ctx, View& view,
                                 const NativeImportMeterBindingDescriptor* meter,
                                 const NativeImportWaveformBindingDescriptor* waveform) {
    if (const auto it = binders().find(std::string(id)); it != binders().end())
        it->second(ctx, view, meter, waveform);
}
} // namespace pulp::view
