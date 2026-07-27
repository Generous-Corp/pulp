// Fixed-capacity modulation-matrix contracts split from the shared
// mod-utilities suite.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/mod_matrix.hpp>

#include <cmath>
#include <cstddef>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

TEST_CASE("ModMatrix sums routes and reports its own worst case",
          "[mod-matrix][mod-utilities]") {
    round2::ModMatrixT<4, 3, double> matrix;
    REQUIRE(matrix.add_route(0, 1, 0.5));
    REQUIRE(matrix.add_route(2, 1, -0.25));
    REQUIRE(matrix.route_count() == 2);

    matrix.set_source(0, 1.0);
    matrix.set_source(2, 1.0);
    matrix.process();
    REQUIRE_THAT(matrix.get(1), WithinAbs(0.25, 1e-12));
    REQUIRE(matrix.get(0) == 0.0);
    REQUIRE(matrix.get(2) == 0.0);

    REQUIRE_THAT(matrix.worst_case_for(1), WithinAbs(0.75, 1e-12));
    for (double a : {-1.0, -0.3, 0.0, 0.6, 1.0}) {
        for (double b : {-1.0, 0.0, 1.0}) {
            matrix.set_source(0, a);
            matrix.set_source(2, b);
            matrix.process();
            REQUIRE(std::abs(matrix.get(1)) <= matrix.worst_case_for(1) + 1e-12);
        }
    }
}

TEST_CASE("ModMatrix refuses bad routes rather than dropping them silently",
          "[mod-matrix][mod-utilities]") {
    DenseModMatrixT<2, 2, double> matrix;
    REQUIRE_FALSE(matrix.add_route(2, 0, 1.0));
    REQUIRE_FALSE(matrix.add_route(0, 2, 1.0));
    REQUIRE(matrix.route_count() == 0);

    for (std::size_t i = 0; i < decltype(matrix)::kMaxRoutes; ++i)
        REQUIRE(matrix.add_route(0, 0, 0.1));
    REQUIRE_FALSE(matrix.add_route(0, 0, 0.1));
}

TEST_CASE("ModMatrix reset clears signal state but preserves the patch",
          "[mod-matrix][mod-utilities]") {
    DenseModMatrixT<2, 2, double> matrix;
    matrix.add_route(0, 0, 1.0);
    matrix.set_source(0, 1.0);
    matrix.process();
    REQUIRE_THAT(matrix.get(0), WithinAbs(1.0, 1e-12));

    matrix.reset();
    REQUIRE(matrix.route_count() == 1);
    REQUIRE(matrix.source(0) == 0.0);
    matrix.process();
    REQUIRE(matrix.get(0) == 0.0);
}
