#pragma once

#include "allpass_processor.hpp"
#include <atomic>
#include <choc/text/choc_JSON.h>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pulp/audio/audio_file.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/inspect/control_sample_region_target.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/runtime/crypto.hpp>
#include <thread>
#include <utility>

namespace pulp::examples::control_allpass {
inline constexpr double sample_rate = 48000;
inline constexpr int block_size = 64;
inline std::atomic<bool> stopping{false};
inline inspect::ControlSampleRegionGeneration generation;
inline std::optional<host::BakedPlan> admitted_plan;
inline std::vector<host::SampleRegionProof> admitted_proofs;
inline format::Processor* frozen_processor = nullptr;
inline bool frozen_prepared = false;
inline std::vector<std::uint8_t> frozen_bytes, frozen_public_key;

inline void stop(int) {
    stopping.store(true, std::memory_order_relaxed);
}

inline std::shared_ptr<inspect::ControlSampleRegionTarget>
editable_target(format::Processor& processor, state::StateStore& store,
                inspect::ControlSampleRegionGeneration& target_generation) {
    auto* allpass = dynamic_cast<SampleRegionAllpassProcessor*>(&processor);
    if (!allpass || !allpass->ready())
        return {};
    return inspect::ControlSampleRegionTarget::editable(allpass->graph(), store, target_generation);
}

inline std::unique_ptr<format::Processor> create_frozen() {
    format::HeadlessHost source(&create_sample_region_allpass);
    source.prepare(sample_rate, block_size, 1, 1);
    const auto* allpass = dynamic_cast<SampleRegionAllpassProcessor*>(source.processor());
    if (!allpass || !allpass->ready())
        return {};
    const auto lowered = host::bake_to_plan(allpass->graph());
    if (!lowered.accepted || !lowered.plan)
        return {};
    const auto seed = runtime::secure_random_bytes(32);
    if (!seed)
        return {};
    const auto key = runtime::ed25519_keypair_from_seed(seed->data(), seed->size());
    if (!key)
        return {};
    const auto bytes = host::write_baked_signed(*lowered.plan, key->private_key);
    host::BakedTrust trust;
    trust.trusted_public_keys.push_back(key->public_key);
    auto validated = host::load_baked_plan(bytes, trust, {});
    if (!validated.accepted || !validated.plan || *validated.plan != *lowered.plan)
        return {};
    auto executable = host::load_baked(bytes, trust, host::BakedTypeRegistry::from({}));
    if (!executable.accepted || !executable.processor)
        return {};
    frozen_bytes = bytes;
    frozen_public_key = key->public_key;
    admitted_plan = std::move(validated.plan);
    admitted_proofs.clear();
    for (const auto& region : admitted_plan->sample_regions) {
        auto proof = allpass->graph().prove_sample_region(region.region_id);
        if (!proof.accepted)
            return {};
        admitted_proofs.push_back(std::move(proof));
    }
    frozen_processor = executable.processor.get();
    return std::move(executable.processor);
}

inline std::shared_ptr<inspect::ControlSampleRegionTarget>
frozen_target(format::Processor& processor, state::StateStore& store,
              inspect::ControlSampleRegionGeneration& target_generation) {
    if (&processor != frozen_processor || !admitted_plan || !frozen_prepared)
        return {};
    auto target = inspect::ControlSampleRegionTarget::frozen(
        *admitted_plan, store, target_generation,
        [](std::uint32_t id, int maximum) {
            for (const auto& proof : admitted_proofs)
                if (proof.region_id == id && maximum == block_size)
                    return proof;
            return host::SampleRegionProof{};
        },
        [] { return frozen_prepared; });
    if (target)
        target->set_preparation_context(sample_rate, block_size);
    return target;
}

inline bool prepared_allpass(format::HeadlessHost& app) {
    audio::Buffer<float> input(1, block_size), output(1, block_size);
    input.clear();
    input.channel(0)[0] = 1.0f;
    const auto in = std::as_const(input).view();
    auto out = output.view();
    format::ProcessContext context;
    context.reset_requested = true;
    app.process(out, in, context);
    const double coefficient = app.state().get_value(kAllpassCoefficient);
    double previous_input = 0, previous_output = 0;
    for (int frame = 0; frame < block_size; ++frame) {
        const double value = frame == 0 ? 1 : 0;
        const double expected =
            coefficient * value + previous_input - coefficient * previous_output;
        if (!std::isfinite(out.channel(0)[frame]) ||
            std::abs(out.channel(0)[frame] - expected) > 1.0e-6)
            return false;
        previous_input = value;
        previous_output = expected;
    }
    input.clear();
    app.process(out, std::as_const(input).view(), context);
    return true;
}

inline bool write_render_evidence(format::HeadlessHost& app, bool frozen) {
    const auto* directory = std::getenv("PULP_SAMPLE_REGION_ARTIFACT_DIR");
    if (!directory || !*directory)
        return true;
    try {
        const auto root = std::filesystem::path(directory) / (frozen ? "frozen" : "editable");
        std::filesystem::create_directories(root);
        const auto stem = "graph-" + std::to_string(generation.value) + "-state-" +
                          std::to_string(app.state().state_generation());
        audio::AudioFileData data;
        data.sample_rate = static_cast<std::uint32_t>(sample_rate);
        data.channels.resize(1);
        audio::Buffer<float> input(1, block_size), output(1, block_size);
        format::ProcessContext context;
        context.reset_requested = true;
        for (int offset = 0; offset < 4096; offset += block_size) {
            input.clear();
            if (offset == 0)
                input.channel(0)[0] = 1.0f;
            auto out = output.view();
            app.process(out, std::as_const(input).view(), context);
            context.reset_requested = false;
            data.channels[0].insert(data.channels[0].end(), out.channel(0).begin(),
                                    out.channel(0).end());
        }
        if (!audio::write_wav_file((root / (stem + ".wav")).string(), data,
                                   audio::WavBitDepth::Float32))
            return false;
        const auto write = [&](const std::string& name, const char* bytes, std::size_t size) {
            std::ofstream stream(root / name, std::ios::binary);
            stream.write(bytes, static_cast<std::streamsize>(size));
            stream.close();
            return stream.good();
        };
        if (frozen) {
            if (!write("allpass.pulpbake", reinterpret_cast<const char*>(frozen_bytes.data()),
                       frozen_bytes.size()) ||
                !write("publisher.pub", reinterpret_cast<const char*>(frozen_public_key.data()),
                       frozen_public_key.size()))
                return false;
        } else {
            const auto* processor = dynamic_cast<SampleRegionAllpassProcessor*>(app.processor());
            if (!processor)
                return false;
            const auto graph_json = host::GraphSerializer::to_json(processor->graph());
            if (!write(stem + ".pulpgraph", graph_json.data(), graph_json.size()))
                return false;
        }
        auto receipt = choc::value::createObject("SampleRegionRender");
        receipt.setMember("graph_generation", static_cast<std::int64_t>(generation.value));
        receipt.setMember("state_generation",
                          static_cast<std::int64_t>(app.state().state_generation()));
        receipt.setMember("coefficient", app.state().get_value(kAllpassCoefficient));
        receipt.setMember("render", stem + ".wav");
        const auto json = choc::json::toString(receipt);
        return write(stem + ".json", json.data(), json.size());
    } catch (...) {
        return false;
    }
}

/// Uses the ordinary Standalone control bridge with the headless audio adapter,
/// so broker proof does not depend on an available physical audio device.
inline int run(format::ProcessorFactory factory,
               inspect::detail::StandaloneSampleRegionTargetFactory target_factory, bool frozen) {
    if (!inspect::detail::install_standalone_sample_region_target_factory(target_factory))
        return 64;
    format::HeadlessHost app(factory);
    if (!app.valid() || !app.processor())
        return 65;
    app.prepare(sample_rate, block_size, 1, 1);
    if (!prepared_allpass(app))
        return 68;
    if (frozen)
        frozen_prepared = true;
    else {
        const auto* processor = dynamic_cast<SampleRegionAllpassProcessor*>(app.processor());
        if (!processor || !processor->ready())
            return 66;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    auto control = inspect::make_control_standalone_host();
    if (!control || !control->start(*app.processor(), app.state(), nullptr, sample_rate))
        return 67;
    std::uint64_t rendered_graph = 0, rendered_state = 0;
    while (!stopping.load(std::memory_order_relaxed) && control->ready()) {
        control->poll();
        if (rendered_graph != generation.value ||
            rendered_state != app.state().state_generation()) {
            if (!write_render_evidence(app, frozen)) {
                control->stop();
                return 69;
            }
            rendered_graph = generation.value;
            rendered_state = app.state().state_generation();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    control->stop();
    frozen_prepared = false;
    return 0;
}
} // namespace pulp::examples::control_allpass
