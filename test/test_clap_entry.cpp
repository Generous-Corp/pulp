// Test: Verify the PULP_CLAP_PLUGIN() macro generates a valid CLAP entry
// This doesn't create a real .clap bundle — it just validates the generated
// symbols and factory can be called programmatically.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/clap_entry.hpp>
#include <pulp/format/host_quirks.hpp>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Minimal test processor
namespace test_clap {

class TestProcessor;
inline TestProcessor* g_last_processor = nullptr;

class TestProcessor : public pulp::format::Processor {
public:
    TestProcessor() { g_last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "TestClap",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.clap",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = true,
            .tail_samples = -1,
        };
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 1, .name = "Gain", .unit = "dB",
                             .range = {-60.0f, 24.0f, 0.0f, 0.1f}});
        store.add_parameter({.id = 2, .name = "Mode", .unit = "",
                             .range = {0.0f, 2.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = 3, .name = "Mix", .unit = "%",
                             .range = {0.0f, 100.0f, 50.0f, 0.5f}});

        pulp::state::ParamInfo quality{.id = 4, .name = "Quality", .unit = "",
                                       .range = {0.0f, 1.0f, 0.75f, 0.01f}};
        quality.to_string = [](float value) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "quality=%.2f", value);
            return std::string(buffer);
        };
        store.add_parameter(quality);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    int latency_samples() const override { return 128; }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch) {
            auto ic = in.channel(ch);
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i) oc[i] = ic[i];
        }
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
};

inline std::unique_ptr<pulp::format::Processor> create_test() {
    return std::make_unique<TestProcessor>();
}

} // namespace test_clap

namespace {

using Catch::Matchers::WithinAbs;

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
        }
    }

    ~ScopedEnv() {
        if (prev_) {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), prev_->c_str());
#else
            ::setenv(name_.c_str(), prev_->c_str(), /*overwrite=*/1);
#endif
        } else {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), "");
#else
            ::unsetenv(name_.c_str());
#endif
        }
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

private:
    std::string name_;
    std::optional<std::string> prev_;
};

// RAII guard: force a comma-decimal global C locale for the test body, then
// restore. Tries a few common comma-decimal locale names; reports whether the
// installed locale actually produced a comma decimal so the test can decide
// whether the C-locale assertions are meaningful on this box.
class ScopedCommaLocale {
public:
    ScopedCommaLocale() {
        const char* prev = std::setlocale(LC_NUMERIC, nullptr);
        if (prev) prev_ = prev;
        for (const char* name : {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8",
                                 "fr_FR", "nl_NL.UTF-8", "nl_NL"}) {
            if (std::setlocale(LC_NUMERIC, name)) {
                // Only accept a locale that genuinely uses a comma decimal
                // point; otherwise keep trying so the test's strict assertions
                // are meaningful (or comma_decimal() ends up false → skipped).
                if (std::localeconv()->decimal_point[0] == ',') {
                    comma_decimal_ = true;
                    break;
                }
            }
        }
    }
    ~ScopedCommaLocale() {
        if (!prev_.empty()) std::setlocale(LC_NUMERIC, prev_.c_str());
    }
    bool comma_decimal() const { return comma_decimal_; }

private:
    std::string prev_;
    bool comma_decimal_ = false;
};

// RAII guard for the global host-quirk policy so an assertion-throw mid-test
// can't leak the override into later test cases.
class ScopedHostQuirkPolicy {
public:
    explicit ScopedHostQuirkPolicy(pulp::format::QuirkFilter policy) {
        pulp::format::set_host_quirk_policy(policy);
    }
    ~ScopedHostQuirkPolicy() {
        pulp::format::set_host_quirk_policy(std::nullopt);
    }
};

struct MemoryStream {
    std::vector<uint8_t> bytes;
    std::size_t read_offset = 0;
};

int64_t stream_write(const clap_ostream_t* stream, const void* buffer, uint64_t size) {
    auto* sink = static_cast<MemoryStream*>(stream->ctx);
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    sink->bytes.insert(sink->bytes.end(), bytes, bytes + size);
    return static_cast<int64_t>(size);
}

int64_t stream_read(const clap_istream_t* stream, void* buffer, uint64_t size) {
    auto* source = static_cast<MemoryStream*>(stream->ctx);
    const auto remaining = source->bytes.size() - source->read_offset;
    const auto to_copy = remaining < size ? remaining : static_cast<std::size_t>(size);
    if (to_copy == 0) return 0;
    std::memcpy(buffer, source->bytes.data() + source->read_offset, to_copy);
    source->read_offset += to_copy;
    return static_cast<int64_t>(to_copy);
}

