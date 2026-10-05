#include <catch2/catch_test_macros.hpp>

TEST_CASE("DSPX-04 product binding typed skip when inspector dependencies are unavailable",
          "[inspect][control][dspx-04][e2e][product][typed-skip]") {
    SKIP("DSPX-04 product binding requires the inspector SDK and product dependencies");
}
