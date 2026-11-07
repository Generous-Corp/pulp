#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/inspect/control_agent_toolkit.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/state/store.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kVendor = "com.example";
constexpr pulp::state::ParamID kGain = 1;
constexpr pulp::state::ParamID kTone = 2;
constexpr pulp::state::ParamID kBypass = 3;

// Plays a sine per held note and passes its input through, both scaled by
// the gain parameter, so renders depend on notes, input, patch and lanes.
class ToolkitSynth final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Toolkit fixture";
        d.manufacturer = "Example";
        d.bundle_id = "com.example.toolkit-fixture";
        d.version = "1.0.0";
        d.accepts_midi = true;
        d.input_buses = {{"In", 2, false}};
        d.output_buses = {{"Out", 2, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo gain;
        gain.id = kGain;
        gain.name = "Gain";
        gain.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(gain);
        pulp::state::ParamInfo tone;
        tone.id = kTone;
        tone.name = "Tone";
        tone.unit = "Hz";
        tone.range = {20.0f, 20000.0f, 440.0f};
        store.add_parameter(tone);
        pulp::state::ParamInfo bypass;
        bypass.id = kBypass;
        bypass.name = "Bypass";
        bypass.range = {0.0f, 1.0f, 0.0f, 1.0f};
        bypass.designation = pulp::state::ParamDesignation::Bypass;
        store.add_parameter(bypass);
    }
    void prepare(const pulp::format::PrepareContext& context) override {
        sample_rate_ = context.sample_rate;
    }
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer& midi,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext& context) override {
        for (const auto& event : midi) {
            if (event.is_note_on() && event.velocity() > 0)
                held_ = event.note();
            else if (event.is_note_off() && held_ == event.note())
                held_.reset();
        }
        const auto gain = state().get_value(kGain);
        const double frequency =
            held_ ? 440.0 * std::pow(2.0, (static_cast<double>(*held_) - 69.0) / 12.0) : 0.0;
        for (int frame = 0; frame < context.num_samples; ++frame) {
            const auto tone = held_ ? std::sin(phase_) : 0.0;
            phase_ += 2.0 * std::numbers::pi * frequency / sample_rate_;
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
                const float dry = channel < input.num_channels()
                                      ? input.channel(channel)[static_cast<std::size_t>(frame)]
                                      : 0.0f;
                output.channel(channel)[static_cast<std::size_t>(frame)] =
                    gain * (static_cast<float>(tone) + dry);
            }
        }
    }

  private:
    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
    std::optional<std::uint8_t> held_;
};

std::unique_ptr<pulp::format::Processor> create_synth() {
    return std::make_unique<ToolkitSynth>();
}

std::string declaration(std::string_view vendor) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "tools" /
                      "cmake" / "control_agent_toolkit_operations.json.in";
    std::ifstream stream(path);
    REQUIRE(stream);
    std::string text{std::istreambuf_iterator<char>(stream), {}};
    const std::string placeholder = "@PULP_AGENT_TOOLKIT_VENDOR@";
    for (auto at = text.find(placeholder); at != std::string::npos; at = text.find(placeholder))
        text.replace(at, placeholder.size(), vendor);
    return text;
}

std::optional<VerifiedControlPeerIdentity> peer(ControlPeerRole role, std::int64_t pid,
                                                std::string start) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    return verifier.verify(ControlPeerEvidence{role, "uid:501", pid, std::move(start),
                                               "signed:com.example.toolkit-fixture",
                                               "publisher.example"});
}

std::string random_suffix() {
    const auto bytes = pulp::runtime::secure_random_bytes(8);
    REQUIRE(bytes);
    return pulp::runtime::hex_encode(*bytes);
}

struct ToolkitFixture {
    std::filesystem::path store_path =
        std::filesystem::temp_directory_path() / ("pulp-agent-toolkit-" + random_suffix());
    VerifiedControlPeerIdentity client =
        std::move(*peer(ControlPeerRole::Client, 101, "client-start"));
    VerifiedControlPeerIdentity host =
        std::move(*peer(ControlPeerRole::StandaloneHost, 201, "host-start"));
    std::unique_ptr<ControlBroker> broker;
    ControlClientIdentity client_identity;
    ControlRegistration registration;
    ControlGrant grant;