struct CappedStream {
    std::vector<uint8_t> bytes;
    uint64_t cap = 23;  // mirrors clap-validator's state-reproducibility-flush cap
};

int64_t stream_write_capped(const clap_ostream_t* stream, const void* buffer, uint64_t size) {
    auto* sink = static_cast<CappedStream*>(stream->ctx);
    const auto n = size < sink->cap ? size : sink->cap;
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    sink->bytes.insert(sink->bytes.end(), bytes, bytes + n);
    return static_cast<int64_t>(n);
}

// Minimal fake clap_input_events that walks a std::vector<const clap_event_header_t*>.
struct EventList {
    std::vector<const clap_event_header_t*> events;
};
uint32_t events_size(const clap_input_events_t* in) {
    return static_cast<uint32_t>(static_cast<const EventList*>(in->ctx)->events.size());
}
const clap_event_header_t* events_get(const clap_input_events_t* in, uint32_t i) {
    return static_cast<const EventList*>(in->ctx)->events[i];
}

} // namespace

// Generate the CLAP entry
PULP_CLAP_PLUGIN(test_clap::create_test)

TEST_CASE("PULP_CLAP_PLUGIN generates valid entry", "[clap][entry]") {
    REQUIRE(clap_entry.init != nullptr);
    REQUIRE(clap_entry.get_factory != nullptr);

    pulp::format::clap_generic::init_descriptor();

    // Initialize
    REQUIRE(clap_entry.init("test"));

    // Get factory
    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);

    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);
    REQUIRE(std::string(desc->name) == "TestClap");
    REQUIRE(std::string(desc->id) == "com.pulp.test.clap");
    REQUIRE(std::string(desc->vendor) == "PulpTest");
    REQUIRE(factory->get_plugin_descriptor(factory, 1) == nullptr);
    REQUIRE(factory->create_plugin(factory, nullptr, "com.pulp.test.missing") == nullptr);
    REQUIRE(clap_entry.get_factory("not.a.clap.factory") == nullptr);

    clap_entry.deinit();
}

