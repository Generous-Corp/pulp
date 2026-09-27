#pragma once

#include "allpass_processor.hpp"
#include <algorithm>
#include <atomic>
#include <choc/text/choc_JSON.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <pulp/audio/audio_file.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/control_sample_region_target.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/runtime/crypto.hpp>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

namespace pulp::examples::control_allpass {
inline constexpr double sample_rate = 48000;
inline constexpr int block_size = 64;
inline std::atomic<bool> stopping{false};
// Borrow the authority passed by the canonical host. A second counter here
// would miss topology-only edits, which deliberately do not change StateStore.
inline inspect::ControlSampleRegionGeneration* generation = nullptr;
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
    auto target =
        inspect::ControlSampleRegionTarget::editable(allpass->graph(), store, target_generation);
    if (target)
        generation = &target_generation;
    return target;
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
    if (target) {
        target->set_preparation_context(sample_rate, block_size);
        generation = &target_generation;
    }
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

inline bool write_atomic(const std::filesystem::path& path, const char* bytes, std::size_t size) {
    const auto temporary = std::filesystem::path(path.string() + ".partial");
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(bytes, static_cast<std::streamsize>(size));
    stream.close();
    if (!stream.good())
        return false;
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

inline bool write_atomic(const std::filesystem::path& path, const std::string& bytes) {
    return write_atomic(path, bytes.data(), bytes.size());
}

struct RenderSnapshot {
    std::filesystem::path directory;
    std::uint64_t graph_generation = 0;
    std::uint64_t state_generation = 0;
    double coefficient = 0;
    std::string graph_json;
};

inline std::filesystem::path artifact_directory(bool frozen) {
    const auto* configured = std::getenv("PULP_SAMPLE_REGION_ARTIFACT_DIR");
    if (!configured || !*configured)
        return {};
    return std::filesystem::path(configured) / (frozen ? "frozen" : "editable");
}

// This file is observed only in the explicitly configured diagnostic directory.
// Removing it before capture lets a later request remain pending for the next
// block-boundary capture, even when the graph and state generations are stable.
inline bool consume_capture_request(const std::filesystem::path& directory) {
    if (directory.empty())
        return false;
    std::error_code error;
    const bool requested = std::filesystem::remove(directory / "capture.request", error);
    if (error)
        throw std::runtime_error("could not consume sample-region capture request");
    return requested;
}

inline std::optional<RenderSnapshot> render_snapshot(format::HeadlessHost& app, bool frozen,
                                                     std::filesystem::path directory = {}) {
    if (!generation)
        return std::nullopt;
    if (directory.empty())
        directory = artifact_directory(frozen);
    if (directory.empty())
        return std::nullopt;
    RenderSnapshot result{.directory = std::move(directory),
                          .graph_generation = generation->value,
                          .state_generation = app.state().state_generation(),
                          .coefficient = app.state().get_value(kAllpassCoefficient)};
    if (!frozen) {
        const auto* processor = dynamic_cast<SampleRegionAllpassProcessor*>(app.processor());
        if (!processor)
            return std::nullopt;
        result.graph_json = host::GraphSerializer::to_json(processor->graph());
    }
    return result;
}

inline std::optional<choc::value::Value> render_identity() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> bytes(size);
    if (size == 0 || _NSGetExecutablePath(bytes.data(), &size) != 0)
        return std::nullopt;
    const auto executable = std::filesystem::canonical(bytes.data());
    const auto digest = runtime::sha256_file_hex(executable, 1024ULL * 1024ULL * 1024ULL);
    std::ifstream stream(executable.string() + ".inspector-capabilities.json", std::ios::binary);
    const std::string manifest_bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    const auto manifest = inspect::parse_control_manifest(manifest_bytes);
    proc_bsdinfo process{};
    if (!digest || !manifest ||
        proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &process, sizeof(process)) != sizeof(process))
        return std::nullopt;
    auto identity = choc::value::createObject("SampleRegionProducer");
    identity.setMember("process_id", static_cast<std::int64_t>(getpid()));
    identity.setMember("process_start_id", std::to_string(process.pbi_start_tvsec) + ":" +
                                               std::to_string(process.pbi_start_tvusec));
    identity.setMember("executable", executable.string());
    identity.setMember("executable_sha256", *digest);
    identity.setMember("build_id", manifest->build_id);
    identity.setMember("plugin_id", manifest->bundle_id);
    identity.setMember("manifest_sha256", runtime::sha256_hex(manifest_bytes));
    return identity;
#else
    return std::nullopt;
#endif
}

