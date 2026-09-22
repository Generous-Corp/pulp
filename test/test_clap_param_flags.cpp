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
        // A bypass is a host-owned switch. An additive offset on it names no
        // defined state, so the adapter must never invite one.
        REQUIRE((flags & CLAP_PARAM_IS_MODULATABLE) == 0);
    };
    require_bypass_flags(legacy);
    require_bypass_flags(designated);

    const pulp::state::ParamInfo continuous{
        .id = 7,
        .name = "Gain",
        .range = {0.0f, 1.0f, 0.5f},
    };
    REQUIRE(pulp::format::clap_generic::params_flags(continuous) ==
            (CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE));

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
    // An indexed choice is not a magnitude: an offset between two indices
    // names no value the author declared.
    REQUIRE((discrete_flags & CLAP_PARAM_IS_MODULATABLE) == 0);
}

TEST_CASE("CLAP modulation advertisement matches the inbound decode predicate",
          "[clap][params][modulation]") {
    // A continuous parameter that quantizes its plain value for display is
    // still a magnitude. Kind decides, not range.step — so this parameter is
    // advertised modulatable and is not advertised stepped.
    pulp::state::ParamInfo quantized_continuous{
        .id = 20,
        .name = "Gain",
        .unit = "dB",
        .range = {-60.0f, 24.0f, 0.0f, 0.1f},
    };
    const auto quantized_flags = pulp::format::clap_generic::params_flags(quantized_continuous);
    REQUIRE((quantized_flags & CLAP_PARAM_IS_MODULATABLE) != 0);
    REQUIRE((quantized_flags & CLAP_PARAM_IS_STEPPED) == 0);

    // An author may opt a control out; nothing else about the parameter
    // changes.
    pulp::state::ParamInfo opted_out = quantized_continuous;
    opted_out.id = 21;
    opted_out.modulatable = false;
    const auto opted_out_flags = pulp::format::clap_generic::params_flags(opted_out);
    REQUIRE((opted_out_flags & CLAP_PARAM_IS_MODULATABLE) == 0);
    REQUIRE((opted_out_flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);

    // A one-shot trigger is cleared at the end of every block, so a standing
    // offset would hold it raised forever.
    pulp::state::ParamInfo trigger{
        .id = 22,
        .name = "Panic",
        .range = {0.0f, 1.0f, 0.0f},
    };
    trigger.is_trigger = true;
    REQUIRE((pulp::format::clap_generic::params_flags(trigger) & CLAP_PARAM_IS_MODULATABLE) == 0);

    pulp::state::ParamInfo reset_designated{
        .id = 23,
        .name = "Rebuild",
        .range = {0.0f, 1.0f, 0.0f},
    };
    reset_designated.designation = pulp::state::ParamDesignation::Reset;
    REQUIRE((pulp::format::clap_generic::params_flags(reset_designated) &
             CLAP_PARAM_IS_MODULATABLE) == 0);

    // The advertisement and the inbound PARAM_MOD decode read the same
    // predicate. If they ever diverge a host is told it may modulate a
    // parameter whose events the adapter then drops.
    const auto advertises_modulatable = [](const pulp::state::ParamInfo& p) {
        return (pulp::format::clap_generic::params_flags(p) & CLAP_PARAM_IS_MODULATABLE) != 0;
    };
    for (const auto* parameter : {&quantized_continuous, &opted_out, &trigger, &reset_designated}) {
        REQUIRE(advertises_modulatable(*parameter) ==
                pulp::state::is_modulatable_param(*parameter));
    }
}

TEST_CASE("CLAP projects declared hidden and read-only parameters", "[clap][params][visibility]") {
    // Baseline: an undeclared parameter is visible, writable and automatable
    // exactly as before, so existing plugins are unchanged.
    const pulp::state::ParamInfo ordinary{
        .id = 40,
        .name = "Gain",
        .range = {0.0f, 1.0f, 0.5f},
    };
    const auto ordinary_flags = pulp::format::clap_generic::params_flags(ordinary);
    REQUIRE((ordinary_flags & CLAP_PARAM_IS_HIDDEN) == 0);
    REQUIRE((ordinary_flags & CLAP_PARAM_IS_READONLY) == 0);
    REQUIRE((ordinary_flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);

    pulp::state::ParamInfo hidden = ordinary;
    hidden.id = 41;
    hidden.hidden = true;
    REQUIRE((pulp::format::clap_generic::params_flags(hidden) & CLAP_PARAM_IS_HIDDEN) != 0);

    // A meter-style value: the host renders it and must never drive it.
    pulp::state::ParamInfo meter = ordinary;
    meter.id = 42;
    meter.read_only = true;
    const auto meter_flags = pulp::format::clap_generic::params_flags(meter);
    REQUIRE((meter_flags & CLAP_PARAM_IS_READONLY) != 0);
    // Automation is a host write, so read-only withholds AUTOMATABLE too.
    REQUIRE((meter_flags & CLAP_PARAM_IS_AUTOMATABLE) == 0);
    // ...and a value the host may not write is not one it may offset either.
    REQUIRE((meter_flags & CLAP_PARAM_IS_MODULATABLE) == 0);

    pulp::state::ParamInfo not_automatable = ordinary;
    not_automatable.id = 43;
    not_automatable.automatable = false;
    const auto not_automatable_flags = pulp::format::clap_generic::params_flags(not_automatable);
    REQUIRE((not_automatable_flags & CLAP_PARAM_IS_AUTOMATABLE) == 0);
    // Non-automatable is not read-only: the host may still set it directly.
    REQUIRE((not_automatable_flags & CLAP_PARAM_IS_READONLY) == 0);
}

TEST_CASE("CLAP keeps a bypass control reachable despite a hide or read-only declaration",
          "[clap][params][visibility][bypass]") {
    // Hosts surface bypass in their own chrome. A plugin that hid or froze it
    // would become un-bypassable from the host, so the shared predicates
    // refuse both for a bypass control.
    pulp::state::ParamInfo bypass{
        .id = 44,
        .name = "Effect Enabled",
        .range = {0.0f, 1.0f, 0.0f},
    };
    bypass.designation = pulp::state::ParamDesignation::Bypass;
    bypass.hidden = true;
    bypass.read_only = true;

    const auto flags = pulp::format::clap_generic::params_flags(bypass);
    REQUIRE((flags & CLAP_PARAM_IS_BYPASS) != 0);
    REQUIRE((flags & CLAP_PARAM_IS_HIDDEN) == 0);
    REQUIRE((flags & CLAP_PARAM_IS_READONLY) == 0);
    REQUIRE((flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
}