TEST_CASE("CLAP entry exposes port, note, latency and tail extensions",
          "[clap][entry][ports]") {
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));

    auto* audio_ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    auto* note_ports = static_cast<const clap_plugin_note_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
    auto* latency = static_cast<const clap_plugin_latency_t*>(
        plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    auto* tail = static_cast<const clap_plugin_tail_t*>(
        plugin->get_extension(plugin, CLAP_EXT_TAIL));
    REQUIRE(audio_ports != nullptr);
    REQUIRE(note_ports != nullptr);
    REQUIRE(latency != nullptr);
    REQUIRE(tail != nullptr);
    REQUIRE(plugin->get_extension(plugin, "pulp.unsupported") == nullptr);

    REQUIRE(audio_ports->count(plugin, true) == 1);
    REQUIRE(audio_ports->count(plugin, false) == 1);

    clap_audio_port_info_t input{};
    REQUIRE(audio_ports->get(plugin, 0, true, &input));
    REQUIRE(input.id == 0);
    REQUIRE(std::string(input.name) == "Audio In");
    REQUIRE(input.channel_count == 2);
    REQUIRE(input.flags == CLAP_AUDIO_PORT_IS_MAIN);
    REQUIRE(std::string(input.port_type) == CLAP_PORT_STEREO);

    clap_audio_port_info_t output{};
    REQUIRE(audio_ports->get(plugin, 0, false, &output));
    REQUIRE(output.id == 100);
    REQUIRE(std::string(output.name) == "Audio Out");
    REQUIRE_FALSE(audio_ports->get(plugin, 1, true, &input));

    REQUIRE(note_ports->count(plugin, true) == 1);
    REQUIRE(note_ports->count(plugin, false) == 1);
    clap_note_port_info_t note_input{};
    REQUIRE(note_ports->get(plugin, 0, true, &note_input));
    REQUIRE((note_input.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0);
    REQUIRE(note_ports->get(plugin, 0, false, &note_input));

    REQUIRE(latency->get(plugin) == 128);
    REQUIRE(tail->get(plugin) == std::numeric_limits<uint32_t>::max());

    plugin->destroy(plugin);
    clap_entry.deinit();
}

TEST_CASE("CLAP entry fallback metadata handles missing processors",
          "[clap][entry][fallback]") {
    auto saved_desc = pulp::format::clap_generic::g_desc;

    pulp::format::PluginDescriptor fallback{};
    fallback.name = "FallbackClap";
    fallback.manufacturer = "PulpTest";
    fallback.bundle_id = "com.pulp.test.fallback";
    fallback.version = "1.0.0";
    fallback.category = pulp::format::PluginCategory::Effect;
    fallback.input_buses = {{"Mono In", 1}, {"Sidechain", 1}};
    fallback.output_buses = {{"Main Out", 2}, {"Aux Out", 1}};
    fallback.accepts_midi = false;
    fallback.produces_midi = false;
    fallback.tail_samples = 64;
    pulp::format::clap_generic::g_desc = fallback;

    pulp::format::clap_adapter::PulpClapPlugin data;
    clap_plugin_t plugin{};
    plugin.plugin_data = &data;

    REQUIRE(pulp::format::clap_generic::audio_ports_count(&plugin, true) == 2);
    REQUIRE(pulp::format::clap_generic::audio_ports_count(&plugin, false) == 2);

    clap_audio_port_info_t port{};
    REQUIRE(pulp::format::clap_generic::audio_ports_get(&plugin, 1, true, &port));
    REQUIRE(port.id == 1);
    REQUIRE(std::string(port.name) == "Sidechain");
    REQUIRE(port.channel_count == 1);
    REQUIRE(port.flags == 0);
    REQUIRE(std::string(port.port_type) == CLAP_PORT_MONO);

    REQUIRE(pulp::format::clap_generic::audio_ports_get(&plugin, 1, false, &port));
    REQUIRE(port.id == 101);
    REQUIRE(std::string(port.name) == "Aux Out");
    REQUIRE(port.flags == 0);
    REQUIRE(std::string(port.port_type) == CLAP_PORT_MONO);

    clap_note_port_info_t note{};
    REQUIRE(pulp::format::clap_generic::note_ports_count(&plugin, true) == 0);
    REQUIRE(pulp::format::clap_generic::note_ports_count(&plugin, false) == 0);
    REQUIRE_FALSE(pulp::format::clap_generic::note_ports_get(&plugin, 1, true, &note));
    REQUIRE_FALSE(pulp::format::clap_generic::note_ports_get(&plugin, 0, true, &note));
    REQUIRE_FALSE(pulp::format::clap_generic::note_ports_get(&plugin, 0, false, &note));
    REQUIRE(pulp::format::clap_generic::latency_get(&plugin) == 0);
    REQUIRE(pulp::format::clap_generic::tail_get(&plugin) == 0);

    MemoryStream sink;
    clap_ostream_t out_stream{.ctx = &sink, .write = stream_write};
    MemoryStream source;
    clap_istream_t in_stream{.ctx = &source, .read = stream_read};
    REQUIRE_FALSE(pulp::format::clap_generic::state_save(&plugin, &out_stream));
    REQUIRE_FALSE(pulp::format::clap_generic::state_load(&plugin, &in_stream));

    pulp::format::clap_generic::g_desc = saved_desc;
}

TEST_CASE("CLAP params extension reports metadata and text conversions",
          "[clap][entry][params]") {
    // Disable host-quirk synthesis so the count below reflects only the
    // plugin's declared params — since host-quirks P3d the CLAP adapter
    // synthesizes a Bypass param by default.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));

    auto* params = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params != nullptr);
    REQUIRE(params->count(plugin) == 4);

    clap_param_info_t info{};
    REQUIRE(params->get_info(plugin, 0, &info));
    REQUIRE(info.id == 1);
    REQUIRE(std::string(info.name) == "Gain");
    REQUIRE_THAT(info.min_value, WithinAbs(-60.0, 0.01));
    REQUIRE_THAT(info.max_value, WithinAbs(24.0, 0.01));
    REQUIRE((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);

    REQUIRE(params->get_info(plugin, 1, &info));
    REQUIRE(info.id == 2);
    REQUIRE((info.flags & CLAP_PARAM_IS_STEPPED) != 0);
    REQUIRE_FALSE(params->get_info(plugin, 9, &info));

    double value = 0.0;
    REQUIRE(params->get_value(plugin, 4, &value));
    REQUIRE_THAT(value, WithinAbs(0.75, 0.01));

    char text[64]{};
    REQUIRE(params->value_to_text(plugin, 4, value, text, sizeof(text)));
    REQUIRE(std::string(text) == "quality=0.75");
    REQUIRE(params->text_to_value(plugin, 1, "-3.25 dB", &value));
    REQUIRE_THAT(value, WithinAbs(-3.25, 0.01));
    REQUIRE_FALSE(params->text_to_value(plugin, 1, "dB", &value));

    plugin->destroy(plugin);
    clap_entry.deinit();
    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("CLAP param text conversion is locale-independent (comma-decimal host)",
          "[clap][entry][params][locale]") {
    // Under a comma-decimal global C locale, the previous snprintf("%.2f")
    // emitted "-3,25" and strtod() parsed "0.5" as 0.0 (stopping at the dot),
    // corrupting display and typed-in values. std::to_chars / std::from_chars
    // always use the C decimal point, so the round-trip is locale-immune.
    ScopedCommaLocale guard;
    if (!guard.comma_decimal()) {
        SKIP("no comma-decimal locale (de_DE/fr_FR/nl_NL) installed on this box "
             "— the C-locale regression cannot be proven here");
    }

    // RAII so an assertion-throw can't leak the quirk override into later tests.
    ScopedHostQuirkPolicy quirk_guard(pulp::format::kQuirkFilterOff);
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));

    auto* params = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params != nullptr);

    // value_to_text on the "Mix" param (unit "%") must produce a dot, not a
    // comma, regardless of the global locale.
    char text[64]{};
    REQUIRE(params->value_to_text(plugin, 3, 0.5, text, sizeof(text)));
    REQUIRE(std::string(text) == "0.50 %");
    REQUIRE(std::string(text).find(',') == std::string::npos);

    // value_to_text on the "Mode" param (no unit).
    REQUIRE(params->value_to_text(plugin, 2, 0.5, text, sizeof(text)));
    REQUIRE(std::string(text) == "0.50");

    // text_to_value must parse a dotted "0.5" fully (not stop at the dot).
    double value = 99.0;
    REQUIRE(params->text_to_value(plugin, 3, "0.5", &value));
    REQUIRE_THAT(value, WithinAbs(0.5, 1e-9));

    // Round-trip: format then parse back.
    REQUIRE(params->value_to_text(plugin, 3, 0.5, text, sizeof(text)));
    value = 0.0;
    REQUIRE(params->text_to_value(plugin, 3, text, &value));
    REQUIRE_THAT(value, WithinAbs(0.5, 1e-9));

    // Leading-space tolerance (strtod parity).
    value = 0.0;
    REQUIRE(params->text_to_value(plugin, 3, "   0.5 %", &value));
    REQUIRE_THAT(value, WithinAbs(0.5, 1e-9));

    // Leading '+' accepted (strtod parity; from_chars alone rejects it).
    value = 0.0;
    REQUIRE(params->text_to_value(plugin, 3, "+0.5", &value));
    REQUIRE_THAT(value, WithinAbs(0.5, 1e-9));

    // Leading tab/other whitespace skipped (strtod parity; not just ' ').
    value = 0.0;
    REQUIRE(params->text_to_value(plugin, 3, "\t0.5", &value));
    REQUIRE_THAT(value, WithinAbs(0.5, 1e-9));

    // Trailing text after the number is still accepted (matches strtod).
    value = 0.0;
    REQUIRE(params->text_to_value(plugin, 3, "0.5 dB", &value));
    REQUIRE_THAT(value, WithinAbs(0.5, 1e-9));

    // Non-numeric text still fails, as before.
    REQUIRE_FALSE(params->text_to_value(plugin, 3, "%", &value));

    // Empty / whitespace-only input is rejected.
    REQUIRE_FALSE(params->text_to_value(plugin, 3, "", &value));
    REQUIRE_FALSE(params->text_to_value(plugin, 3, "   ", &value));

    // Out-of-range input (advances ptr but sets ec=out_of_range) must be
    // rejected WITHOUT writing a bogus 0.0 — the value sentinel survives.
    value = 7.0;
    REQUIRE_FALSE(params->text_to_value(plugin, 3, "1e999999", &value));
    REQUIRE_THAT(value, WithinAbs(7.0, 1e-9));

    plugin->destroy(plugin);
    clap_entry.deinit();
}