    pulp::state::StateStore store;
    ToolkitSynth processor;
    std::shared_ptr<pulp::state::PresetManager> presets =
        std::make_shared<pulp::state::PresetManager>(
            store, "PulpAgentToolkitTest" + random_suffix(), "Fixture");
    std::shared_ptr<ControlVendorOperationHandlers> handlers =
        std::make_shared<ControlVendorOperationHandlers>();
    ControlOperationExecutor executor;
    int requests = 0;

    ToolkitFixture() {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        auto allow = [](const ControlRegistration&, const ControlOperationDescriptor&) {
            return true;
        };
        ControlBrokerConfig config;
        config.admission = {allow, allow, allow};
        config.operation_store = ControlOperationStoreConfig{
            .directory = store_path, .replay_window = 24h, .retention = 24h * 7};
        config.artifact_store = ControlArtifactStoreConfig{
            .root = store_path.string() + "-artifacts", .maximum_lifetime = 24h};
        broker = std::make_unique<ControlBroker>(std::move(config));
        auto ticket = broker->issue_bootstrap(client);
        REQUIRE(ticket.ticket);
        auto connected = broker->redeem_bootstrap(ticket.ticket->ticket_id,
                                                  ticket.ticket->secret.bytes(), client);
        REQUIRE(connected.client);
        client_identity = *connected.client;

        ControlManifest manifest;
        manifest.profile = ControlBuildProfile::DeveloperLocal;
        manifest.target = "ToolkitFixture";
        manifest.product_name = "Toolkit Fixture";
        manifest.bundle_id = "com.example.toolkit-fixture";
        manifest.build_id = "build:0123456789abcdef0123456789abcdef";
        manifest.endpoint_included = true;
        manifest.capabilities = {
            InspectorCapability::SessionDescribe, InspectorCapability::SessionControl,
            InspectorCapability::VendorRead,      InspectorCapability::VendorGesture,
            InspectorCapability::VendorRender,    InspectorCapability::VendorDestructive};
        manifest.vendor_operations = declaration(kVendor);
        const auto validation = validate_control_manifest_detailed(manifest);
        INFO(validation.error);
        REQUIRE(validation.valid);
        auto registered = broker->register_instance(
            host, ControlRegistrationRequest{ControlHostTier::Standalone, "session-kit",
                                             "instance-kit", "publication-kit", std::move(manifest),
                                             std::string(64, 'a')});
        REQUIRE(registered.registration);
        registration = *registered.registration;
        auto issued = broker->issue_grant(
            client,
            ControlGrantRequest{.client_id = client_identity.client_id,
                                .registration_id = registration.registration_id,
                                .capabilities = {InspectorCapability::VendorRead,
                                                 InspectorCapability::VendorGesture,
                                                 InspectorCapability::VendorRender,
                                                 InspectorCapability::VendorDestructive},
                                .ttl = 5min},
            {.approved = true,
             .authority = ControlConsentAuthority::TrustedPulpCli,
             .decision_id = "toolkit-consent"});
        REQUIRE(issued.grant);
        grant = *issued.grant;

        REQUIRE(add_control_agent_toolkit(*handlers, store,
                                          {.vendor_id = std::string(kVendor),
                                           .presets = presets,
                                           .render_factory = &create_synth,
                                           .render_sample_rate = 48000.0,
                                           .render_channels = 2}));
        executor = make_control_vendor_operation_executor(handlers);
    }

