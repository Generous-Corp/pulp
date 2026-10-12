#include <pulp/view/data_display.hpp>
#include <unordered_map>
#include <utility>

namespace pulp::view {
namespace {
std::unordered_map<std::string, DataDisplayRegistration>& registry() {
    static std::unordered_map<std::string, DataDisplayRegistration> value;
    return value;
}
} // namespace

LevelDisplay::~LevelDisplay() = default;
SeriesDisplay::~SeriesDisplay() = default;
BinDisplay::~BinDisplay() = default;

void register_data_display(DataDisplayRegistration registration) {
    if (registration.id.empty() || registration.make == nullptr)
        return;
    registry()[registration.id] = std::move(registration);
}

bool has_data_display(std::string_view id) {
    return registry().find(std::string(id)) != registry().end();
}

std::unique_ptr<View> make_data_display(std::string_view id) {
    const auto it = registry().find(std::string(id));
    return it == registry().end() ? nullptr : it->second.make();
}

void clear_data_displays() {
    registry().clear();
}
std::size_t data_display_registration_count() {
    return registry().size();
}

} // namespace pulp::view