TEST_CASE("CLAP params extension formats fallbacks and flushes gestures",
          "[clap][entry][params][flush]") {
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));
    auto* proc = test_clap::g_last_processor;
    REQUIRE(proc != nullptr);

    auto* params = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params != nullptr);

    char text[64]{};
    REQUIRE(params->value_to_text(plugin, 1, -6.5, text, sizeof(text)));
    REQUIRE(std::string(text) == "-6.50 dB");
    REQUIRE(params->value_to_text(plugin, 2, 1.0, text, sizeof(text)));
    REQUIRE(std::string(text) == "1.00");
    REQUIRE_FALSE(params->value_to_text(plugin, 404, 0.0, text, sizeof(text)));

    params->flush(plugin, nullptr, nullptr);

    clap_event_param_gesture_t begin{};
    begin.header.size = sizeof(begin);
    begin.header.time = 0;
    begin.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    begin.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
    begin.param_id = 1;

    clap_event_param_gesture_t end = begin;
    end.header.type = CLAP_EVENT_PARAM_GESTURE_END;

    EventList gestures{.events = {&begin.header, &end.header}};
    clap_input_events_t in{.ctx = &gestures, .size = events_size, .get = events_get};
    params->flush(plugin, &in, nullptr);

    proc->state().set_value(1, -1.0f);
    double value = 0.0;
    REQUIRE(params->get_value(plugin, 1, &value));
    REQUIRE_THAT(value, WithinAbs(-1.0, 0.01));

    plugin->destroy(plugin);
    clap_entry.deinit();
}