    ~ToolkitFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(store_path, ignored);
        std::filesystem::remove_all(store_path.string() + "-artifacts", ignored);
        std::filesystem::remove_all(presets->user_presets_dir(), ignored);
    }

    struct Outcome {
        ControlAdmissionStatus admission = ControlAdmissionStatus::InvalidRequest;
        ControlReceiptState state = ControlReceiptState::Failed;
        std::string explanation;
        choc::value::Value detail;
    };

    /// Admits, executes, and completes one operation through the broker, so
    /// arguments and results are checked against the declared schemas.
    Outcome call(std::string_view name, std::string params) {
        const auto request_id = "request-" + std::to_string(++requests);
        ControlRequestEnvelope request{
            .request_id = request_id,
            .client_id = client_identity.client_id.value,
            .registration_id = registration.registration_id.value,
            .grant_id = grant.grant_id.value,
            .instance_generation = registration.publication_id,
            .operation_id = "dev." + std::string(kVendor) + "/" + std::string(name) + "@1",
            .operation_version = 1,
            .idempotency_key = "idempotency-" + request_id,
            .deadline_unix_ms = 4102444800000,
            .params_json = std::move(params),
        };
        request.request_hash = control_request_hash(request).value_or("");
        Outcome outcome;
        const auto admitted = broker->admit_operation(client, request);
        outcome.admission = admitted.status;
        outcome.explanation = std::string(control_admission_status_id(admitted.status));
        if (!admitted.plan)
            return outcome;
        REQUIRE(broker->begin_operation(client, *admitted.plan).succeeded());
        ControlExecutionContext context;
        context.checkpoint = [] { return ControlExecutionCheckpoint::Continue; };
        auto executed = executor(*admitted.plan, request, context);
        outcome.explanation = executed.result.explanation;
        const auto finished = broker->finish_operation(client, *admitted.plan,
                                                       executed.terminal_state, executed.result);
        INFO(executed.result.explanation);
        INFO(executed.result.detail_json);
        REQUIRE(finished.succeeded());
        outcome.state = finished.receipt->state;
        if (outcome.state == ControlReceiptState::Completed)
            outcome.detail = choc::json::parse(finished.receipt->result.detail_json);
        return outcome;
    }
};

double num(const choc::value::ValueView& value) {
    return value.isFloat64() ? value.getFloat64() : static_cast<double>(value.getInt64());
}

double value_of(const choc::value::Value& detail, pulp::state::ParamID id) {
    for (const auto entry : detail["values"])
        if (entry["id"].getInt64() == static_cast<std::int64_t>(id))
            return num(entry["value"]);
    FAIL("parameter missing from result");
    return 0.0;
}

} // namespace

TEST_CASE("the agent tool kit declaration matches the handlers it binds",
          "[inspect][control][vendor][toolkit]") {
    ControlVendorOperationsDiagnostics diagnostics;
    const auto table =
        parse_control_vendor_operations(declaration(kVendor), "com.example.toolkit", &diagnostics);
    INFO(diagnostics.error);
    REQUIRE(table);
    std::set<std::string> declared;
    for (const auto& operation : table->operations())
        declared.insert(operation.id);
    std::set<std::string> named;
    for (const auto name : control_agent_toolkit_operation_names())
        named.insert("dev.com.example/" + std::string(name) + "@1");
    CHECK(declared == named);

    pulp::state::StateStore store;
    ToolkitSynth processor;
    processor.define_parameters(store);
    ControlVendorOperationHandlers handlers;
    CHECK_FALSE(add_control_agent_toolkit(handlers, store, {.vendor_id = "pulp.x"}));
    REQUIRE(add_control_agent_toolkit(handlers, store, {.vendor_id = "com.example"}));
    // Without a preset manager or render factory only the patch operations bind.
    const auto bound = handlers.operation_ids();
    CHECK(bound == std::vector<std::string>{
                       "dev.com.example/patch.get@1", "dev.com.example/patch.randomize@1",
                       "dev.com.example/patch.schema@1", "dev.com.example/patch.set@1"});
}

