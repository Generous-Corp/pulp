#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

#include <pulp/midi/device.hpp>

#include "../../core/midi/platform/mac/coremidi_shared_client.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kResultFile = "coremidi-result.txt";

void virtual_destination_read(const MIDIPacketList*, void*, void*) {}

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

int fail(NSString* reason, MIDIEndpointRef source, MIDIEndpointRef destination) {
    if (source) MIDIEndpointDispose(source);
    if (destination) MIDIEndpointDispose(destination);
    write_result([@"FAIL: " stringByAppendingString:reason]);
    NSLog(@"PULP_COREMIDI_HARNESS: FAIL: %@", reason);
    return 1;
}

int run_harness() {
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
    OSStatus status = MIDISourceCreate(client, (__bridge CFStringRef)source_ns,
                                       &source);
    if (status != noErr || !source) {
        return fail([NSString stringWithFormat:@"source create status=%d",
                                                static_cast<int>(status)],
                    source, destination);
    }
    status = MIDIDestinationCreate(client,
                                   (__bridge CFStringRef)destination_ns,
                                   &virtual_destination_read, nullptr,
                                   &destination);
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

    MIDIEndpointDispose(source);
    source = 0;
    MIDIEndpointDispose(destination);
    destination = 0;

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
