// MIDI port identity: the id enumerate_*() reports is the key open() takes,
// it is the same across MidiSystem instances and across processes, and an
// unknown id fails instead of opening some other port. resolve_port() rebinds
// a saved port when only the id changed.

#include <catch2/catch_test_macros.hpp>

#include <ports_json.hpp>
#include <pulp/midi/device.hpp>

#include <string>
#include <vector>

using pulp::midi::MidiPortInfo;
using pulp::midi::resolve_port;

namespace {

MidiPortInfo port(std::string id, std::string key, bool online = true) {
    MidiPortInfo p;
    p.id = std::move(id);
    p.stable_key = std::move(key);
    p.online = online;
    p.is_input = true;
    return p;
}

} // namespace

TEST_CASE("resolve_port prefers the saved id", "[midi][port-identity]") {
    const std::vector<MidiPortInfo> ports = {port("11", "Dev|1|A"), port("22", "Dev|1|B")};
    CHECK(resolve_port(ports, "22", "Dev|1|A") == 1);
}

TEST_CASE("resolve_port falls back to a unique stable key when the id changed",
          "[midi][port-identity]") {
    const std::vector<MidiPortInfo> ports = {port("11", "Dev|1|A"), port("22", "Dev|1|B")};
    CHECK(resolve_port(ports, "999", "Dev|1|B") == 1);
    CHECK(resolve_port(ports, "", "Dev|1|A") == 0);
}

TEST_CASE("resolve_port refuses an ambiguous or missing match", "[midi][port-identity]") {
    const std::vector<MidiPortInfo> twins = {port("11", "Dev|1|A"), port("22", "Dev|1|A")};
    CHECK(resolve_port(twins, "999", "Dev|1|A") == -1);
    CHECK(resolve_port(twins, "999", "") == -1);
    CHECK(resolve_port(twins, "", "") == -1);
    CHECK(resolve_port({}, "11", "Dev|1|A") == -1);
}

TEST_CASE("resolve_port skips offline ports", "[midi][port-identity]") {
    const std::vector<MidiPortInfo> ports = {port("11", "Dev|1|A", false), port("22", "Dev|1|A")};
    CHECK(resolve_port(ports, "11", "Dev|1|A") == 1);
    const std::vector<MidiPortInfo> only_offline = {port("11", "Dev|1|A", false)};
    CHECK(resolve_port(only_offline, "11", "Dev|1|A") == -1);
}

TEST_CASE("ports_json lists every identity field and escapes strings", "[midi][port-identity]") {
    MidiPortInfo p;
    p.id = "42";
    p.name = "Dev \"A\"";
    p.device_name = "Dev";
    p.entity_name = "E\\1";
    p.endpoint_name = "A";
    p.stable_key = "Dev|E\\1|A";
    p.online = false;
    const std::string json = pulp::midi::tools::ports_json({p}, {});
    CHECK(json.find("\"id\": \"42\"") != std::string::npos);
    CHECK(json.find("\"name\": \"Dev \\\"A\\\"\"") != std::string::npos);
    CHECK(json.find("\"entity\": \"E\\\\1\"") != std::string::npos);
    CHECK(json.find("\"online\": false") != std::string::npos);
    CHECK(json.find("\"outputs\": []") != std::string::npos);
}

#ifdef __APPLE__

#include <CoreMIDI/CoreMIDI.h>
#include <mach-o/dyld.h>

#include <cstdio>
#include <set>
#include <sstream>

namespace {

constexpr SInt32 kSourceId = 0x50504931; // "PPI1"
constexpr SInt32 kDestId = 0x50504932;   // "PPI2"

struct VirtualPair {
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    MIDIEndpointRef dest = 0;

    bool create() {
        if (MIDIClientCreate(CFSTR("PulpPortIdentityTest"), nullptr, nullptr, &client) != noErr)
            return false;
        if (MIDISourceCreate(client, CFSTR("Pulp Identity Source"), &source) != noErr)
            return false;

        if (MIDIDestinationCreateWithProtocol(client, CFSTR("Pulp Identity Dest"),
                                              kMIDIProtocol_1_0, &dest,
                                              ^(const MIDIEventList*, void*){
                                              }) != noErr)
            return false;
        MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, kSourceId);
        MIDIObjectSetIntegerProperty(dest, kMIDIPropertyUniqueID, kDestId);
        return true;
    }

    ~VirtualPair() {
        if (source)
            MIDIEndpointDispose(source);
        if (dest)
            MIDIEndpointDispose(dest);
        if (client)
            MIDIClientDispose(client);
    }
};

const MidiPortInfo* find(const std::vector<MidiPortInfo>& ports, const std::string& id) {
    for (const auto& p : ports)
        if (p.id == id)
            return &p;
    return nullptr;
}

std::string executable_path() {
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    return buf;
}

} // namespace