TEST_CASE("agent tool kit patch operations run through the broker",
          "[inspect][control][vendor][toolkit][broker]") {
    ToolkitFixture kit;

    const auto schema = kit.call("patch.schema", "{}");
    INFO(schema.explanation);
    REQUIRE(schema.state == ControlReceiptState::Completed);
    REQUIRE(schema.detail["parameters"].size() == 3);
    CHECK(schema.detail["parameters"][1]["unit"].getString() == "Hz");
    CHECK(num(schema.detail["parameters"][1]["max"]) == 20000.0);

    const auto get = kit.call("patch.get", R"({"ids":[1]})");
    REQUIRE(get.state == ControlReceiptState::Completed);
    CHECK(value_of(get.detail, kGain) == 0.5);
    CHECK(kit.call("patch.get", R"({"ids":[99]})").state == ControlReceiptState::Failed);

    const auto set =
        kit.call("patch.set", R"({"values":[{"id":1,"value":0.8},{"id":2,"value":99999}]})");
    REQUIRE(set.state == ControlReceiptState::Completed);
    CHECK(std::abs(value_of(set.detail, kGain) - 0.8) < 1e-6);
    CHECK(value_of(set.detail, kTone) == 20000.0); // clamped to the range
    // One unknown id rejects the whole write.
    const auto partial =
        kit.call("patch.set", R"({"values":[{"id":1,"value":0.1},{"id":42,"value":1}]})");
    CHECK(partial.state == ControlReceiptState::Failed);
    CHECK(std::abs(kit.store.get_value(kGain) - 0.8f) < 1e-6f);
    CHECK(kit.call("patch.set", R"({"values":[]})").admission ==
          ControlAdmissionStatus::InvalidRequest);

    const auto first = kit.call("patch.randomize", R"({"seed":7})");
    REQUIRE(first.state == ControlReceiptState::Completed);
    CHECK(first.detail["seed"].getInt64() == 7);
    // The bypass parameter is never randomized.
    CHECK(first.detail["values"].size() == 2);
    CHECK(kit.store.get_value(kBypass) == 0.0f);
    const auto gain_after_first = kit.store.get_value(kGain);
    kit.call("patch.set", R"({"values":[{"id":1,"value":0.0}]})");
    const auto again = kit.call("patch.randomize", R"({"seed":7})");
    REQUIRE(again.state == ControlReceiptState::Completed);
    CHECK(kit.store.get_value(kGain) == gain_after_first);
    const auto half = kit.call("patch.randomize", R"({"seed":7,"amount":0,"ids":[1]})");
    REQUIRE(half.state == ControlReceiptState::Completed);
    CHECK(kit.store.get_value(kGain) == gain_after_first);
}

TEST_CASE("agent tool kit preset operations save, list and load through the broker",
          "[inspect][control][vendor][toolkit][broker]") {
    ToolkitFixture kit;
    kit.call("patch.set", R"({"values":[{"id":1,"value":0.9}]})");
    const auto saved = kit.call("presets.save", R"({"name":"Bright Lead","folder":"Leads"})");
    INFO(saved.explanation);
    REQUIRE(saved.state == ControlReceiptState::Completed);

    for (const auto* params : {R"({"name":"../escape"})", R"({"name":".hidden"})",
                               R"({"name":"ok","folder":"../up"})", R"({"name":"a/b"})"}) {
        INFO(params);
        CHECK(kit.call("presets.save", params).state == ControlReceiptState::Failed);
    }
    CHECK_FALSE(
        std::filesystem::exists(kit.presets->user_presets_dir().parent_path() / "escape.json"));

    const auto listed = kit.call("presets.list", "{}");
    REQUIRE(listed.state == ControlReceiptState::Completed);
    bool found = false;
    for (const auto preset : listed.detail["presets"])
        found = found || (preset["name"].getString() == "Bright Lead" &&
                          preset["folder"].getString() == "Leads" && !preset["factory"].getBool());
    CHECK(found);

    kit.call("patch.set", R"({"values":[{"id":1,"value":0.1}]})");
    const auto loaded = kit.call("presets.load", R"({"name":"Bright Lead","folder":"Leads"})");
    INFO(loaded.explanation);
    REQUIRE(loaded.state == ControlReceiptState::Completed);
    CHECK(std::abs(kit.store.get_value(kGain) - 0.9f) < 1e-6f);
    CHECK(kit.call("presets.load", R"({"name":"Missing"})").state == ControlReceiptState::Failed);
}

