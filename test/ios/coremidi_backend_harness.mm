#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>

#include <pulp/midi/device.hpp>
#include <pulp/midi/ump_session.hpp>

#include "../../core/midi/platform/mac/coremidi_shared_client.h"
#include "../../core/midi/src/ump_session_backend.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kResultFile = "coremidi-result.txt";

std::string utf8(NSString* value) {
    return value ? std::string{value.UTF8String} : std::string{};
}

void write_result(NSString* text) {
    NSArray<NSString*>* documents = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* directory = documents.firstObject;
    NSString* path = [directory stringByAppendingPathComponent:
        [NSString stringWithUTF8String:kResultFile]];
    NSError* error = nil;
    if (![text writeToFile:path
                atomically:YES
                  encoding:NSUTF8StringEncoding
                     error:&error]) {
        NSLog(@"PULP_COREMIDI_HARNESS: result write failed: %@", error);
    }
}

template <typename Predicate>
bool wait_until(Predicate&& predicate) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (predicate()) return true;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

const pulp::midi::MidiPortInfo* find_port(
    const std::vector<pulp::midi::MidiPortInfo>& ports,
    const std::string& id,
    const std::string& name) {
    for (const auto& port : ports) {
        if (port.id == id && port.name == name) return &port;
    }
    return nullptr;
}

const pulp::midi::UmpEndpointInfo* find_ump_endpoint(
    const std::vector<pulp::midi::UmpEndpointInfo>& endpoints,
    const std::string& name,
    bool can_receive,
    bool can_send) {
    for (const auto& endpoint : endpoints) {
        if (endpoint.name == name &&
            endpoint.direction.can_receive == can_receive &&
            endpoint.direction.can_send == can_send) {
            return &endpoint;
        }
    }
    return nullptr;
}

MIDIEventList one_word_event_list(uint32_t word) {
    MIDIEventList list;
    MIDIEventPacket* packet = MIDIEventListInit(&list, kMIDIProtocol_2_0);
    MIDIEventListAdd(&list, sizeof(list), packet, mach_absolute_time(), 1,
                     &word);
    return list;
}

MIDIEventList two_word_event_list(uint32_t first, uint32_t second) {
    MIDIEventList list;
    MIDIEventPacket* packet = MIDIEventListInit(&list, kMIDIProtocol_2_0);
    const uint32_t words[] = {first, second};
    MIDIEventListAdd(&list, sizeof(list), packet, mach_absolute_time(), 2,
                     words);
    return list;
}

bool topology_contract_passes() {
    using Endpoint = pulp::midi::CoreMidiUmpTopologyEndpoint;
    const std::vector<Endpoint> census = {
        {"101", "multi", "Multi Source A", true},
        {"102", "multi", "Multi Source B", true},
        {"201", "multi", "Multi Destination A", false},
        {"202", "multi", "Multi Destination B", false},
        {"301", "pair", "Paired Device", true},
        {"302", "pair", "Paired Device", false},
    };
    const auto grouped = pulp::midi::group_coremidi_ump_topology(census);
    if (grouped.size() != 5) return false;
    bool paired = false;
    std::size_t multi_count = 0;
    for (const auto& endpoint : grouped) {
        if (endpoint.id == "entity:pair") {
            paired = endpoint.direction.can_receive &&
                     endpoint.direction.can_send;
        } else if (endpoint.id == "101" || endpoint.id == "102") {
            multi_count += endpoint.direction.can_receive &&
                           !endpoint.direction.can_send;
        } else if (endpoint.id == "201" || endpoint.id == "202") {
            multi_count += !endpoint.direction.can_receive &&
                           endpoint.direction.can_send;
        }
    }
    return paired && multi_count == 4;
}

int fail(NSString* reason, MIDIEndpointRef source, MIDIEndpointRef destination) {
    if (source) MIDIEndpointDispose(source);
    if (destination) MIDIEndpointDispose(destination);
    write_result([@"FAIL: " stringByAppendingString:reason]);
    NSLog(@"PULP_COREMIDI_HARNESS: FAIL: %@", reason);
    return 1;
}