TEST_CASE("CLAP GUI extension is hidden under automation env",
          "[clap][entry][gui][issue-2515]") {
    ScopedEnv disable_editor("PULP_DISABLE_PLUGIN_EDITOR");
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    ScopedEnv display("DISPLAY");
    ScopedEnv wayland("WAYLAND_DISPLAY");
    display.set(":99");
    wayland.unset();
#endif
    disable_editor.unset();
    headless.unset();
    test_mode.unset();
    ci.unset();

    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));

    auto* gui = static_cast<const clap_plugin_gui_t*>(
        plugin->get_extension(plugin, CLAP_EXT_GUI));
    REQUIRE(gui != nullptr);

    const char* preferred_api = nullptr;
    bool is_floating = true;
    REQUIRE(gui->get_preferred_api(plugin, &preferred_api, &is_floating));
    REQUIRE_FALSE(is_floating);
#if defined(__APPLE__)
    REQUIRE(std::string(preferred_api) == CLAP_WINDOW_API_COCOA);
    REQUIRE(gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false));
    REQUIRE_FALSE(gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, true));
#elif defined(_WIN32)
    REQUIRE(std::string(preferred_api) == CLAP_WINDOW_API_WIN32);
    REQUIRE(gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, false));
    REQUIRE_FALSE(gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, true));
#elif defined(__linux__)
    REQUIRE(std::string(preferred_api) == CLAP_WINDOW_API_X11);
    REQUIRE(gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false));
    REQUIRE_FALSE(gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, true));
#endif
    REQUIRE_FALSE(gui->is_api_supported(plugin, "pulp.unsupported-window-api", false));

    uint32_t width = 0;
    uint32_t height = 0;
    REQUIRE(gui->get_size(plugin, &width, &height));
    REQUIRE(width == 400);
    REQUIRE(height == 300);
    REQUIRE_FALSE(gui->set_scale(plugin, 2.0));
    REQUIRE_FALSE(gui->can_resize(plugin));
    clap_gui_resize_hints_t hints{};
    REQUIRE_FALSE(gui->get_resize_hints(plugin, &hints));
    REQUIRE_FALSE(gui->adjust_size(plugin, &width, &height));
    REQUIRE_FALSE(gui->set_size(plugin, 640, 480));
    clap_window_t window{};
    REQUIRE_FALSE(gui->set_parent(plugin, &window));
    REQUIRE_FALSE(gui->set_transient(plugin, &window));
    gui->suggest_title(plugin, "Pulp Test");
    REQUIRE_FALSE(gui->show(plugin));
    REQUIRE(gui->hide(plugin));
    gui->destroy(plugin);

    disable_editor.set("1");
    REQUIRE(plugin->get_extension(plugin, CLAP_EXT_GUI) == nullptr);
    REQUIRE_FALSE(gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false));
    REQUIRE_FALSE(gui->create(plugin, CLAP_WINDOW_API_COCOA, false));

    plugin->destroy(plugin);
    clap_entry.deinit();
}

