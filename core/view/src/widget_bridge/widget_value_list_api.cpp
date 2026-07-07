// widget_bridge/widget_value_list_api.cpp - list value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_list.hpp>

#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void WidgetBridge::register_widget_value_list_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "setListItems", [this](choc::javascript::ArgumentList args) {
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

    register_bridge_function(api, "setListSelected", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            lb->set_selected(args.get<int>(1, 0));
            lb->ensure_visible(lb->selected());
        }
        return choc::value::Value{};
    });

    register_bridge_function(api, "setListRowHeight", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v))
            vl->set_row_height(static_cast<float>(args.get<double>(1, 24.0)));
        else if (auto* lb = dynamic_cast<ListBox*>(v))
            lb->set_row_height(static_cast<float>(args.get<double>(1, 24.0)));
        return choc::value::Value{};
    });

    register_bridge_function(api, "setVirtualListRowCount", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v))
            vl->set_row_count(static_cast<std::size_t>(std::max(0, args.get<int>(1, 0))));
        return choc::value::Value{};
    });

    register_bridge_function(api, "setVirtualListOverscan", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v))
            vl->set_overscan(args.get<int>(1, 3));
        return choc::value::Value{};
    });

    register_bridge_function(api, "setVirtualListSelectionMode", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "single");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v)) {
            if (mode == "none") vl->set_selection_mode(VirtualList::SelectionMode::none);
            else if (mode == "multi") vl->set_selection_mode(VirtualList::SelectionMode::multi);
            else vl->set_selection_mode(VirtualList::SelectionMode::single);
        }
        return choc::value::Value{};
    });

    register_bridge_function(api, "setVirtualListSelected", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v)) {
            const auto index = args.get<int>(1, -1);
            if (index < 0) vl->clear_selection();
            else vl->select_row(static_cast<std::size_t>(index));
        }
        return choc::value::Value{};
    });

    register_bridge_function(api, "scrollVirtualListToRow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v))
            vl->scroll_to_row(static_cast<std::size_t>(std::max(0, args.get<int>(1, 0))));
        return choc::value::Value{};
    });

    register_bridge_function(api, "refreshVirtualListRows", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* vl = dynamic_cast<VirtualList*>(v))
            vl->refresh_rows();
        return choc::value::Value{};
    });
}

} // namespace pulp::view