inline bool write_render_evidence(format::HeadlessHost& app, bool frozen,
                                  const RenderSnapshot& snapshot,
                                  const choc::value::Value& identity,
                                  std::uint64_t continuous_blocks, std::uint64_t capture_sequence) {
    try {
        const auto& root = snapshot.directory;
        std::filesystem::create_directories(root);
        const auto stem = "graph-" + std::to_string(snapshot.graph_generation) + "-state-" +
                          std::to_string(snapshot.state_generation);
        const auto capture_stem = stem + "-capture-" + std::to_string(capture_sequence);
        audio::AudioFileData data;
        data.sample_rate = static_cast<std::uint32_t>(sample_rate);
        data.channels.resize(1);
        data.channels[0].reserve(4096);
        audio::Buffer<float> input(1, block_size), output(1, block_size);
        format::ProcessContext context;
        // The editable example currently does not forward reset_requested.
        // Settle the bounded allpass instead, and record that precondition;
        // these captures do not claim reset forwarding or realtime scheduling.
        constexpr int settling_frames = 16384;
        if (!std::isfinite(snapshot.coefficient) || std::abs(snapshot.coefficient) > 0.991)
            return false;
        double settling_peak = 0;
        input.clear();
        for (int offset = 0; offset < settling_frames; offset += block_size) {
            auto out = output.view();
            app.process(out, std::as_const(input).view(), context);
            for (const auto value : out.channel(0)) {
                if (!std::isfinite(value))
                    return false;
                if (offset == settling_frames - block_size)
                    settling_peak = std::max(settling_peak, std::abs(static_cast<double>(value)));
            }
        }
        if (settling_peak > 1.0e-7)
            return false;
        for (int offset = 0; offset < 4096; offset += block_size) {
            input.clear();
            if (offset == 0)
                input.channel(0)[0] = 1.0f;
            auto out = output.view();
            app.process(out, std::as_const(input).view(), context);
            for (const auto value : out.channel(0))
                if (!std::isfinite(value))
                    return false;
            data.channels[0].insert(data.channels[0].end(), out.channel(0).begin(),
                                    out.channel(0).end());
        }
        const auto wav = root / (capture_stem + ".wav");
        const auto pending_wav = root / (capture_stem + ".wav.partial");
        if (!audio::write_wav_file(pending_wav.string(), data, audio::WavBitDepth::Float32))
            return false;
        std::filesystem::rename(pending_wav, wav);
        const auto wav_digest = runtime::sha256_file_hex(wav, 1024 * 1024);
        if (!wav_digest)
            return false;
        auto receipt = choc::value::createObject("SampleRegionRender");
        receipt.setMember("schema", "pulp.sample-region.control-render.v1");
        receipt.setMember("producer", identity);
        receipt.setMember("capture_sequence", static_cast<std::int64_t>(capture_sequence));
        if (frozen) {
            if (!write_atomic(root / "allpass.pulpbake",
                              reinterpret_cast<const char*>(frozen_bytes.data()),
                              frozen_bytes.size()) ||
                !write_atomic(root / "publisher.pub",
                              reinterpret_cast<const char*>(frozen_public_key.data()),
                              frozen_public_key.size()))
                return false;
            receipt.setMember("bake", "allpass.pulpbake");
            receipt.setMember("bake_sha256",
                              runtime::sha256_hex(frozen_bytes.data(), frozen_bytes.size()));
        } else {
            if (!write_atomic(root / (capture_stem + ".pulpgraph"), snapshot.graph_json))
                return false;
            receipt.setMember("graph", capture_stem + ".pulpgraph");
            receipt.setMember("graph_sha256", runtime::sha256_hex(snapshot.graph_json));
        }
        receipt.setMember("graph_generation", static_cast<std::int64_t>(snapshot.graph_generation));
        receipt.setMember("state_generation", static_cast<std::int64_t>(snapshot.state_generation));
        receipt.setMember("coefficient", snapshot.coefficient);
        receipt.setMember("sample_rate", static_cast<std::int64_t>(sample_rate));
        receipt.setMember("block_size", block_size);
        receipt.setMember("frames", 4096);
        receipt.setMember("settling_frames", settling_frames);
        receipt.setMember("settling_peak", settling_peak);
        receipt.setMember("reset_requested", false);
        receipt.setMember("continuous_blocks", static_cast<std::int64_t>(continuous_blocks));
        receipt.setMember("render", capture_stem + ".wav");
        receipt.setMember("render_sha256", *wav_digest);
        const auto json = choc::json::toString(receipt);
        // A complete JSON is the publication barrier for all its hashed files.
        return write_atomic(root / (stem + ".json"), json);
    } catch (...) {
        return false;
    }
}

