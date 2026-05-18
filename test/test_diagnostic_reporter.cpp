#include <catch2/catch_test_macros.hpp>
#include <pulp/format/diagnostic_reporter.hpp>
#include <pulp/format/host_type.hpp>
#include <pulp/state/store.hpp>

using namespace pulp::format;
using namespace pulp::state;

static PluginDescriptor make_desc() {
    PluginDescriptor d;
    d.name = "TestPlugin";
    d.manufacturer = "TestCo";
    d.version = "1.0.0";
    d.bundle_id = "com.testco.testplugin";
    d.category = PluginCategory::Effect;
    return d;
}

static void setup_store(StateStore& s) {
    s.add_parameter({.id = 1, .name = "Gain", .unit = "dB", .range = {-60, 12, 0}});
    s.add_parameter({.id = 2, .name = "Mix", .unit = "%", .range = {0, 100, 50}});
}

TEST_CASE("DiagnosticReporter generates text report", "[format][diagnostic]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    setup_store(store);
    store.set_value(1, -6.0f);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(44100, 512, 2, 2);

    auto report = diag.generate_report();

    REQUIRE(report.find("Pulp Diagnostic Report") != std::string::npos);
    REQUIRE(report.find("TestPlugin") != std::string::npos);
    REQUIRE(report.find("TestCo") != std::string::npos);
    REQUIRE(report.find("44100") != std::string::npos);
    REQUIRE(report.find("512") != std::string::npos);
    REQUIRE(report.find("Gain") != std::string::npos);
    REQUIRE(report.find("Effect") != std::string::npos);
}

TEST_CASE("DiagnosticReporter generates JSON report", "[format][diagnostic]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    setup_store(store);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(48000, 256, 2, 2);

    auto json = diag.generate_json();

    REQUIRE(json.find("\"name\": \"TestPlugin\"") != std::string::npos);
    REQUIRE(json.find("\"sampleRate\": 48000") != std::string::npos);
    REQUIRE(json.find("\"parameters\"") != std::string::npos);
    REQUIRE(json.find("\"Gain\"") != std::string::npos);
}

TEST_CASE("DiagnosticReporter includes format and host", "[format][diagnostic]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    setup_store(store);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(44100, 512, 2, 2);
    diag.set_format_name("CLAP");
    diag.set_host_name("Bitwig Studio 5.2");

    auto report = diag.generate_report();
    REQUIRE(report.find("Format: CLAP") != std::string::npos);
    REQUIRE(report.find("Host: Bitwig Studio 5.2") != std::string::npos);
}

TEST_CASE("DiagnosticReporter includes CPU load", "[format][diagnostic]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    setup_store(store);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(44100, 512, 2, 2);
    diag.set_cpu_load(0.15f, 0.25f);

    auto report = diag.generate_report();
    REQUIRE(report.find("CPU Load: 15") != std::string::npos);
    REQUIRE(report.find("CPU Peak: 25") != std::string::npos);
}

TEST_CASE("DiagnosticReporter custom entries", "[format][diagnostic]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    setup_store(store);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(44100, 512, 2, 2);
    diag.add_custom_entry("GPU", "Apple M1 Pro");
    diag.add_custom_entry("Driver", "CoreAudio");

    auto report = diag.generate_report();
    REQUIRE(report.find("GPU: Apple M1 Pro") != std::string::npos);
    REQUIRE(report.find("Driver: CoreAudio") != std::string::npos);
}

TEST_CASE("DiagnosticReporter captures parameter values", "[format][diagnostic]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    setup_store(store);
    store.set_value(1, -12.0f);
    store.set_value(2, 75.0f);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(44100, 512, 2, 2);

    auto report = diag.generate_report();
    REQUIRE(report.find("-12") != std::string::npos);
    REQUIRE(report.find("75") != std::string::npos);
}

