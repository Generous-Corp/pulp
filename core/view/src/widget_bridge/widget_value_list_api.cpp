// widget_bridge/widget_value_list_api.cpp - list value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

#include <pulp/view/ui_components.hpp>

#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void WidgetBridge::register_widget_value_list_api() {
    engine_.register_function("setListItems", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            std::vector<std::string> items;
            if (args.numArgs > 1 && args[1]) {
                auto& arr = *args[1];
                for (uint32_t i = 0; i < arr.size(); ++i)
                    items.push_back(std::string(arr[i].getString()));
            }
            lb->set_items(std::move(items));
        }
        return choc::value::Value{};
    });

    engine_.register_function("setListSelected", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            lb->set_selected(args.get<int>(1, 0));
            lb->ensure_visible(lb->selected());
        }
        return choc::value::Value{};
    });

    engine_.register_function("setListRowHeight", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v))
            lb->set_row_height(static_cast<float>(args.get<double>(1, 24.0)));
        return choc::value::Value{};
    });
}

} // namespace pulp::view