int run_harness() {
    if (!topology_contract_passes()) {
        return fail(@"production UMP topology census/pairing failed", 0, 0);
    }
    const MIDIClientRef client = pulp::midi::mac::shared_client();
    if (!client) return fail(@"production shared client unavailable", 0, 0);
    if (pulp::midi::mac::shared_client() != client) {
        return fail(@"production shared client identity changed", 0, 0);
    }

    NSString* nonce = NSUUID.UUID.UUIDString;
    NSString* source_ns = [@"Pulp iOS Source " stringByAppendingString:nonce];
    NSString* destination_ns =
        [@"Pulp iOS Destination " stringByAppendingString:nonce];
    const std::string source_name = utf8(source_ns);
    const std::string destination_name = utf8(destination_ns);

    MIDIEndpointRef source = 0;
    MIDIEndpointRef destination = 0;
    OSStatus status = MIDISourceCreateWithProtocol(
        client, (__bridge CFStringRef)source_ns, kMIDIProtocol_2_0, &source);
    if (status != noErr || !source) {
        return fail([NSString stringWithFormat:@"source create status=%d",
                                                static_cast<int>(status)],
                    source, destination);
    }
    auto destination_word = std::make_shared<std::atomic<uint32_t>>(0);
    status = MIDIDestinationCreateWithProtocol(
        client, (__bridge CFStringRef)destination_ns, kMIDIProtocol_2_0,
        &destination, ^(const MIDIEventList* event_list, void*) {
            if (event_list && event_list->numPackets > 0 &&
                event_list->packet[0].wordCount > 0) {
                destination_word->store(event_list->packet[0].words[0],
                                        std::memory_order_release);
            }
        });
    if (status != noErr || !destination) {
        return fail([NSString stringWithFormat:@"destination create status=%d",
                                                static_cast<int>(status)],
                    source, destination);
    }

    SInt32 source_uid = 0;
    SInt32 destination_uid = 0;
    if (MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID,
                                     &source_uid) != noErr ||
        MIDIObjectGetIntegerProperty(destination, kMIDIPropertyUniqueID,
                                     &destination_uid) != noErr ||
        source_uid == 0 || destination_uid == 0 ||
        source_uid == destination_uid) {
        return fail(@"virtual endpoint unique ids are invalid", source,
                    destination);
    }

    const std::string source_id = std::to_string(source_uid);
    const std::string destination_id = std::to_string(destination_uid);
    auto system = pulp::midi::create_midi_system();
    if (!system) return fail(@"production MidiSystem unavailable", source,
                             destination);

    bool discovered = wait_until([&] {
        const auto inputs = system->enumerate_inputs();
        const auto outputs = system->enumerate_outputs();
        const auto* input = find_port(inputs, source_id, source_name);
        const auto* output =
            find_port(outputs, destination_id, destination_name);
        return input && input->is_input && output && output->is_output;
    });
    if (!discovered) {
        return fail(@"production enumeration missed virtual endpoints", source,
                    destination);
    }

    {
        pulp::midi::UmpSession session({"Pulp iOS UMP Harness", true});
        if (!session.os_backend_active()) {
            return fail(@"production UMP backend is inactive", source,
                        destination);
        }

        std::string ump_source_id;
        std::string ump_destination_id;
        const bool ump_discovered = wait_until([&] {
            const auto endpoints = session.enumerate_endpoints();
            const auto* input = find_ump_endpoint(endpoints, source_name,
                                                  true, false);
            const auto* output = find_ump_endpoint(endpoints, destination_name,
                                                   false, true);
            if (!input || !output || input->id == output->id) return false;
            ump_source_id = input->id;
            ump_destination_id = output->id;
            return true;
        });
        if (!ump_discovered) {
            return fail(@"production UMP enumeration missed virtual endpoints",
                        source, destination);
        }

        pulp::midi::UmpOpenStatus open_status =
            pulp::midi::UmpOpenStatus::NotFound;
        auto* input = session.open_endpoint(ump_source_id, &open_status);
        if (!input || open_status != pulp::midi::UmpOpenStatus::Ok ||
            !input->is_open()) {
            return fail(@"production UMP source open failed", source,
                        destination);
        }
        auto received_word = std::make_shared<std::atomic<uint32_t>>(0);
        input->set_receive_callback(
            [received_word](const pulp::midi::UmpPacket& packet, double) {
                if (packet.word_count > 0) {
                    received_word->store(packet.words[0],
                                         std::memory_order_release);
                }
            });

        auto* output = session.open_endpoint(ump_destination_id, &open_status);
        if (!output || open_status != pulp::midi::UmpOpenStatus::Ok ||
            !output->is_open()) {
            return fail(@"production UMP destination open failed", source,
                        destination);
        }

        // MT 0x6 is a complete one-word message. Following it with a valid
        // MT 0x2 message detects any private/incorrect size table that skips
        // the second packet instead of using the canonical UMP walker.
        constexpr uint32_t kSourceWord = 0x20903c7f;
        auto source_events = two_word_event_list(0x60000000, kSourceWord);
        status = MIDIReceivedEventList(source, &source_events);
        if (status != noErr || !wait_until([&] {
                return received_word->load(std::memory_order_acquire) ==
                       kSourceWord;
            })) {
            return fail(@"production UMP input event-list delivery failed",
                        source, destination);
        }

        constexpr uint32_t kDestinationWord = 0x20803c00;
        pulp::midi::UmpPacket packet;
        packet.word_count = 1;
        packet.words[0] = kDestinationWord;
        if (!output->send(packet) || !wait_until([&] {
                return destination_word->load(std::memory_order_acquire) ==
                       kDestinationWord;
            })) {
            return fail(@"production UMP output event-list delivery failed",
                        source, destination);
        }

        // Callback replacement is control-thread synchronized, not an RT API.
        // Stress concurrent replacement against real CoreMIDI delivery so a
        // future unsynchronized std::function handoff is detected.
        received_word->store(0, std::memory_order_release);
        std::atomic<bool> keep_replacing{true};
        std::thread replacer([&] {
            for (int replacement = 0;
                 replacement < 10000 &&
                 keep_replacing.load(std::memory_order_acquire);
                 ++replacement) {
                input->set_receive_callback(
                    [received_word](const pulp::midi::UmpPacket& p, double) {
                        if (p.word_count > 0) {
                            received_word->store(p.words[0],
                                                 std::memory_order_release);
                        }
                    });
            }
        });
        for (uint32_t i = 1; i <= 64; ++i) {
            auto event = one_word_event_list(0x20000000 | i);
            if (MIDIReceivedEventList(source, &event) != noErr) {
                keep_replacing.store(false, std::memory_order_release);
                replacer.join();
                return fail(@"UMP callback replacement stress send failed",
                            source, destination);
            }
        }
        keep_replacing.store(false, std::memory_order_release);
        replacer.join();
        if (!wait_until([&] {
                return received_word->load(std::memory_order_acquire) != 0;
            })) {
            return fail(@"UMP callback replacement stress lost all delivery",
                        source, destination);
        }

        MIDIEndpointDispose(source);
        source = 0;
        MIDIEndpointDispose(destination);
        destination = 0;
        if (!wait_until([&] {
                return !input->is_open() && !output->is_open();
            }) || output->send(packet)) {
            return fail(@"cached UMP endpoint survived hot-unplug", 0, 0);
        }
        open_status = pulp::midi::UmpOpenStatus::Ok;
        if (session.open_endpoint(ump_source_id, &open_status) != nullptr ||
            open_status != pulp::midi::UmpOpenStatus::NotFound) {
            return fail(@"cached UMP open reported success after hot-unplug",
                        0, 0);
        }
    }

    bool disappeared = wait_until([&] {
        const auto inputs = system->enumerate_inputs();
        const auto outputs = system->enumerate_outputs();
        return find_port(inputs, source_id, source_name) == nullptr &&
               find_port(outputs, destination_id, destination_name) == nullptr;
    });
    if (!disappeared) {
        return fail(@"disposed virtual endpoints remained discoverable", 0, 0);
    }

    write_result(@"PASS\n");
    NSLog(@"PULP_COREMIDI_HARNESS: PASS source=%@ (%d) destination=%@ (%d)",
          source_ns, source_uid, destination_ns, destination_uid);
    return 0;
}

}  // namespace

int main() {
    @autoreleasepool {
        return run_harness();
    }
}