TEST_CASE("DiagnosticReporter renders unitless parameters without a unit suffix",
          "[format][diagnostic][coverage][phase3]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    StateStore store;
    store.add_parameter({.id = 7, .name = "Shape", .unit = "", .range = {0, 1, 0.25f}});
    store.set_value(7, 0.5f);

    diag.set_plugin_info(desc, store);

    auto report = diag.generate_report();
    REQUIRE(report.find("[7] Shape: 0.50 (norm=") != std::string::npos);
    REQUIRE(report.find("Shape: 0.50  (norm=") == std::string::npos);
}

TEST_CASE("DiagnosticReporter instrument type", "[format][diagnostic]") {
    DiagnosticReporter diag;
    PluginDescriptor desc;
    desc.name = "MySynth";
    desc.manufacturer = "Co";
    desc.version = "1.0";
    desc.category = PluginCategory::Instrument;

    StateStore store;
    diag.set_plugin_info(desc, store);
    diag.set_audio_config(44100, 512, 0, 2);

    auto report = diag.generate_report();
    REQUIRE(report.find("Instrument") != std::string::npos);
}

TEST_CASE("DiagnosticReporter default report omits optional sections until set",
          "[format][diagnostic][issue-493]") {
    DiagnosticReporter diag;
    auto report = diag.generate_report();
    auto json = diag.generate_json();

    REQUIRE(report.find("--- Plugin ---") != std::string::npos);
    REQUIRE(report.find("Format:") == std::string::npos);
    REQUIRE(report.find("Host:") == std::string::npos);
    REQUIRE(report.find("CPU Load:") == std::string::npos);
    REQUIRE(report.find("--- Custom ---") == std::string::npos);
    REQUIRE(report.find("--- Parameters (0) ---") != std::string::npos);
    REQUIRE(json.find("\"parameters\": [") != std::string::npos);
    REQUIRE(json.find("\"name\": \"\"") != std::string::npos);
}

TEST_CASE("DiagnosticReporter JSON reflects instrument/effect type and parameter values",
          "[format][diagnostic][issue-493]") {
    DiagnosticReporter diag;
    auto desc = make_desc();
    desc.category = PluginCategory::Instrument;
    StateStore store;
    setup_store(store);
    store.set_value(1, -18.0f);
    store.set_value(2, 25.0f);

    diag.set_plugin_info(desc, store);
    diag.set_audio_config(96000, 128, 0, 2);

    auto json = diag.generate_json();
    REQUIRE(json.find("\"type\": \"instrument\"") != std::string::npos);
    REQUIRE(json.find("\"sampleRate\": 96000") != std::string::npos);
    REQUIRE(json.find("\"bufferSize\": 128") != std::string::npos);
    REQUIRE(json.find("\"inputChannels\": 0") != std::string::npos);
    REQUIRE(json.find("\"outputChannels\": 2") != std::string::npos);
    REQUIRE(json.find("\"value\": -18.0000") != std::string::npos);
    REQUIRE(json.find("\"value\": 25.0000") != std::string::npos);
}