TEST_CASE("CoreMIDI reports the same ids from two systems and fills identity fields",
          "[midi][coremidi][port-identity]") {
    VirtualPair vp;
    if (!vp.create())
        SKIP("CoreMIDI server not reachable; skipping");

    auto a = pulp::midi::create_midi_system();
    auto b = pulp::midi::create_midi_system();
    const auto in_a = a->enumerate_inputs();
    const auto in_b = b->enumerate_inputs();
    REQUIRE(in_a.size() == in_b.size());
    for (size_t i = 0; i < in_a.size(); ++i) {
        CHECK(in_a[i].id == in_b[i].id);
        CHECK(in_a[i].stable_key == in_b[i].stable_key);
    }

    const MidiPortInfo* src = find(in_a, std::to_string(kSourceId));
    REQUIRE(src != nullptr);
    CHECK(src->is_input);
    CHECK_FALSE(src->is_output);
    CHECK(src->endpoint_name == "Pulp Identity Source");
    CHECK(src->device_name.empty()); // virtual endpoint: no owning device
    CHECK(src->stable_key.empty());
    CHECK(src->online);

    const auto out = a->enumerate_outputs();
    const MidiPortInfo* dst = find(out, std::to_string(kDestId));
    REQUIRE(dst != nullptr);
    CHECK(dst->is_output);
    CHECK(dst->endpoint_name == "Pulp Identity Dest");
}

TEST_CASE("CoreMIDI open takes the enumerated id and fails on an unknown one",
          "[midi][coremidi][port-identity]") {
    VirtualPair vp;
    if (!vp.create())
        SKIP("CoreMIDI server not reachable; skipping");

    auto system = pulp::midi::create_midi_system();
    auto in = system->create_input();
    REQUIRE(in->open(std::to_string(kSourceId), [](const pulp::midi::MidiEvent&) {}));
    in->close();

    for (const char* bad : {"2147483000", "-5", "not-a-number", "12abc"}) {
        INFO("input id " << bad);
        CHECK_FALSE(in->open(bad, [](const pulp::midi::MidiEvent&) {}));
        CHECK_FALSE(in->is_open());
    }

    auto out = system->create_output();
    REQUIRE(out->open(std::to_string(kDestId)));
    out->close();
    for (const char* bad : {"2147483000", "-5", "not-a-number", "12abc"}) {
        INFO("output id " << bad);
        CHECK_FALSE(out->open(bad));
        CHECK_FALSE(out->is_open());
    }
}

TEST_CASE("CoreMIDI does not list an offline endpoint as online",
          "[midi][coremidi][port-identity]") {
    VirtualPair vp;
    if (!vp.create())
        SKIP("CoreMIDI server not reachable; skipping");

    // CoreMIDI drops an offline endpoint from its source list; if a future
    // system keeps it, it must read as not online.
    MIDIObjectSetIntegerProperty(vp.source, kMIDIPropertyOffline, 1);
    auto system = pulp::midi::create_midi_system();
    const auto inputs = system->enumerate_inputs();
    const MidiPortInfo* src = find(inputs, std::to_string(kSourceId));
    if (src != nullptr)
        CHECK_FALSE(src->online);
}

// Child half of the cross-process check. Hidden so a normal run skips it.
TEST_CASE("port identity child lists ports", "[.port-identity-child]") {
    auto system = pulp::midi::create_midi_system();
    for (const auto& p : system->enumerate_inputs())
        std::printf("PORT\tin\t%s\t%s\n", p.id.c_str(), p.stable_key.c_str());
    for (const auto& p : system->enumerate_outputs())
        std::printf("PORT\tout\t%s\t%s\n", p.id.c_str(), p.stable_key.c_str());
}

TEST_CASE("CoreMIDI ids reported by a separate process match this one",
          "[midi][coremidi][port-identity]") {
    VirtualPair vp;
    if (!vp.create())
        SKIP("CoreMIDI server not reachable; skipping");

    const std::string exe = executable_path();
    REQUIRE_FALSE(exe.empty());
    const std::string cmd = "\"" + exe + "\" \"[.port-identity-child]\" 2>/dev/null";

    std::set<std::string> child;
    FILE* pipe = popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char line[1024];
    while (fgets(line, sizeof(line), pipe)) {
        std::string s(line);
        if (s.rfind("PORT\t", 0) != 0)
            continue;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        child.insert(s);
    }
    pclose(pipe);

    auto system = pulp::midi::create_midi_system();
    std::set<std::string> mine;
    for (const auto& p : system->enumerate_inputs())
        mine.insert("PORT\tin\t" + p.id + "\t" + p.stable_key);
    for (const auto& p : system->enumerate_outputs())
        mine.insert("PORT\tout\t" + p.id + "\t" + p.stable_key);

    REQUIRE_FALSE(child.empty());
    CHECK(child == mine);
    CHECK(child.count("PORT\tin\t" + std::to_string(kSourceId) + "\t") == 1);
}

#endif // __APPLE__
