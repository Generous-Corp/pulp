#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/render/gpu_graph.hpp>

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