TEST_CASE("CLAP state extension round-trips plugin-owned payload", "[clap][entry][state]") {
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin1 = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin1 != nullptr);
    REQUIRE(plugin1->init(plugin1));
    auto* proc1 = test_clap::g_last_processor;
    REQUIRE(proc1 != nullptr);
    proc1->state().set_value(1, -12.5f);
    proc1->plugin_state = "snapshots=A|B";

    auto* state1 = static_cast<const clap_plugin_state_t*>(
        plugin1->get_extension(plugin1, CLAP_EXT_STATE));
    REQUIRE(state1 != nullptr);

    MemoryStream sink;
    clap_ostream_t out_stream{.ctx = &sink, .write = stream_write};
    REQUIRE(state1->save(plugin1, &out_stream));

    const clap_plugin_t* plugin2 = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin2 != nullptr);
    REQUIRE(plugin2->init(plugin2));
    auto* proc2 = test_clap::g_last_processor;
    REQUIRE(proc2 != nullptr);
    proc2->state().set_value(1, 6.0f);
    proc2->plugin_state = "stale";

    auto* state2 = static_cast<const clap_plugin_state_t*>(
        plugin2->get_extension(plugin2, CLAP_EXT_STATE));
    REQUIRE(state2 != nullptr);

    MemoryStream source{.bytes = sink.bytes};
    clap_istream_t in_stream{.ctx = &source, .read = stream_read};
    REQUIRE(state2->load(plugin2, &in_stream));
    REQUIRE_THAT(proc2->state().get_value(1), WithinAbs(-12.5, 0.01));
    REQUIRE(proc2->plugin_state == "snapshots=A|B");

    plugin1->destroy(plugin1);
    plugin2->destroy(plugin2);
    clap_entry.deinit();
}

TEST_CASE("CLAP state_save loops on short writes [issue-743]",
          "[clap][entry][state][short-write]") {
    // clap-validator's `state-reproducibility-flush` caps stream writes at
    // 23 bytes/call. Before the fix, state_save() treated a short write as
    // failure and returned false — which the validator saw as a plugin bug.
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));

    auto* proc = test_clap::g_last_processor;
    proc->plugin_state = std::string(300, 'x');  // > any reasonable single-write cap

    auto* state = static_cast<const clap_plugin_state_t*>(
        plugin->get_extension(plugin, CLAP_EXT_STATE));
    REQUIRE(state != nullptr);

    CappedStream sink;
    clap_ostream_t out_stream{.ctx = &sink, .write = stream_write_capped};
    REQUIRE(state->save(plugin, &out_stream));
    REQUIRE(sink.bytes.size() >= 300);  // the full payload, loop-written

    plugin->destroy(plugin);
    clap_entry.deinit();
}

TEST_CASE("CLAP params_flush ignores events outside the core namespace [issue-743]",
          "[clap][entry][params][namespace]") {
    // clap-validator's `param-set-wrong-namespace` sends PARAM_VALUE events
    // with space_id = 0xb33f (not CLAP_CORE_EVENT_SPACE_ID). A plugin that
    // doesn't filter by space_id will apply those as real param writes.
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));
    auto* proc = test_clap::g_last_processor;
    proc->state().set_value(1, 0.0f);  // baseline

    auto* params = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params != nullptr);

    // Construct one PARAM_VALUE event with a non-core namespace. Should be
    // dropped by the flush path.
    clap_event_param_value_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.space_id = 0xb33f;  // non-core — must be ignored
    ev.header.flags = 0;
    ev.header.time = 0;
    ev.param_id = 1;
    ev.value = 42.0;

    EventList list{.events = {&ev.header}};
    clap_input_events_t in{.ctx = &list, .size = events_size, .get = events_get};
    params->flush(plugin, &in, nullptr);

    REQUIRE_THAT(proc->state().get_value(1), WithinAbs(0.0, 0.01));  // untouched

    // Sanity: the same event with space_id=CLAP_CORE_EVENT_SPACE_ID should
    // apply — confirms our guard isn't blocking well-formed core events.
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.value = -6.0;
    params->flush(plugin, &in, nullptr);
    REQUIRE_THAT(proc->state().get_value(1), WithinAbs(-6.0, 0.01));

    plugin->destroy(plugin);
    clap_entry.deinit();
}
