// A Processor may reach its StateStore for its whole lifetime — including from
// its destructor, and from any worker thread that destructor is about to join.
// The store belongs to the host, so the host must outlive the Processor with it.
//
// Every host in `core/format` holds the two as members. C++ destroys members in
// reverse declaration order, so the store has to be declared *first*. All of them
// had it backwards, which is invisible for a Processor that owns no threads and a
// use-after-free for one that does: a background thread ticking against
// `state().get_value()` while the destructor walks to its `join()` reads a store
// whose vectors were freed a microsecond earlier. It crashes on plug-in close and
// nowhere else, once in some tens of loads.
//
// These tests observe the store's destruction directly rather than reading freed
// memory and hoping the result looks wrong. A parameter's `to_string` closure owns
// a sentinel; when the store dies, the closure dies with it and the sentinel says
// so. A Processor destructor that finds the sentinel already fired has outlived its
// own store.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <memory>
#include <string>

using namespace pulp;
using namespace pulp::format;

namespace {

/// Fires when the StateStore that owns it is destroyed. Never construct one as a
/// temporary and copy it — the temporary's own destructor would fire immediately.
struct Sentinel {
    bool* fired;
    explicit Sentinel(bool* f) : fired(f) {}
    Sentinel(const Sentinel&) = delete;
    Sentinel& operator=(const Sentinel&) = delete;
    ~Sentinel() { *fired = true; }
};

/// Records, at its own destruction, whether its store had already been destroyed.
class StoreWatcher : public Processor {
public:
    StoreWatcher(bool* store_destroyed, bool* store_was_dead_at_teardown)
        : store_destroyed_(store_destroyed), saw_dead_(store_was_dead_at_teardown) {}

    ~StoreWatcher() override { *saw_dead_ = *store_destroyed_; }

    PluginDescriptor descriptor() const override {
        return {
            .name = "StoreWatcher",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.store-lifetime",
            .version = "1.0.0",
            .category = PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        // The sentinel rides in the parameter's own formatter, so it is destroyed
        // exactly when the store's parameter list is.
        auto sentinel = std::make_shared<Sentinel>(store_destroyed_);
        state::ParamInfo p{.id = 1, .name = "Gain", .range = {0.0f, 1.0f, 0.0f, 0.0f}};
        p.to_string = [sentinel](float) { return std::string("gain"); };
        store.add_parameter(p);
    }

    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext&) override {}

private:
    bool* store_destroyed_;
    bool* saw_dead_;
};

bool g_store_destroyed = false;
bool g_saw_dead_store = false;

std::unique_ptr<Processor> make_watcher() {
    return std::make_unique<StoreWatcher>(&g_store_destroyed, &g_saw_dead_store);
}

/// Reset the observation between tests: both flags are process-wide, because a
/// ProcessorFactory takes no arguments.
void arm() {
    g_store_destroyed = false;
    g_saw_dead_store = false;
}

}  // namespace

TEST_CASE("HeadlessHost's StateStore outlives its Processor",
          "[format][store-lifetime]") {
    arm();
    {
        HeadlessHost host(make_watcher);
        host.prepare(48000.0, 512, 2, 2);
        REQUIRE_FALSE(g_store_destroyed);
    }
    // The sentinel fired, so the store really was destroyed — the test is watching
    // something, not passing vacuously on a flag nobody set.
    CHECK(g_store_destroyed);
    CHECK_FALSE(g_saw_dead_store);
}

TEST_CASE("The CLAP adapter's StateStore outlives its Processor",
          "[format][clap][store-lifetime]") {
    arm();
    {
        clap_adapter::PulpClapPlugin plugin;
        plugin.factory = make_watcher;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE_FALSE(g_store_destroyed);
    }
    CHECK(g_store_destroyed);
    CHECK_FALSE(g_saw_dead_store);
}

TEST_CASE("A Processor can read its parameters from its own destructor",
          "[format][store-lifetime]") {
    // The invariant stated positively: whatever a Processor could read in
    // `process()`, it can still read while being torn down. A worker thread it has
    // not joined yet is in exactly that position.
    struct LateReader : Processor {
        float* out;
        explicit LateReader(float* o) : out(o) {}
        ~LateReader() override { *out = state().get_value(7); }
        PluginDescriptor descriptor() const override {
            return {.name = "LateReader",
                    .manufacturer = "PulpTest",
                    .bundle_id = "com.pulp.test.late-reader",
                    .version = "1.0.0",
                    .category = PluginCategory::Effect,
                    .output_buses = {{"Out", 2}}};
        }
        void define_parameters(state::StateStore& store) override {
            store.add_parameter({.id = 7, .name = "Level",
                                 .range = {0.0f, 1.0f, 0.75f, 0.0f}});
        }
        void prepare(const PrepareContext&) override {}
        void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                     midi::MidiBuffer&, midi::MidiBuffer&,
                     const ProcessContext&) override {}
    };

    static float read_back = -1.0f;
    read_back = -1.0f;
    {
        HeadlessHost host([] { return std::unique_ptr<Processor>(new LateReader(&read_back)); });
        host.state().set_value(7, 0.25f);
    }
    CHECK(read_back == 0.25f);
}
