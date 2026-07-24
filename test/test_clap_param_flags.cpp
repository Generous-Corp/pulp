#include <catch2/catch_test_macros.hpp>
#include <pulp/format/clap_entry.hpp>

TEST_CASE("CLAP presents every bypass parameter as stepped",
          "[clap][params][bypass]") {
    const pulp::state::ParamInfo legacy{
        .id = 5,
        .name = "Bypass",
        .range = {0.0f, 1.0f, 0.0f, 1.0f},
    };
    pulp::state::ParamInfo designated{
        .id = 6,
        .name = "Effect Enabled",
        .range = {0.0f, 1.0f, 0.0f},
    };
    designated.designation = pulp::state::ParamDesignation::Bypass;

    const auto require_bypass_flags = [](const pulp::state::ParamInfo& parameter) {
        const auto flags = pulp::format::clap_generic::params_flags(parameter);
        REQUIRE((flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
        REQUIRE((flags & CLAP_PARAM_IS_BYPASS) != 0);
        REQUIRE((flags & CLAP_PARAM_IS_STEPPED) != 0);
    };
    require_bypass_flags(legacy);
    require_bypass_flags(designated);

    const pulp::state::ParamInfo continuous{
        .id = 7,
        .name = "Gain",
        .range = {0.0f, 1.0f, 0.5f},
    };
    REQUIRE(pulp::format::clap_generic::params_flags(continuous) ==
            CLAP_PARAM_IS_AUTOMATABLE);

    pulp::state::ParamInfo discrete{
        .id = 8,
        .name = "Mode",
        .range = {0.0f, 2.0f, 0.0f, 1.0f},
    };
    discrete.kind = pulp::state::ParamKind::Enum;
    const auto discrete_flags = pulp::format::clap_generic::params_flags(discrete);
    REQUIRE((discrete_flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
    REQUIRE((discrete_flags & CLAP_PARAM_IS_STEPPED) != 0);
    REQUIRE((discrete_flags & CLAP_PARAM_IS_BYPASS) == 0);
}