TEST_CASE("agent tool kit renders notes and test signals offline through the broker",
          "[inspect][control][vendor][toolkit][broker][render]") {
    ToolkitFixture kit;
    kit.call("patch.set", R"({"values":[{"id":1,"value":1.0}]})");

    const auto silent = kit.call("notes.play", R"({"duration_seconds":0.25,"notes":[]})");
    REQUIRE(silent.state == ControlReceiptState::Completed);
    CHECK(silent.detail["frames"].getInt64() == 12000);
    CHECK(num(silent.detail["peak_dbfs"]) <= -199.0);

    const auto played = kit.call(
        "notes.play",
        R"({"duration_seconds":0.5,"notes":[{"note":69,"velocity":100,"start_seconds":0,"duration_seconds":0.4}]})");
    INFO(played.explanation);
    REQUIRE(played.state == ControlReceiptState::Completed);
    CHECK(std::abs(num(played.detail["peak_dbfs"])) < 0.1);
    CHECK(played.detail["clipped_samples"].getInt64() >= 0);
    CHECK(num(played.detail["loudness_lufs_ungated"]) > -10.0);
    // A 440 Hz tone puts the most energy in the 500 Hz octave band.
    double best_level = -1000.0, best_hz = 0.0;
    for (const auto band : played.detail["bands"])
        if (num(band["level_db"]) > best_level) {
            best_level = num(band["level_db"]);
            best_hz = num(band["hz"]);
        }
    CHECK(best_hz == 500.0);
    // The live patch is rendered, not the processor defaults.
    kit.call("patch.set", R"({"values":[{"id":1,"value":0.5}]})");
    const auto quieter = kit.call(
        "notes.play",
        R"({"duration_seconds":0.5,"notes":[{"note":69,"start_seconds":0,"duration_seconds":0.4}]})");
    REQUIRE(quieter.state == ControlReceiptState::Completed);
    CHECK(std::abs(num(quieter.detail["peak_dbfs"]) + 6.02) < 0.1);
    // An automation lane silences the second half.
    const auto lane = kit.call(
        "notes.play",
        R"({"duration_seconds":0.5,"notes":[{"note":69,"start_seconds":0,"duration_seconds":0.5}],"lanes":[{"id":1,"points":[{"time_seconds":0,"value":0.5},{"time_seconds":0.25,"value":0}]}]})");
    REQUIRE(lane.state == ControlReceiptState::Completed);
    const auto held = kit.call(
        "notes.play",
        R"({"duration_seconds":0.5,"notes":[{"note":69,"start_seconds":0,"duration_seconds":0.5}]})");
    REQUIRE(held.state == ControlReceiptState::Completed);
    // Muting half of the same note removes half its energy: 3 dB.
    CHECK(std::abs(num(lane.detail["rms_dbfs"]) - (num(held.detail["rms_dbfs"]) - 3.01)) < 0.1);
    CHECK(
        kit.call(
               "notes.play",
               R"({"duration_seconds":0.1,"notes":[],"lanes":[{"id":77,"points":[{"time_seconds":0,"value":1}]}]})")
            .state == ControlReceiptState::Failed);
    CHECK(kit.call("notes.play", R"({"duration_seconds":60,"notes":[]})").admission ==
          ControlAdmissionStatus::InvalidRequest);

    const auto sine = kit.call(
        "render.analyze",
        R"({"duration_seconds":0.5,"stimulus":"sine","frequency_hz":4000,"level_dbfs":-6})");
    REQUIRE(sine.state == ControlReceiptState::Completed);
    CHECK(std::abs(num(sine.detail["peak_dbfs"]) + 12.0) < 0.2); // -6 dB in, gain 0.5
    CHECK(num(sine.detail["spectral_centroid_hz"]) > 2000.0);
    const auto noise_a =
        kit.call("render.analyze", R"({"duration_seconds":0.2,"stimulus":"noise","seed":3})");
    const auto noise_b =
        kit.call("render.analyze", R"({"duration_seconds":0.2,"stimulus":"noise","seed":3})");
    REQUIRE(noise_a.state == ControlReceiptState::Completed);
    CHECK(num(noise_a.detail["rms_dbfs"]) == num(noise_b.detail["rms_dbfs"]));
    CHECK(kit.call("render.analyze", R"({"duration_seconds":0.2,"stimulus":"square"})").admission ==
          ControlAdmissionStatus::InvalidRequest);
}