/// One processing owner; capture handoff waits at a block boundary. Main keeps
/// pumping control while ordinary blocks run, but pauses mutations while an
/// exact-generation capture is being written by the processing owner.
class RenderWorker {
  public:
    RenderWorker(format::HeadlessHost& app, bool frozen)
        : app_(app), frozen_(frozen), thread_([this] { run(); }) {}
    ~RenderWorker() {
        stop();
    }
    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    bool capture(RenderSnapshot snapshot) {
        std::unique_lock lock(mutex_);
        if (stopped_ || failed_)
            return false;
        completed_ = false;
        pending_ = std::move(snapshot);
        changed_.notify_all();
        changed_.wait(lock, [this] { return completed_ || failed_ || stopped_; });
        return completed_ && !failed_;
    }
    bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }
    std::uint64_t blocks() const noexcept {
        return blocks_.load(std::memory_order_acquire);
    }
    std::uint64_t capture_sequence() const noexcept {
        return capture_sequence_.load(std::memory_order_acquire);
    }
    void stop() noexcept {
        stopped_.store(true, std::memory_order_release);
        changed_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

  private:
    void run() noexcept {
        try {
            audio::Buffer<float> input(1, block_size), output(1, block_size);
            std::optional<choc::value::Value> identity;
            while (!stopped_.load(std::memory_order_acquire)) {
                std::optional<RenderSnapshot> snapshot;
                {
                    std::lock_guard lock(mutex_);
                    snapshot = std::move(pending_);
                    pending_.reset();
                }
                if (snapshot) {
                    if (!identity)
                        identity = render_identity();
                    const bool ok =
                        identity && write_render_evidence(app_, frozen_, *snapshot, *identity,
                                                          blocks(), capture_sequence() + 1);
                    {
                        std::lock_guard lock(mutex_);
                        failed_.store(!ok, std::memory_order_release);
                        if (ok)
                            capture_sequence_.fetch_add(1, std::memory_order_release);
                        completed_ = true;
                    }
                    changed_.notify_all();
                    if (!ok)
                        return;
                    continue;
                }
                input.clear();
                if (blocks() % 16 == 0)
                    input.channel(0)[0] = 0.25f;
                auto out = output.view();
                app_.process(out, std::as_const(input).view());
                for (const auto value : out.channel(0))
                    if (!std::isfinite(value))
                        throw std::runtime_error("non-finite continuous allpass output");
                blocks_.fetch_add(1, std::memory_order_release);
                std::unique_lock lock(mutex_);
                changed_.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return stopped_.load(std::memory_order_acquire) || pending_.has_value();
                });
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            failed_.store(true, std::memory_order_release);
        }
        changed_.notify_all();
    }
    format::HeadlessHost& app_;
    bool frozen_;
    std::atomic<bool> stopped_{false}, failed_{false};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> capture_sequence_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<RenderSnapshot> pending_;
    bool completed_ = false;
    std::thread thread_;
};

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
    RenderWorker renderer(app, frozen);
    const auto directory = artifact_directory(frozen);
    std::uint64_t rendered_graph = 0, rendered_state = 0;
    while (!stopping.load(std::memory_order_relaxed) && control->ready()) {
        control->poll();
        if (renderer.failed()) {
            renderer.stop();
            control->stop();
            generation = nullptr;
            return 69;
        }
        const bool requested = consume_capture_request(directory);
        if (generation && (requested || rendered_graph != generation->value ||
                           rendered_state != app.state().state_generation())) {
            const auto snapshot = render_snapshot(app, frozen, directory);
            if (snapshot && !renderer.capture(*snapshot)) {
                renderer.stop();
                control->stop();
                generation = nullptr;
                return 69;
            }
            rendered_graph = generation->value;
            rendered_state = app.state().state_generation();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    renderer.stop();
    control->stop();
    generation = nullptr;
    frozen_prepared = false;
    return 0;
}
} // namespace pulp::examples::control_allpass
