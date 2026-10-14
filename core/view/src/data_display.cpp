#include <mutex>
#include <pulp/runtime/log.hpp>
#include <pulp/view/data_display.hpp>
#include <unordered_map>
#include <utility>

namespace pulp::view {
namespace {
std::unordered_map<std::string, DataDisplayRegistration>& registry() {
    static std::unordered_map<std::string, DataDisplayRegistration> value;
    return value;
}
std::mutex& registry_mutex() {
    static std::mutex value;
    return value;
}
} // namespace

LevelDisplay::~LevelDisplay() = default;
SeriesDisplay::~SeriesDisplay() = default;
BinDisplay::~BinDisplay() = default;

void register_data_display(DataDisplayRegistration registration) {
    if (registration.id.empty() || registration.make == nullptr)
        return;
    std::lock_guard lock(registry_mutex());
    auto& entries = registry();
    const auto it = entries.find(registration.id);
    bool same_catalog =
        it != entries.end() && it->second.catalog_rows.size() == registration.catalog_rows.size();
    if (same_catalog) {
        for (std::size_t i = 0; i < registration.catalog_rows.size(); ++i) {
            const auto& a = it->second.catalog_rows[i];
            const auto& b = registration.catalog_rows[i];
            if (a.id != b.id || a.class_name != b.class_name || a.header != b.header) {
                same_catalog = false;
                break;
            }
        }
    }
    if (it != entries.end() && it->second.make == registration.make && same_catalog)
        return;
    if (it != entries.end() && it->second.make != registration.make)
        runtime::log_info("view: replacing data-display factory for '{}'", registration.id);
    entries[registration.id] = std::move(registration);
}

bool has_data_display(std::string_view id) {
    std::lock_guard lock(registry_mutex());
    return registry().find(std::string(id)) != registry().end();
}

std::unique_ptr<View> make_data_display(std::string_view id) {
    DataDisplayFactory make = nullptr;
    {
        std::lock_guard lock(registry_mutex());
        const auto it = registry().find(std::string(id));
        if (it != registry().end())
            make = it->second.make;
    }
    return make == nullptr ? nullptr : make();
}

void clear_data_displays() {
    std::lock_guard lock(registry_mutex());
    registry().clear();
}
std::size_t data_display_registration_count() {
    std::lock_guard lock(registry_mutex());
    return registry().size();
}

} // namespace pulp::view
