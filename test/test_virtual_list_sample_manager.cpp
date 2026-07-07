#include <catch2/catch_test_macros.hpp>

#include <pulp/view/input_events.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include "../examples/virtual-list-sample-manager/sample_manager_virtual_list.hpp"

#include <algorithm>
#include <optional>
#include <vector>

using namespace pulp::examples::virtual_list_sample_manager;

namespace {

std::vector<const pulp::view::View*> row_identities(const pulp::view::VirtualList& list) {
    std::vector<const pulp::view::View*> ids;
    for (std::size_t i = 0; i < list.realized_row_count(); ++i)
        ids.push_back(list.realized_row_at_slot(i));
    std::sort(ids.begin(), ids.end());
    return ids;
}

pulp::view::MouseEvent click_at(float x, float y, int clicks = 1) {
    pulp::view::MouseEvent ev;
    ev.position = {x, y};
    ev.is_down = true;
    ev.button = pulp::view::MouseButton::left;
    ev.click_count = clicks;
    return ev;
}

} // namespace

TEST_CASE("VirtualList sample-manager example recycles rich rows",
          "[view][virtual-list][sample-manager]") {
    auto fixture = build_sample_manager();
    fixture.root->layout_children();
    auto& list = fixture.root->list();

    REQUIRE(list.row_count() == kSampleCount);
    REQUIRE(list.realized_row_count() < 24);
    REQUIRE(list.child_count() == list.realized_row_count());

    auto before = row_identities(list);
    const auto initial_bind_calls = fixture.state->bind_calls;
    list.scroll_to_row(7345);
    auto after = row_identities(list);
    REQUIRE(after == before);
    REQUIRE(fixture.state->bind_calls > initial_bind_calls);

    const auto indices = list.realized_indices();
    REQUIRE_FALSE(indices.empty());
    REQUIRE(indices.front() <= 7345);
    REQUIRE(indices.back() >= 7345);

    std::optional<std::size_t> activated;
    list.on_row_activated([&](std::size_t index) { activated = index; });
    const auto visible = std::find_if(indices.begin(), indices.end(), [&](std::size_t i) {
        const float y = static_cast<float>(i) * kRowHeight - list.scroll_y() + 4.0f;
        return y >= 0.0f && y < list.local_bounds().height;
    });
    REQUIRE(visible != indices.end());
    const std::size_t target = *visible;
    const float y = static_cast<float>(target) * kRowHeight - list.scroll_y() + 4.0f;
    list.on_mouse_event(click_at(8.0f, y, 2));
    REQUIRE(activated == target);
    REQUIRE(list.selection() == std::vector<std::size_t>{target});
}

TEST_CASE("VirtualList sample-manager example captures a non-blank PNG",
          "[view][virtual-list][sample-manager][screenshot]") {
    auto fixture = build_sample_manager();
    auto capture = pulp::view::capture_view(*fixture.root, kWidth, kHeight, 1.0f);
    if (!capture.ok) {
        SKIP("screenshot capture unavailable: " << capture.reason);
    }

    const auto stats = pulp::view::analyze_screenshot_content(capture.png);
    REQUIRE(stats.passes_content_floor());
}
