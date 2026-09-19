// Installed-SDK proof: only public headers and the shipped example are used.
#include "allpass_processor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_doctor_artifacts.hpp>
#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using pulp::examples::SampleRegionAllpassProcessor;
constexpr double sample_rate = 48000.0;
constexpr int maximum_block = 257;
constexpr double coefficient = 0.5;
constexpr double oracle_tolerance = 1.e-6;
// HeadlessHost's public factory is a function pointer. This one-shot slot is
// consumed synchronously during construction; no render thread reads it.
thread_local std::unique_ptr<pulp::format::Processor> pending_processor;
std::unique_ptr<pulp::format::Processor> take_processor() {
    return std::move(pending_processor);
}
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
std::unique_ptr<pulp::format::HeadlessHost>
make_host(std::unique_ptr<pulp::format::Processor> processor) {
    require(processor != nullptr, "factory returned no Processor");
    require(!pending_processor, "nested consumer host construction");
    pending_processor = std::move(processor);
    auto host = std::make_unique<pulp::format::HeadlessHost>(take_processor);
    require(host->valid(), "HeadlessHost rejected Processor");
    host->prepare(sample_rate, maximum_block, 1, 1);
    if (auto* editable = host->processor_as<SampleRegionAllpassProcessor>())
        require(editable->ready(), std::string(editable->error()));
    require(host->processor()->latency_samples() == 0, "allpass must report zero PDC latency");
    require(host->state().param_count() == 1, "expected one frozen promoted parameter");
    require(host->state().get_value(pulp::examples::kAllpassCoefficient) == 0.5f,
            "coefficient identity/default differs from the canonical allpass");
    return host;
}
void write_bytes(const std::filesystem::path& path, const char* data, std::size_t size) {
    std::ofstream file(path, std::ios::binary);
    require(file.good(), "cannot create " + path.string());
    file.write(data, static_cast<std::streamsize>(size));
    file.close();
    require(file.good(), "cannot finish " + path.string());
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    write_bytes(path, text.data(), text.size());
}
pulp::audio::Buffer<float> buffer(const std::vector<float>& samples) {
    pulp::audio::Buffer<float> result(1, samples.size());
    std::copy(samples.begin(), samples.end(), result.channel(0).begin());
    return result;
}
std::vector<float> render(pulp::format::HeadlessHost& host, const std::vector<float>& input,
                          bool irregular) {
    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<std::uint32_t>(sample_rate);
    data.channels = {input};
    pulp::audio::OfflineRenderOptions options;
    options.fallback_block_size = 64;
    if (irregular) {
        constexpr std::array<int, 8> sizes{1, 127, 3, 64, 17, 257, 5, 31};
        std::size_t frames = 0;
        while (frames < input.size()) {
            const auto size = sizes[options.block_size_schedule.size() % sizes.size()];
            options.block_size_schedule.push_back(size);
            frames += static_cast<std::size_t>(size);
        }
    }
    auto output = host.render_offline(data, options);
    require(output.has_value() && output->channels.size() == 1,
            "HeadlessHost offline render failed");
    require(output->channels[0].size() == input.size(), "render changed frame count");
    return std::move(output->channels[0]);
}
double oracle(const std::vector<float>& input, const std::vector<float>& output) {
    double previous_input = 0, previous_output = 0, maximum_error = 0;
    require(input.size() == output.size(), "oracle shape mismatch");
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double expected =
            coefficient * input[i] + previous_input - coefficient * previous_output;
        require(std::isfinite(output[i]), "non-finite rendered sample");
        maximum_error = std::max(maximum_error, std::abs(output[i] - expected));
        previous_input = input[i];
        previous_output = expected;
    }
    require(maximum_error <= oracle_tolerance, "independent scalar oracle exceeded 1e-6");
    return maximum_error;
}
void wav(const std::filesystem::path& path, const std::vector<float>& samples) {
    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<std::uint32_t>(sample_rate);
    data.channels = {samples};
    require(pulp::audio::write_wav_file(path.string(), data, pulp::audio::WavBitDepth::Float32),
            "failed to write WAV " + path.string());
}
void measurement_sensitivity(const std::vector<float>& input,
                             const std::vector<float>& accepted_render) {
    require(accepted_render.size() > 17, "sensitivity fixture is too short");
    auto changed = accepted_render;
    changed[17] += 0.01f;
    bool rejected = false;
    try {
        (void)oracle(input, changed);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "scalar oracle accepted a planted 0.01 sample error");
    const auto check =
        pulp::test::audio::assert_null_near(buffer(accepted_render), buffer(changed), -180);
    require(!check.passed, "null detector accepted a planted 0.01 sample error");
}
void reject_changed_contract(const std::string& graph_json) {
    const auto rejected = [](const std::string& changed) {
        pulp::state::StateStore store;
        SampleRegionAllpassProcessor processor(changed);
        require(!processor.error().empty(), "constructor admitted noncanonical graph contract");
        processor.define_parameters(store);
        require(store.param_count() == 0, "rejected graph published parameter identities");
    };
    auto renamed = graph_json;
    constexpr auto original = "Allpass coefficient";
    const auto name = renamed.find(original);
    require(name != std::string::npos, "canonical graph parameter name missing");
    renamed.replace(name, std::char_traits<char>::length(original), "Changed coefficient");
    rejected(renamed);
    pulp::host::SignalGraph empty;
    const auto empty_json = pulp::host::GraphSerializer::to_json(empty);
    require(!empty_json.empty(), "empty-graph negative fixture not serialized");
    rejected(empty_json);
}
void lifecycle_recovery() {
    for (bool invalid_prepare_first : {false, true}) {
        pulp::format::HeadlessHost host(pulp::examples::create_sample_region_allpass);
        auto* processor = host.processor_as<SampleRegionAllpassProcessor>();
        require(processor != nullptr, "lifecycle factory failed");
        require(processor->parameter_contract().frozen(), "lifecycle manifest not frozen");
        require(host.state().param_count() == 1, "lifecycle host store registration failed");
        if (invalid_prepare_first) {
            pulp::format::PrepareContext invalid;
            invalid.sample_rate = -1;
            invalid.max_buffer_size = maximum_block;
            invalid.input_channels = invalid.output_channels = 1;
            processor->prepare(invalid);
            require(!processor->ready(), "invalid preparation marked ready");
        }
        host.release();
        host.prepare(sample_rate, maximum_block, 1, 1);
        require(processor->ready(),
                "release-before-prepare recovery failed: " + processor->error());
        require(host.state().param_count() == 1, "recovery duplicated parameter registration");
        const auto first = render(host, {1.f, 0.f}, false);
        require(first[0] == 0.5f && first[1] == 0.75f, "recovered initial state differs");
        host.release();
        host.prepare(sample_rate, maximum_block, 1, 1);
        require(processor->ready(), "prepared lifecycle release/reprepare failed");
        const auto second = render(host, {1.f, 0.f}, false);
        require(second == first, "release/reprepare retained prior stream state");
    }
}
void doctor(const std::filesystem::path& directory) {
    auto host = make_host(std::make_unique<SampleRegionAllpassProcessor>());
    std::vector<float> impulse(16384, 0.f);
    impulse[0] = 1.f;
    const auto output = render(*host, impulse, true);
    oracle(impulse, output);
    require(output[0] == 0.5f && output[1] == 0.75f, "first two analytic impulse terms differ");
    const auto in = buffer(impulse), out = buffer(output);
    constexpr std::array<double, 5> frequencies{50, 200, 1000, 5000, 18000};
    const auto magnitude = pulp::test::audio::response_relative_to_input(in.view(), out.view(),
                                                                         sample_rate, frequencies);
    const auto phase =
        pulp::test::audio::measure_group_delay(in.view(), out.view(), sample_rate, frequencies);
    require(magnitude.checkpoints.size() == frequencies.size(),
            "Doctor omitted magnitude checkpoints");
    require(phase.checkpoints.size() == frequencies.size(), "Doctor omitted phase checkpoints");
    for (const auto& point : magnitude.checkpoints)
        require(std::isfinite(point.magnitude_db) && std::abs(point.magnitude_db) <= 0.02,
                "allpass magnitude differs from unity by more than 0.02 dB");
    for (const auto& point : phase.checkpoints) {
        require(point.defined && point.phase_defined, "Doctor phase/group delay undefined");
        const double omega = 2 * std::numbers::pi * point.hz / sample_rate;
        const auto delay = std::exp(std::complex<double>(0, -omega));
        const auto transfer = (coefficient + delay) / (1.0 + coefficient * delay);
        const double expected_phase = std::arg(transfer);
        const double expected_delay =
            (1 - coefficient * coefficient) /
            (1 + coefficient * coefficient + 2 * coefficient * std::cos(omega));
        require(std::isfinite(point.phase_rad) &&
                    std::abs(std::remainder(point.phase_rad - expected_phase,
                                            2 * std::numbers::pi)) <= 0.01,
                "Doctor phase disagrees with analytic allpass");
        require(std::isfinite(point.group_delay_samples) &&
                    std::abs(point.group_delay_samples - expected_delay) <= 0.05,
                "Doctor group delay disagrees with analytic allpass");
    }
    write_text(directory / "doctor-response.json",
               pulp::test::audio::response_curve_to_json(magnitude, "installed allpass impulse"));
    write_text(directory / "doctor-phase.json",
               pulp::test::audio::phase_curve_to_json(phase, "installed allpass impulse"));
    wav(directory / "impulse-input.wav", impulse);
    wav(directory / "impulse-output.wav", output);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: sample-region-allpass-consumer ARTIFACT_DIRECTORY");
        lifecycle_recovery();
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        auto source = make_host(std::make_unique<SampleRegionAllpassProcessor>());
        auto* editable = source->processor_as<SampleRegionAllpassProcessor>();
        require(editable != nullptr, "editable Processor type missing");
        require(editable->parameter_contract().frozen(), "public parameter contract not frozen");
        const auto graph_json = pulp::host::GraphSerializer::to_json(editable->graph());
        require(!graph_json.empty(), "empty graph serialization");
        reject_changed_contract(graph_json);
        auto reloaded = make_host(std::make_unique<SampleRegionAllpassProcessor>(graph_json));
        auto* reload = reloaded->processor_as<SampleRegionAllpassProcessor>();
        require(pulp::host::GraphSerializer::to_json(reload->graph()) == graph_json,
                "reloaded graph changed structural manifest");
        require(editable->parameter_contract().matches_promoted(reload->parameter_contract()),
                "reload changed promoted manifest");
        const auto plan = pulp::host::bake_to_plan(editable->graph());
        const auto reload_plan = pulp::host::bake_to_plan(reload->graph());
        require(plan.accepted && plan.plan && reload_plan.accepted && reload_plan.plan,
                "graph cannot be baked");
        require(*plan.plan == *reload_plan.plan, "reload changed bake plan");
        // Public fixed fixture seed, not a production signing identity.
        std::array<std::uint8_t, 32> seed{};
        seed.fill(37);
        const auto key = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
        require(key.has_value(), "fixture signing key construction failed");
        const auto bytes = pulp::host::write_baked_signed(*plan.plan, key->private_key);
        require(!bytes.empty(), "signed bake empty");
        pulp::host::BakedTrust trust;
        trust.trusted_public_keys.push_back(key->public_key);
        const auto verified = pulp::host::verify_and_extract_plan(bytes, trust);
        require(verified && *verified == *plan.plan, "signed artifact structural mismatch");
        const auto preflight = pulp::host::load_baked_plan(bytes, trust, {});
        require(preflight.accepted && preflight.plan && *preflight.plan == *plan.plan,
                "signed artifact preflight failed");
        const auto baked_factory = [&]() {
            auto loaded =
                pulp::host::load_baked(bytes, trust, pulp::host::BakedTypeRegistry::from({}));
            require(loaded.accepted && loaded.processor != nullptr, loaded.message);
            return make_host(std::move(loaded.processor));
        };
        auto baked = baked_factory();
        auto irregular_source = make_host(std::make_unique<SampleRegionAllpassProcessor>());
        auto irregular_reload =
            make_host(std::make_unique<SampleRegionAllpassProcessor>(graph_json));
        auto irregular_bake = baked_factory();
        std::vector<float> input(96000);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] =
                static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 440 * i / sample_rate));
        const std::array<std::vector<float>, 6> renders{
            render(*source, input, false),   render(*irregular_source, input, true),
            render(*reloaded, input, false), render(*irregular_reload, input, true),
            render(*baked, input, false),    render(*irregular_bake, input, true)};
        constexpr std::array<const char*, 6> names{"source-regular.wav", "source-irregular.wav",
                                                   "reload-regular.wav", "reload-irregular.wav",
                                                   "bake-regular.wav",   "bake-irregular.wav"};
        double maximum_error = 0;
        for (std::size_t i = 0; i < renders.size(); ++i) {
            maximum_error = std::max(maximum_error, oracle(input, renders[i]));
            const auto check =
                pulp::test::audio::assert_null_near(buffer(renders[0]), buffer(renders[i]), -180);
            require(check.passed, check.message);
            wav(directory / names[i], renders[i]);
        }
        measurement_sensitivity(input, renders[0]);
        auto tampered = bytes;
        tampered.back() ^= 1;
        require(!pulp::host::load_baked(tampered, trust, pulp::host::BakedTypeRegistry::from({}))
                     .accepted,
                "tampered signed bake accepted");
        require(!pulp::host::load_baked(bytes, pulp::host::BakedTrust{},
                                        pulp::host::BakedTypeRegistry::from({}))
                     .accepted,
                "untrusted signed bake accepted");
        require(!pulp::host::load_baked(bytes, trust, {}).accepted,
                "legacy v1 loader admitted v2 sample-region bake");
        write_text(directory / "allpass.pulpgraph", graph_json);
        write_bytes(directory / "allpass.pulpbake", reinterpret_cast<const char*>(bytes.data()),
                    bytes.size());
        doctor(directory);
        std::ostringstream error_number;
        error_number << std::setprecision(17) << maximum_error;
        write_text(
            directory / "consumer-receipt.json",
            "{\"schema\":\"pulp.sample-region-installed-consumer.v1\",\"accepted\":true,"
            "\"sample_rate\":48000,\"maximum_block\":257,\"renders\":6,"
            "\"oracle_tolerance\":1e-6,\"maximum_oracle_error\":" +
                error_number.str() +
                ",\"null_tolerance_dbfs\":-180,\"reported_latency\":0,"
                "\"signed_bake_negative_controls\":3,\"lifecycle_recovery_cases\":2,\"measurement_"
                "negative_controls\":2,\"constructor_negative_controls\":2}\n");
        std::cout << "Installed allpass consumer passed: six renders, scalar/null/Doctor gates, "
                     "signed artifact round trip\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sample-region installed consumer: " << error.what() << '\n';
        return 1;
    }
}
