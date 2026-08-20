#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>

#include <pulp/midi/device.hpp>
#include <pulp/midi/ump_session.hpp>

#include "../../core/midi/platform/mac/coremidi_shared_client.h"
#include "../../core/midi/src/ump_session_backend.hpp"

#include <atomic>
#include <cmath>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kResultFile = "coremidi-result.txt";

struct OwnedMidiEndpoint {
    MIDIEndpointRef value = 0;

    ~OwnedMidiEndpoint() {
        if (value) MIDIEndpointDispose(value);
    }
};

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

double host_ticks_to_seconds(MIDITimeStamp ticks) {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return static_cast<double>(static_cast<long double>(ticks) *
                               static_cast<long double>(timebase.numer) /
                               static_cast<long double>(timebase.denom) /
                               1.0e9L);
}

MIDIEventList two_word_event_list(uint32_t first, uint32_t second,
                                  MIDITimeStamp timestamp) {
    MIDIEventList list;
    MIDIEventPacket* packet = MIDIEventListInit(&list, kMIDIProtocol_2_0);
    const uint32_t words[] = {first, second};
    MIDIEventListAdd(&list, sizeof(list), packet, timestamp, 2,
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
        {"401", "source-only", "Source Only", true},
        {"501", "destination-only", "Destination Only", false},
        {"601", "", "Unparented Source", true},
        {"701", "unbalanced", "Unbalanced Source A", true},
        {"702", "unbalanced", "Unbalanced Source B", true},
        {"703", "unbalanced", "Unbalanced Destination", false},
    };
    const auto grouped = pulp::midi::group_coremidi_ump_topology(census);
    if (grouped.size() != 11) return false;
    bool paired = false;
    std::size_t independent_count = 0;
    for (const auto& endpoint : grouped) {
        if (endpoint.id == "entity:pair") {
            paired = endpoint.direction.can_receive &&
                     endpoint.direction.can_send;
        } else if (endpoint.id == "101" || endpoint.id == "102") {
            independent_count += endpoint.direction.can_receive &&
                                 !endpoint.direction.can_send;
        } else if (endpoint.id == "201" || endpoint.id == "202") {
            independent_count += !endpoint.direction.can_receive &&
                                 endpoint.direction.can_send;
        } else if (endpoint.id == "401" || endpoint.id == "601" ||
                   endpoint.id == "701" || endpoint.id == "702") {
            independent_count += endpoint.direction.can_receive &&
                                 !endpoint.direction.can_send;
        } else if (endpoint.id == "501" || endpoint.id == "703") {
            independent_count += !endpoint.direction.can_receive &&
                                 endpoint.direction.can_send;
        }
    }
    return paired && independent_count == 10;
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
    NSString* unaffected_source_ns =
        [@"Pulp iOS Unaffected Source " stringByAppendingString:nonce];
    const std::string source_name = utf8(source_ns);
    const std::string destination_name = utf8(destination_ns);
    const std::string unaffected_source_name = utf8(unaffected_source_ns);

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
    OwnedMidiEndpoint unaffected_source;
    status = MIDISourceCreateWithProtocol(
        client, (__bridge CFStringRef)unaffected_source_ns,
        kMIDIProtocol_2_0, &unaffected_source.value);
    if (status != noErr || !unaffected_source.value) {
        return fail(
            [NSString stringWithFormat:@"unaffected source create status=%d",
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

    auto legacy_word = std::make_shared<std::atomic<uint32_t>>(0);
    auto legacy_timestamp = std::make_shared<std::atomic<double>>(0.0);
    auto legacy_input = system->create_input();
    if (!legacy_input ||
        !legacy_input->open(source_id,
            [legacy_word, legacy_timestamp](const pulp::midi::MidiEvent& event) {
                const auto* bytes = event.data();
                legacy_word->store((static_cast<uint32_t>(bytes[0]) << 16) |
                                       (static_cast<uint32_t>(bytes[1]) << 8) |
                                       static_cast<uint32_t>(bytes[2]),
                                   std::memory_order_release);
                legacy_timestamp->store(event.timestamp,
                                        std::memory_order_release);
            })) {
        return fail(@"production legacy CoreMIDI input open failed", source,
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
        std::string unaffected_ump_source_id;
        const bool ump_discovered = wait_until([&] {
            const auto endpoints = session.enumerate_endpoints();
            const auto* input = find_ump_endpoint(endpoints, source_name,
                                                  true, false);
            const auto* output = find_ump_endpoint(endpoints, destination_name,
                                                   false, true);
            const auto* unaffected_input = find_ump_endpoint(
                endpoints, unaffected_source_name, true, false);
            if (!input || !output || !unaffected_input ||
                input->id == output->id ||
                unaffected_input->id == input->id ||
                unaffected_input->id == output->id) {
                return false;
            }
            ump_source_id = input->id;
            ump_destination_id = output->id;
            unaffected_ump_source_id = unaffected_input->id;
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
        auto* unaffected_input =
            session.open_endpoint(unaffected_ump_source_id, &open_status);
        if (!unaffected_input ||
            open_status != pulp::midi::UmpOpenStatus::Ok ||
            !unaffected_input->is_open()) {
            return fail(@"production unaffected UMP source open failed", source,
                        destination);
        }
        auto unaffected_received_word =
            std::make_shared<std::atomic<uint32_t>>(0);
        unaffected_input->set_receive_callback(
            [unaffected_received_word](const pulp::midi::UmpPacket& packet,
                                       double) {
                if (packet.word_count > 0) {
                    unaffected_received_word->store(
                        packet.words[0], std::memory_order_release);
                }
            });

        // MT 0x6 is a complete one-word message. Following it with a valid
        // MT 0x2 message detects any private/incorrect size table that skips
        // the second packet instead of using the canonical UMP walker.
        constexpr uint32_t kSourceWord = 0x20903c7f;
        const MIDITimeStamp source_timestamp = mach_absolute_time();
        auto source_events = two_word_event_list(0x60000000, kSourceWord,
                                                 source_timestamp);
        status = MIDIReceivedEventList(source, &source_events);
        if (status != noErr || !wait_until([&] {
                return received_word->load(std::memory_order_acquire) ==
                           kSourceWord &&
                       legacy_word->load(std::memory_order_acquire) ==
                           0x903c7f &&
                       std::abs(legacy_timestamp->load(
                                    std::memory_order_acquire) -
                                host_ticks_to_seconds(source_timestamp)) < 1.0;
            })) {
            return fail(@"canonical UMP walk missed legacy/native delivery",
                        source, destination);
        }

        auto callback_generation =
            std::make_shared<std::atomic<uint32_t>>(0);
        constexpr uint32_t kGenerationOneWord = 0x20903d01;
        constexpr uint32_t kGenerationTwoWord = 0x20903e02;
        input->set_receive_callback(
            [callback_generation](const pulp::midi::UmpPacket& p, double) {
                if (p.word_count > 0 && p.words[0] == kGenerationOneWord) {
                    callback_generation->store(1, std::memory_order_release);
                }
            });
        auto generation_one = one_word_event_list(kGenerationOneWord);
        if (MIDIReceivedEventList(source, &generation_one) != noErr ||
            !wait_until([&] {
                return callback_generation->load(std::memory_order_acquire) ==
                       1;
            })) {
            return fail(@"first UMP callback generation was not observed",
                        source, destination);
        }
        input->set_receive_callback(
            [callback_generation](const pulp::midi::UmpPacket& p, double) {
                if (p.word_count > 0 && p.words[0] == kGenerationTwoWord &&
                    callback_generation->load(std::memory_order_acquire) == 1) {
                    callback_generation->store(2, std::memory_order_release);
                }
            });
        auto generation_two = one_word_event_list(kGenerationTwoWord);
        if (MIDIReceivedEventList(source, &generation_two) != noErr ||
            !wait_until([&] {
                return callback_generation->load(std::memory_order_acquire) ==
                       2;
            })) {
            return fail(@"replacement UMP callback generation/order failed",
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
            const uint32_t note = 0x30 + (i % 24);
            auto event = one_word_event_list(0x20900040 | (note << 8));
            if (MIDIReceivedEventList(source, &event) != noErr) {
                keep_replacing.store(false, std::memory_order_release);
                replacer.join();
                return fail(@"UMP callback replacement stress send failed",
                            source, destination);
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        keep_replacing.store(false, std::memory_order_release);
        replacer.join();
        if (!wait_until([&] {
                return received_word->load(std::memory_order_acquire) != 0;
            })) {
            return fail(input->is_open()
                            ? @"UMP callback replacement stress lost all delivery while open"
                            : @"UMP callback replacement stress endpoint closed",
                        source, destination);
        }

        auto retired_callback_count =
            std::make_shared<std::atomic<uint32_t>>(0);
        constexpr uint32_t kReopenedSourceWord = 0x20903f03;
        input->set_receive_callback(
            [retired_callback_count](const pulp::midi::UmpPacket& p, double) {
                if (p.word_count > 0 && p.words[0] == kReopenedSourceWord) {
                    retired_callback_count->fetch_add(
                        1, std::memory_order_acq_rel);
                }
            });

        const SInt32 retired_uid_candidates[] = {
            static_cast<SInt32>(static_cast<uint32_t>(source_uid) ^
                               0x40000000u),
            static_cast<SInt32>(static_cast<uint32_t>(source_uid) ^
                               0x20000000u),
            static_cast<SInt32>(static_cast<uint32_t>(source_uid) ^
                               0x10000000u),
        };
        SInt32 retired_uid = 0;
        for (const auto candidate : retired_uid_candidates) {
            if (candidate != 0 && candidate != source_uid &&
                candidate != destination_uid &&
                MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID,
                                             candidate) == noErr) {
                retired_uid = candidate;
                break;
            }
        }
        if (retired_uid == 0) {
            return fail(@"old UMP source UID reassignment failed", source,
                        destination);
        }
        const MIDIEndpointRef retired_source = source;

        std::atomic<bool> keep_sending{true};
        std::atomic<uint32_t> sender_attempts{0};
        std::vector<std::thread> senders;
        for (int sender = 0; sender < 4; ++sender) {
            senders.emplace_back([&] {
                for (int attempt = 0;
                     attempt < 200000 &&
                     keep_sending.load(std::memory_order_acquire);
                     ++attempt) {
                    output->send(packet);
                    sender_attempts.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
        if (!wait_until([&] {
                return sender_attempts.load(std::memory_order_acquire) >= 100;
            })) {
            keep_sending.store(false, std::memory_order_release);
            for (auto& sender : senders) sender.join();
            return fail(@"concurrent UMP sender stress did not start",
                        retired_source, destination);
        }
        MIDIEndpointDispose(destination);
        destination = 0;
        // The destination removal must retire its open sender. Do not also
        // require the independent source to close here: CoreMIDI is allowed to
        // report the earlier source UID edit as a property change rather than
        // remove/add, and an unrelated removal must not invalidate a healthy
        // input. The old source identity is retired by the fresh-census
        // open_endpoint() check below, which then proves the borrowed input
        // handle stays closed.
        if (!wait_until([&] {
                return !output->is_open();
            })) {
            keep_sending.store(false, std::memory_order_release);
            for (auto& sender : senders) sender.join();
            return fail(@"cached UMP destination survived topology change",
                        retired_source, 0);
        }
        constexpr uint32_t kUnaffectedSourceWord = 0x20904004;
        auto unaffected_event = one_word_event_list(kUnaffectedSourceWord);
        if (!unaffected_input->is_open() ||
            MIDIReceivedEventList(unaffected_source.value,
                                  &unaffected_event) != noErr ||
            !wait_until([&] {
                return unaffected_received_word->load(
                           std::memory_order_acquire) ==
                       kUnaffectedSourceWord;
            })) {
            keep_sending.store(false, std::memory_order_release);
            for (auto& sender : senders) sender.join();
            return fail(@"unrelated removal retired unaffected UMP source",
                        retired_source, 0);
        }
        open_status = pulp::midi::UmpOpenStatus::Ok;
        if (session.open_endpoint(ump_destination_id, &open_status) != nullptr ||
            open_status != pulp::midi::UmpOpenStatus::NotFound) {
            keep_sending.store(false, std::memory_order_release);
            for (auto& sender : senders) sender.join();
            return fail(@"concurrent UMP sender retirement stayed open",
                        retired_source, 0);
        }
        keep_sending.store(false, std::memory_order_release);
        for (auto& sender : senders) sender.join();
        if (output->send(packet)) {
            return fail(@"retired UMP sender accepted a packet",
                        retired_source, 0);
        }
        open_status = pulp::midi::UmpOpenStatus::Ok;
        if (session.open_endpoint(ump_source_id, &open_status) != nullptr ||
            open_status != pulp::midi::UmpOpenStatus::NotFound) {
            return fail(@"cached UMP open reported stale identity success",
                        retired_source, 0);
        }
        if (input->is_open()) {
            return fail(@"retired borrowed UMP handle did not stay closed",
                        retired_source, 0);
        }

        MIDIEndpointRef replacement_source = 0;
        status = MIDISourceCreateWithProtocol(
            client, (__bridge CFStringRef)source_ns, kMIDIProtocol_2_0,
            &replacement_source);
        if (status != noErr || !replacement_source ||
            MIDIObjectSetIntegerProperty(replacement_source,
                                         kMIDIPropertyUniqueID,
                                         source_uid) != noErr) {
            return fail(@"same-ID UMP source recreation failed",
                        retired_source, replacement_source);
        }
        source = replacement_source;
        if (!wait_until([&] {
                const auto endpoints = session.enumerate_endpoints();
                return find_ump_endpoint(endpoints, source_name,
                                         true, false) != nullptr;
            })) {
            return fail(@"same-ID UMP source was not rediscovered",
                        retired_source, source);
        }
        open_status = pulp::midi::UmpOpenStatus::NotFound;
        auto* reopened = session.open_endpoint(ump_source_id, &open_status);
        if (!reopened || reopened == input ||
            open_status != pulp::midi::UmpOpenStatus::Ok ||
            !reopened->is_open() || input->is_open()) {
            return fail(@"same-ID reopen violated borrowed-handle lifetime",
                        retired_source, source);
        }
        auto reopened_callback_count =
            std::make_shared<std::atomic<uint32_t>>(0);
        reopened->set_receive_callback(
            [reopened_callback_count](const pulp::midi::UmpPacket& p, double) {
                if (p.word_count > 0 && p.words[0] == kReopenedSourceWord) {
                    reopened_callback_count->fetch_add(
                        1, std::memory_order_acq_rel);
                }
            });
        auto reopened_event = one_word_event_list(kReopenedSourceWord);
        if (MIDIReceivedEventList(retired_source, &reopened_event) != noErr) {
            return fail(@"retired UMP source delivery setup failed",
                        retired_source, source);
        }
        for (int drain = 0; drain < 5; ++drain) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        if (MIDIReceivedEventList(source, &reopened_event) != noErr ||
            !wait_until([&] {
                return reopened_callback_count->load(
                           std::memory_order_acquire) == 1;
            })) {
            return fail(@"same-ID replacement callback delivery failed",
                        retired_source, source);
        }
        for (int drain = 0; drain < 5; ++drain) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        if (retired_callback_count->load(std::memory_order_acquire) != 0 ||
            reopened_callback_count->load(std::memory_order_acquire) != 1) {
            return fail(@"same-ID reopen left duplicate live callbacks",
                        retired_source, source);
        }
        MIDIEndpointDispose(retired_source);
        MIDIEndpointDispose(source);
        source = 0;
        if (!wait_until([&] { return !reopened->is_open(); })) {
            return fail(@"same-ID replacement survived second hot-unplug",
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
