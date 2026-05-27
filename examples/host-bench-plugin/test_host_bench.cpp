// PulpHostBench — bench plugin unit coverage.
//
// The interesting integration evidence comes from running the plugin
// inside a real DAW; these tests just confirm the lifecycle hooks all
// fire and the log file gets written so the per-DAW scripts have
// something to chew on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "host_bench.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

using namespace pulp;
using namespace pulp::examples;

namespace {

// Force log writes into a per-test tmp dir so we don't pollute the
// real `~/Library/Logs/PulpHostBench/`.
class ScopedBenchLogDir {
public:
    ScopedBenchLogDir() {
        dir_ = std::filesystem::temp_directory_path() /
               ("pulp-host-bench-test-" + std::to_string(current_pid()) + "-" +
                std::to_string(::time(nullptr)));
        std::filesystem::create_directories(dir_);
#if defined(__APPLE__)
        prev_ = std::getenv("HOME") ? std::getenv("HOME") : "";
        setenv("HOME", dir_.string().c_str(), 1);
#elif defined(_WIN32)
        prev_ = std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : "";
        _putenv_s("LOCALAPPDATA", dir_.string().c_str());
#else
        prev_ = std::getenv("XDG_STATE_HOME") ? std::getenv("XDG_STATE_HOME") : "";
        setenv("XDG_STATE_HOME", dir_.string().c_str(), 1);
#endif
    }
    ~ScopedBenchLogDir() {
#if defined(__APPLE__)
        if (prev_.empty()) unsetenv("HOME"); else setenv("HOME", prev_.c_str(), 1);
#elif defined(_WIN32)
        if (prev_.empty()) _putenv_s("LOCALAPPDATA", ""); else _putenv_s("LOCALAPPDATA", prev_.c_str());
#else
        if (prev_.empty()) unsetenv("XDG_STATE_HOME"); else setenv("XDG_STATE_HOME", prev_.c_str(), 1);
#endif
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path root() const { return dir_; }

private:
    static int current_pid() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(::getpid());
#endif
    }

    std::filesystem::path dir_;
    std::string prev_;
};

struct BenchFixture {
    ScopedBenchLogDir tmp;
    state::StateStore store;
    std::unique_ptr<HostBenchProcessor> processor;

    BenchFixture() {
        processor = std::make_unique<HostBenchProcessor>("Standalone");
        processor->set_state_store(&store);
        processor->define_parameters(store);
    }

    void prepare(double sr = 48000.0, int n = 512) {
        format::PrepareContext ctx{sr, n, 2, 2};
        processor->prepare(ctx);
    }
    void process(int n = 256) {
        audio::Buffer<float> in(2, n), out(2, n);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i)
                in.channel(ch)[i] = 0.5f;
        auto out_view = out.view();
        const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> const_in(in_ptrs, 2, n);
        midi::MidiBuffer mi, mo;
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = n;
        processor->process(out_view, const_in, mi, mo, ctx);
    }
};

}  // namespace

TEST_CASE("HostBench descriptor declares sidechain + MIDI in", "[bench]") {
    HostBenchProcessor proc("Standalone");
    auto desc = proc.descriptor();
    REQUIRE(desc.name == "PulpHostBench");
    REQUIRE(desc.accepts_midi);
    REQUIRE(desc.input_buses.size() == 2);
    REQUIRE(desc.input_buses[1].name == "Sidechain");
    REQUIRE(desc.input_buses[1].optional);
}

TEST_CASE("HostBench parameters register", "[bench]") {
    BenchFixture fx;
    REQUIRE(fx.store.param_count() == 3);
    REQUIRE(fx.store.info(kBenchGain) != nullptr);
}

TEST_CASE("HostBench prepare/process logs lifecycle", "[bench]") {
    BenchFixture fx;
    auto path = fx.processor->log_path();
    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(path));

    fx.prepare();
    fx.process();

    REQUIRE(fx.processor->prepare_count() == 1);
    REQUIRE(fx.processor->process_block_count() == 1);
    REQUIRE(fx.processor->unprepared_process_count() == 0);

    // Read back the file — should contain the expected events.
    std::ifstream f(path);
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    REQUIRE(body.find("session_start") != std::string::npos);
    REQUIRE(body.find("prepare") != std::string::npos);
    REQUIRE(body.find("define_parameters") != std::string::npos);
}

TEST_CASE("HostBench logs process_without_prepare", "[bench]") {
    BenchFixture fx;
    // Intentionally skip prepare()
    fx.process();
    REQUIRE(fx.processor->unprepared_process_count() == 1);

    std::ifstream f(fx.processor->log_path());
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    REQUIRE(body.find("process_without_prepare") != std::string::npos);
}

TEST_CASE("HostBench bus layout proposal records inputs+outputs", "[bench]") {
    BenchFixture fx;
    format::Processor::BusesLayout layout{{2, 2}, {2}};
    REQUIRE(fx.processor->is_bus_layout_supported(layout));

    std::ifstream f(fx.processor->log_path());
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    REQUIRE(body.find("bus_layout_proposal") != std::string::npos);
    REQUIRE(body.find("inputs=2,2") != std::string::npos);
}

TEST_CASE("HostBench state round-trip serializes marker", "[bench]") {
    BenchFixture fx;
    auto blob = fx.processor->serialize_plugin_state();
    REQUIRE_FALSE(blob.empty());
    REQUIRE(fx.processor->deserialize_plugin_state(std::span<const uint8_t>(blob)));

    std::ifstream f(fx.processor->log_path());
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    REQUIRE(body.find("serialize_plugin_state") != std::string::npos);
    REQUIRE(body.find("marker_ok=true") != std::string::npos);
}

TEST_CASE("HostBench transport-change and tempo-change events fire", "[bench]") {
    BenchFixture fx;
    fx.processor->on_host_transport_changed(true, 1.25);
    fx.processor->on_host_tempo_changed(140.0);

    std::ifstream f(fx.processor->log_path());
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    REQUIRE(body.find("transport_changed") != std::string::npos);
    REQUIRE(body.find("is_playing=true") != std::string::npos);
    REQUIRE(body.find("tempo_changed") != std::string::npos);
    REQUIRE(body.find("tempo_bpm=140") != std::string::npos);
}
