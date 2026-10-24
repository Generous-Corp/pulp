// libFuzzer entry point over Standard MIDI File import.
//
// A .mid dropped on the timeline is untrusted bytes: chunk framing, variable
// length quantities, running status and meta payload lengths all come from the
// file. The input is imported twice, under the default strict policy and under
// the lenient policy with ceilings small enough that the limit checks run on
// ordinary inputs rather than only on multi-megabyte ones. A rejected import
// is the expected outcome for most inputs; the property is no crash and no
// allocation beyond the authorized ceilings.

#include <pulp/timeline/smf.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);

    (void)pulp::timeline::import_smf(bytes);

    pulp::timeline::SmfImportOptions lenient;
    lenient.unsupported_events = pulp::timeline::SmfUnsupportedEventPolicy::IgnoreNonNote;
    lenient.limits.max_tracks = 64;
    lenient.limits.max_events = 65'536;
    lenient.limits.max_notes = 32'768;
    lenient.limits.max_concurrent_notes = 512;
    lenient.limits.max_tempo_points = 256;
    lenient.limits.max_meter_points = 256;
    lenient.limits.max_payload_bytes = 4096;
    lenient.limits.max_track_name_bytes = 256;
    (void)pulp::timeline::import_smf(bytes, lenient);
    return 0;
}
