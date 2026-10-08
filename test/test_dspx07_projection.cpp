#include <catch2/catch_test_macros.hpp>

#include <pulp/format/projection_capability.hpp>

#include <array>
#include <string_view>

TEST_CASE("DSPX-07 bounded baked projections cover native and browser surfaces",
          "[dspx-07][projection]") {
    using namespace pulp::format;
    constexpr std::array supported = {ProjectionSurface::clap, ProjectionSurface::vst3,
                                      ProjectionSurface::lv2, ProjectionSurface::wam,
                                      ProjectionSurface::wclap};
    for (const auto surface : supported) {
        const auto result = projection_capability(surface, true, true);
        REQUIRE(result.supported());
        REQUIRE(result.reason.empty());
    }
}

TEST_CASE("DSPX-07 refuses unsupported projection inputs with typed reasons",
          "[dspx-07][projection][negative]") {
    using namespace pulp::format;
    const auto graph_only = projection_capability(ProjectionSurface::clap, false, true);
    REQUIRE_FALSE(graph_only.supported());
    REQUIRE(graph_only.reason == "graph-only descriptor has no baked Processor projection");

    const auto unbounded = projection_capability(ProjectionSurface::wclap, true, false);
    REQUIRE_FALSE(unbounded.supported());
    REQUIRE(unbounded.reason == "descriptor bounds are missing or exceed adapter limits");

    const auto au = projection_capability(ProjectionSurface::au, true, true);
    REQUIRE_FALSE(au.supported());
    REQUIRE(au.reason == "AU projection is deferred to a separate adapter owner");
}
