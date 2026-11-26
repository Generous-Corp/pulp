#include <pulp/vellum_components/catalogue.hpp>

#include <pulp/vellum_components/lasso.hpp>
#include <pulp/vellum_components/model_manager_view.hpp>
#include <pulp/vellum_components/preferences_panel.hpp>
#include <pulp/vellum_components/preset_program_editor.hpp>

#include <functional>
#include <utility>

namespace pulp::vellum_components {

namespace {

using Factory = std::function<std::unique_ptr<View>(const PulpViewComponentContext&)>;

struct Entry {
    std::string_view id;
    Factory make;
};

const std::vector<Entry>& entries() {
    static const std::vector<Entry> table = {
        {"pulp.panic_button",
         [](const PulpViewComponentContext& ctx) {
             return std::make_unique<PanicButton>(ctx.panic);
         }},
        {"pulp.lasso",
         [](const PulpViewComponentContext&) { return std::make_unique<LassoComponent>(); }},
        {"pulp.preferences_panel",
         [](const PulpViewComponentContext&) {
             // Persistence is bound by the caller after the pages are added:
             // binding restores the stored page, which needs the pages to exist.
             return std::make_unique<PreferencesPanel>();
         }},
        {"pulp.model_manager",
         [](const PulpViewComponentContext&) { return std::make_unique<ModelManagerView>(); }},
        {"pulp.preset_program_editor",
         [](const PulpViewComponentContext& ctx) -> std::unique_ptr<View> {
             if (!ctx.preset_library)
                 return nullptr;
             return std::make_unique<PresetProgramEditor>(*ctx.preset_library);
         }},
    };
    return table;
}

} // namespace

const std::vector<std::string_view>& pulp_view_component_ids() {
    static const std::vector<std::string_view> ids = [] {
        std::vector<std::string_view> out;
        for (const auto& entry : entries())
            out.push_back(entry.id);
        return out;
    }();
    return ids;
}

std::unique_ptr<::vellum::view::View>
make_pulp_view_component(std::string_view component_id, const PulpViewComponentContext& context) {
    for (const auto& entry : entries())
        if (entry.id == component_id)
            return entry.make(context);
    return nullptr;
}

} // namespace pulp::vellum_components