TEST_CASE("DiagnosticReporter JSON escapes string fields",
          "[format][diagnostic][coverage][phase3]") {
    DiagnosticReporter diag;
    PluginDescriptor desc;
    desc.name = R"(Quote "Synth")";
    desc.manufacturer = R"(Back\Slash)";
    desc.version = "1.0\nbeta";
    desc.bundle_id = "com.test.escaped";

    StateStore store;
    store.add_parameter({.id = 5, .name = R"(Cutoff "Hz")", .unit = "Hz", .range = {20, 20000, 440}});
    store.set_value(5, 880.0f);
    diag.set_plugin_info(desc, store);

    auto json = diag.generate_json();
    REQUIRE(json.find(R"("name": "Quote \"Synth\"")") != std::string::npos);
    REQUIRE(json.find(R"("manufacturer": "Back\\Slash")") != std::string::npos);
    REQUIRE(json.find(R"("version": "1.0\nbeta")") != std::string::npos);
    REQUIRE(json.find(R"("name": "Cutoff \"Hz\"")") != std::string::npos);
}

TEST_CASE("DiagnosticReporter replacing plugin info clears stale parameter snapshot",
          "[format][diagnostic][coverage][phase3]") {
    DiagnosticReporter diag;

    auto first_desc = make_desc();
    StateStore first_store;
    setup_store(first_store);
    first_store.set_value(1, -24.0f);
    diag.set_plugin_info(first_desc, first_store);

    PluginDescriptor second_desc;
    second_desc.name = "Second";
    second_desc.manufacturer = "OtherCo";
    second_desc.version = "2.0.0";
    second_desc.bundle_id = "com.other.second";
    StateStore second_store;
    second_store.add_parameter({.id = 99, .name = "Depth", .unit = "%", .range = {0, 100, 10}});
    second_store.set_value(99, 60.0f);
    diag.set_plugin_info(second_desc, second_store);

    auto report = diag.generate_report();
    auto json = diag.generate_json();

    REQUIRE(report.find("Name: Second") != std::string::npos);
    REQUIRE(report.find("OtherCo") != std::string::npos);
    REQUIRE(report.find("Depth") != std::string::npos);
    REQUIRE(report.find("Gain") == std::string::npos);
    REQUIRE(report.find("Mix") == std::string::npos);
    REQUIRE(json.find("\"id\": 99") != std::string::npos);
    REQUIRE(json.find("\"Gain\"") == std::string::npos);
}

TEST_CASE("HostType names and feature heuristics cover fixed-size and no-sidechain hosts",
          "[format][host-type][issue-493]") {
    REQUIRE(host_type_name(HostType::Unknown) == "Unknown");
    REQUIRE(host_type_name(HostType::Other) == "Other");
    REQUIRE(host_type_name(static_cast<HostType>(255)) == "Unknown");

    REQUIRE_FALSE(host_supports_resize(HostType::ProTools));
    REQUIRE_FALSE(host_supports_resize(HostType::GarageBand));
    REQUIRE(host_supports_resize(HostType::LogicPro));
    REQUIRE(host_supports_resize(HostType::Unknown));

    REQUIRE_FALSE(host_supports_sidechain(HostType::GarageBand));
    REQUIRE_FALSE(host_supports_sidechain(HostType::AudacityTenacity));
    REQUIRE(host_supports_sidechain(HostType::Reaper));
    REQUIRE(host_supports_sidechain(HostType::Unknown));
}

TEST_CASE("HostType names cover every declared host enum",
          "[format][host-type][coverage][phase3]") {
    REQUIRE(host_type_name(HostType::LogicPro) == "Logic Pro");
    REQUIRE(host_type_name(HostType::GarageBand) == "GarageBand");
    REQUIRE(host_type_name(HostType::AbletonLive) == "Ableton Live");
    REQUIRE(host_type_name(HostType::Reaper) == "REAPER");
    REQUIRE(host_type_name(HostType::ProTools) == "Pro Tools");
    REQUIRE(host_type_name(HostType::Cubase) == "Cubase");
    REQUIRE(host_type_name(HostType::Nuendo) == "Nuendo");
    REQUIRE(host_type_name(HostType::StudioOne) == "Studio One");
    REQUIRE(host_type_name(HostType::FLStudio) == "FL Studio");
    REQUIRE(host_type_name(HostType::Bitwig) == "Bitwig Studio");
    REQUIRE(host_type_name(HostType::Maschine) == "Maschine");
    REQUIRE(host_type_name(HostType::AudacityTenacity) == "Audacity/Tenacity");
    REQUIRE(host_type_name(HostType::Ardour) == "Ardour");
    REQUIRE(host_type_name(HostType::Standalone) == "Pulp Standalone");
}

TEST_CASE("HostType feature heuristics default permissive for modern hosts",
          "[format][host-type][coverage][phase3]") {
    for (auto host : {HostType::LogicPro,
                      HostType::AbletonLive,
                      HostType::Reaper,
                      HostType::Cubase,
                      HostType::Nuendo,
                      HostType::StudioOne,
                      HostType::FLStudio,
                      HostType::Bitwig,
                      HostType::Maschine,
                      HostType::Ardour,
                      HostType::Standalone,
                      HostType::Other}) {
        REQUIRE(host_supports_resize(host));
        REQUIRE(host_supports_sidechain(host));
    }
}
