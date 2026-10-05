#include <pulp/format/app_updates.hpp>
#include <pulp/format/app_updates_bridge.hpp>

#include <choc/text/choc_JSON.h>

#include <string>

namespace pulp::format {

namespace {

// The status object, with one extra boolean member, as an ok response.
std::string status_response(const char* extra_key = nullptr, bool extra_value = false) {
    auto value = choc::json::parse(app_update_status_json(app_update_status()));
    if (extra_key != nullptr)
        value.addMember(extra_key, extra_value);
    return view::EditorBridge::ok_response(value);
}

bool read_on(const choc::value::ValueView& payload, bool& on) {
    if (!payload.isObject() || !payload.hasObjectMember("on"))
        return false;
    const auto member = payload["on"];
    if (!member.isBool())
        return false;
    on = member.getBool();
    return true;
}

} // namespace

void add_app_update_handlers(view::EditorBridge& bridge) {
    bridge.add_handler(kAppUpdatesGetMessage,
                       [](const choc::value::ValueView&) { return status_response(); });
    bridge.add_handler(kAppUpdatesCheckMessage, [](const choc::value::ValueView&) {
        const bool started = check_for_app_updates();
        return status_response("started", started);
    });
    bridge.add_handler(kAppUpdatesSetAutomaticMessage, [](const choc::value::ValueView& payload) {
        bool on = false;
        if (!read_on(payload, on))
            return view::EditorBridge::err_response(
                "pulp_updates_set_automatic needs {on: true|false}");
        const bool applied = set_app_update_automatic_checks(on);
        return status_response("applied", applied);
    });
    bridge.add_handler(kAppUpdatesOpenReleasesMessage, [](const choc::value::ValueView&) {
        const bool opened = open_app_releases_page();
        return status_response("opened", opened);
    });
}

} // namespace pulp::format
