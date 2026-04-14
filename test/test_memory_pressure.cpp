// Verifies Processor::on_memory_pressure() hook (workstream 05 slice 5.3).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp::format;

namespace {

class PlainProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

class CacheDroppingProcessor : public PlainProcessor {
public:
    int advisory_calls = 0;
    int critical_calls = 0;
    std::size_t cache_size = 1024;
    void on_memory_pressure(MemoryPressure level) override {
        if (level == MemoryPressure::Advisory) ++advisory_calls;
        if (level == MemoryPressure::Critical) ++critical_calls;
        if (level == MemoryPressure::Critical) cache_size = 0;
    }
};

} // namespace

TEST_CASE("default on_memory_pressure is a no-op", "[processor][memory]") {
    PlainProcessor p;
    p.on_memory_pressure(Processor::MemoryPressure::Advisory);
    p.on_memory_pressure(Processor::MemoryPressure::Critical);
    SUCCEED("default override returned cleanly");
}

TEST_CASE("plugin receives pressure levels and can drop caches",
          "[processor][memory]") {
    CacheDroppingProcessor p;
    REQUIRE(p.cache_size == 1024);
    p.on_memory_pressure(Processor::MemoryPressure::Advisory);
    REQUIRE(p.advisory_calls == 1);
    REQUIRE(p.cache_size == 1024);  // advisory: keep working set
    p.on_memory_pressure(Processor::MemoryPressure::Critical);
    REQUIRE(p.critical_calls == 1);
    REQUIRE(p.cache_size == 0);     // critical: drop
}
