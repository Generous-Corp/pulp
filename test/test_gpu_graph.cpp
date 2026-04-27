#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/render/gpu_graph.hpp>

#include <limits>

using namespace pulp::render;

TEST_CASE("GpuGraphRenderer basic usage", "[render][gpu-graph]") {
    GpuGraphRenderer g;
    REQUIRE(g.empty());

    std::vector<float> data = {0.0f, 0.5f, 1.0f, 0.5f, 0.0f};
    g.set_data(data);
    REQUIRE(g.count() == 5);
    REQUIRE_FALSE(g.empty());
    REQUIRE(g.data()[2] == Catch::Approx(1.0f));
}

TEST_CASE("GpuGraphRenderer properties", "[render][gpu-graph]") {
    GpuGraphRenderer g;
    g.set_line_thickness(3.0f);
    REQUIRE(g.line_thickness() == Catch::Approx(3.0f));

    g.set_show_fill(false);
    REQUIRE_FALSE(g.show_fill());

    g.set_fill_center(0.0f);
    REQUIRE(g.fill_center() == Catch::Approx(0.0f));
}

TEST_CASE("GpuHeatMapRenderer stores 2D data", "[render][gpu-graph]") {
    GpuHeatMapRenderer h;
    REQUIRE(h.empty());

    float data[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    h.set_data(data, 3, 2);
    REQUIRE(h.width() == 3);
    REQUIRE(h.height() == 2);
    REQUIRE(h.data().size() == 6);
}

TEST_CASE("GpuBarRenderer stores bar data", "[render][gpu-graph]") {
    GpuBarRenderer b;
    float data[] = {0.5f, 0.8f, 0.3f};
    b.set_data(data, 3);
    REQUIRE(b.count() == 3);
    b.set_bar_width(6.0f);
    REQUIRE(b.bar_width() == Catch::Approx(6.0f));
}

// ── Additional coverage — issue-646 ─────────────────────────────────────

TEST_CASE("GpuGraphRenderer raw-pointer set_data overload - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuGraphRenderer g;
    float data[] = {-1.0f, -0.25f, 0.25f, 1.0f};
    g.set_data(data, 4);
    REQUIRE(g.count() == 4);
    REQUIRE_FALSE(g.empty());
    REQUIRE(g.data().front() == Catch::Approx(-1.0f));
    REQUIRE(g.data().back() == Catch::Approx(1.0f));

    // Empty overwrite resets to empty.
    g.set_data(data, 0);
    REQUIRE(g.empty());
    REQUIRE(g.count() == 0);
}

TEST_CASE("GpuGraphRenderer null data clears without pointer arithmetic - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuGraphRenderer g;
    float data[] = {0.25f, 0.5f};
    g.set_data(data, 2);
    REQUIRE_FALSE(g.empty());

    g.set_data(nullptr, 8);
    REQUIRE(g.empty());
    REQUIRE(g.count() == 0);
}

TEST_CASE("GpuGraphRenderer defaults match documented values - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuGraphRenderer g;
    REQUIRE(g.line_thickness() == Catch::Approx(1.5f));
    REQUIRE(g.show_fill() == true);
    REQUIRE(g.fill_center() == Catch::Approx(0.5f));
}

TEST_CASE("GpuHeatMapRenderer range set/get - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuHeatMapRenderer h;
    // Defaults.
    REQUIRE(h.min_val() == Catch::Approx(0.0f));
    REQUIRE(h.max_val() == Catch::Approx(1.0f));

    h.set_range(-60.0f, 0.0f);
    REQUIRE(h.min_val() == Catch::Approx(-60.0f));
    REQUIRE(h.max_val() == Catch::Approx(0.0f));

    // set_data with zero-size clears.
    float row[] = {0.1f};
    h.set_data(row, 0, 0);
    REQUIRE(h.empty());
    REQUIRE(h.width() == 0);
    REQUIRE(h.height() == 0);
}

TEST_CASE("GpuHeatMapRenderer rejects null and overflowing grids - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuHeatMapRenderer h;
    float data[] = {0.1f, 0.2f, 0.3f, 0.4f};
    h.set_data(data, 2, 2);
    REQUIRE_FALSE(h.empty());
    REQUIRE(h.width() == 2);
    REQUIRE(h.height() == 2);

    h.set_data(nullptr, 2, 2);
    REQUIRE(h.empty());
    REQUIRE(h.width() == 0);
    REQUIRE(h.height() == 0);

    h.set_data(data, std::numeric_limits<size_t>::max(), 2);
    REQUIRE(h.empty());
    REQUIRE(h.width() == 0);
    REQUIRE(h.height() == 0);
}

TEST_CASE("GpuBarRenderer gap + data accessors - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuBarRenderer b;
    // Defaults.
    REQUIRE(b.bar_width() == Catch::Approx(4.0f));
    REQUIRE(b.gap() == Catch::Approx(1.0f));

    b.set_gap(2.5f);
    REQUIRE(b.gap() == Catch::Approx(2.5f));

    float data[] = {0.1f, 0.9f};
    b.set_data(data, 2);
    REQUIRE(b.data().size() == 2);
    REQUIRE(b.data()[1] == Catch::Approx(0.9f));
}

TEST_CASE("GpuBarRenderer null and zero data clear existing bars - issue-646",
          "[render][gpu-graph][issue-646]") {
    GpuBarRenderer b;
    float data[] = {0.2f, 0.4f, 0.8f};
    b.set_data(data, 3);
    REQUIRE(b.count() == 3);

    b.set_data(nullptr, 3);
    REQUIRE(b.count() == 0);
    REQUIRE(b.data().empty());

    b.set_data(data, 3);
    REQUIRE(b.count() == 3);
    b.set_data(data, 0);
    REQUIRE(b.count() == 0);
    REQUIRE(b.data().empty());
}
