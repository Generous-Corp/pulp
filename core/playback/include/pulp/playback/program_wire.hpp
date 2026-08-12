#pragma once

/// @file program_wire.hpp
/// The flat, pointer-free byte layout one compiled program generation is
/// published in, plus its native encoder and validating decoder.
///
/// PlaybackProgram is a graph of `shared_ptr` and `std::vector`. That is the
/// right shape for a process-local renderer and an impossible shape for any
/// consumer that does not share the producer's address space: an AudioWorklet
/// and its Worker are separate linear memories, and a helper process shares no
/// heap with its host. Neither can follow a pointer the compiler wrote. This
/// wire is the crossing form — one contiguous, self-describing byte range that
/// carries indices where the program carries pointers, so publication becomes a
/// copy of bytes plus a generation counter rather than a graph walk.
///
/// **Byte order and alignment.** Records are native-layout structs with
/// explicit padding, written and read by `memcpy`, so the layout is
/// little-endian by construction and a `static_assert` below refuses to compile
/// on a big-endian host rather than emitting bytes a reader would silently
/// misread. Every record is a multiple of eight bytes and every section starts
/// at an eight-byte payload offset, so a decoder can borrow typed spans
/// directly out of the buffer with no copy and no unaligned load; a buffer
/// whose base address is not eight-byte aligned is rejected rather than read.
/// This is the deliberate trade: portability to a hypothetical big-endian
/// target is spent to buy a decode that allocates nothing at all, which is what
/// the adopting realtime realm actually needs.
///
/// **Trailing data.** Sections tile the payload exactly — ascending, gapless,
/// starting at zero and ending at `payload_bytes`. There is no slack a writer
/// could hide meaning in and no trailing region a reader would have to decide
/// about, so a length that disagrees with the content is a rejection rather
/// than an interpretation.
///
/// **Versioning.** The header carries both the writer's `version` and the
/// lowest `min_reader_version` that can read the payload correctly. A reader
/// rejects a payload whose `min_reader_version` exceeds its own version and
/// otherwise proceeds; it never guesses. Additive growth does not need either
/// number to move, because unknown sections marked `kSectionOptional` are
/// skipped while unknown sections without that flag are rejected — the writer
/// states whether ignoring the new data still renders the right audio, and a
/// reader that cannot honour it refuses instead of rendering a program it only
/// partly understands. This posture is deliberately not the document command
/// schema's exact-equality gate: a command schema mismatch stops an edit, while
/// a program wire mismatch would stop playback in a realm that cannot be
/// upgraded in lockstep with its producer, so this format needs a migration
/// story and gets one.
///
/// **Canonical encoding.** Every known section is always emitted, in ascending
/// id order, even when empty. One program, one tempo-point sequence, and one
/// producer epoch have exactly one byte encoding, which is what makes a
/// byte-level golden a real guard rather than a record of whatever the encoder
/// happened to do. Note the unit: one *program*, not one document. Each
/// automation lane carries its program's instance token, which is minted per
/// compile, so two compiles of one document are two programs and encode to two
/// byte ranges — deliberately, since that difference is the identity a consumer
/// needs. A golden over a fixed document therefore normalises those tokens
/// before hashing; everything else in the payload is still fixed by the input.
///
/// **Adoption contract — `(producer_epoch, generation)` decides the producer
/// and the ordering; the lane's `instance_token` decides sameness.**
/// A consumer decides three ways about an arriving payload:
///
///   - `producer_epoch` differs from the adopted one → a *different producer*.
///     Reset any carried cursor state and adopt unconditionally. Comparing
///     generations across producers is meaningless.
///   - Same epoch, `generation` greater → the same producer advancing. Adopt.
///   - Same epoch, same generation → **not** necessarily the same publication.
///     `generation` is supplied by the caller rather than minted per compile,
///     so one producer can publish two different programs under one generation.
///     Compare each lane's `instance_token`: equal means genuinely the same
///     program, so carried cursor state is still valid and is kept; different
///     means a new program that happens to share a generation, and the lane is
///     re-adopted. Deciding this case on `(lane_id, generation)` alone is what
///     renders a stale curve with nothing malformed for a decoder to reject.
///
/// Neither program-level component is sufficient alone, because the two
/// counters have different scopes. `generation` is **per store and restarts**:
/// a fresh `PlaybackProgramStore` publishes generation 1, so a producer that is
/// torn down and recreated — a page rebuilding a Worker while the AudioWorklet
/// survives — begins again at 1 while the consumer sits at N. Judged on
/// generation alone that is not monotonic, so every publish is refused and the
/// consumer renders a stale program indefinitely, with nothing distinguishing
/// "refused once" from "refused forever". Judged on the epoch first, it is
/// simply a new producer. In the other direction, two *producers* of one
/// document both mint generation 1 for the same lane, and without an epoch a
/// consumer would read that as the same publication.
///
/// The epoch answers both of those and only those: they are questions about
/// *producers*. It does not separate two programs from **one** producer, which
/// is the more common case. So the full lane identity a consumer compares is
/// `(producer_epoch, lane_id, generation, instance_token)`, and no proper
/// subset of it is sufficient — see `ProgramWireAutomationLaneRecord`.
///
/// **What this format is sufficient for.** A note, automation, and mixer
/// program. It structurally cannot express an audio-clip program: there is no
/// section for clip audio regions and no way to name decoded media, and the
/// encoder refuses such a program rather than emitting a thinner one. A
/// consumer can therefore rely on "decoded successfully" meaning "no audio
/// content was dropped on the way here", and a producer that needs audio needs
/// a format change rather than an extra channel alongside this one.
///
/// **What the wire carries, and what it does not.** It carries the note,
/// automation, and mixer spine a realtime consumer needs: identity and
/// generation, the tempo points and sample rate, per-track note events and note
/// modifiers, clip and device-placement ordering, the mixer's constants and
/// which lane supersedes them, and every automation lane's compiled segments in
/// both the tick and sample domains. It does not carry decoded audio: the
/// asset pool is bulk media that is already content-hash addressed, and a
/// generation wire that inlined it would republish gigabytes per edit. It does
/// not carry the audio clip programs either — those are derived from the
/// document and the asset pool, and three of their members are derived-cache
/// pointers whose types are still moving; the encoder refuses a program with an
/// audio track rather than dropping it silently. It does not carry
/// per-track production declarations. Those remain process-local in this wire
/// version; a track whose declaration is not the default synchronous,
/// deterministic, zero-lookahead contract is refused rather than encoded with
/// a stronger replay claim than it earned. It does not carry
/// AudioRendererLimits, whose two dozen fields are mostly offline-stretch and
/// converter-cache budgets governing the compiler's host rather than the
/// adopting realm. AutomationPlaybackLimits is carried, because those ceilings
/// size the arrays a block render writes into.

#include <pulp/playback/automation_cursor.hpp>
#include <pulp/playback/automation_limits.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace pulp::playback {

namespace detail {
struct ProgramWireDecoder;
}

static_assert(std::endian::native == std::endian::little,
              "the program wire is a native-layout little-endian byte range; a big-endian "
              "host must gain an explicit byte-swapping codec before it can produce or "
              "consume one, not silently emit records a reader would misread");

/// The format this build writes and is able to read.
///
/// Version 2 widened `ProgramWireAutomationLaneRecord` to carry the lane's
/// instance token. A version 1 reader would not merely miss that field: it
/// divides the section's byte length by its own 48-byte record, and a lane
/// count whose 56-byte length happens to divide by 48 — six lanes, for one —
/// decodes as seven records of shifted garbage rather than failing. So
/// `min_reader_version` moves with it, which is what makes that reader refuse
/// the payload instead of misreading it.
inline constexpr std::uint16_t kProgramWireVersion = 2;

/// Written into every payload this build produces. Bump only when a reader at
/// an earlier version would misread the bytes rather than merely miss data.
inline constexpr std::uint16_t kProgramWireMinReaderVersion = 2;

/// Spells `PLPW` in the first four bytes of every payload.
inline constexpr std::uint32_t kProgramWireMagic = 0x5750'4C50u;

/// A track's mixer control that no automation lane supersedes.
inline constexpr std::uint32_t kProgramWireNoLane = 0xFFFF'FFFFu;

enum class ProgramWireSection : std::uint32_t {
    Program = 1,
    TempoPoints = 2,
    Tracks = 3,
    ClipIds = 4,
    NoteEvents = 5,
    NoteModifiers = 6,
    DevicePlacementIds = 7,
    AutomationLanes = 8,
    AutomationSegments = 9,
};

/// The section is safe to ignore: a reader that does not know this id still
/// renders the program the writer meant. Absent, an unknown id is a rejection.
inline constexpr std::uint32_t kProgramWireSectionOptional = 1u << 0u;

/// How often the adopting realm must evaluate one automation lane. Which
/// parameters warrant per-sample evaluation is an open product decision; the
/// wire commits only to carrying the answer per lane. Every lane this build
/// encodes is `SampleAccurate`, which is what AutomationCursor already does.
enum class AutomationEvaluationRate : std::uint8_t {
    SampleAccurate = 0,
    BlockRate = 1,
};

enum class ProgramWireTargetKind : std::uint8_t {
    DeviceParameter = 0,
    TrackMixer = 1,
};

enum class ProgramWireErrorCode : std::uint8_t {
    /// The destination span is smaller than the encoding needs. `detail`
    /// carries the required byte count.
    InsufficientCapacity,
    /// The buffer's base address is not eight-byte aligned, so typed spans
    /// borrowed from it would be misaligned.
    MisalignedBuffer,
    /// The supplied tempo points did not compile the program's tempo map.
    TempoMapMismatch,
    /// A track carries an audio renderer program, which this version does not
    /// represent. Refused rather than dropped. `detail` carries the track id.
    AudioProgramUnsupported,
    /// A track has a non-default production declaration, which this wire
    /// version cannot carry. Refused rather than silently strengthening the
    /// adopting realm's replay claim. `detail` carries the track id.
    ProductionDeclarationUnsupported,
    /// A mixer control points at an automation program that is not one of the
    /// owning track's lanes. `detail` carries the track id.
    MixerAutomationUnresolved,
    /// A count exceeded the 32-bit index domain the flat layout addresses.
    CountOverflow,

    /// Fewer bytes than the fixed header, the section directory, or the payload
    /// the header declares.
    ShortBuffer,
    BadMagic,
    /// The payload's `min_reader_version` exceeds this build's version.
    UnsupportedVersion,
    /// `header_bytes` disagrees with this format's fixed header size.
    BadHeaderSize,
    ChecksumMismatch,
    /// A section's offset plus length overflows or leaves the payload.
    SectionOutOfBounds,
    /// Sections do not tile the payload exactly: unsorted, gapped, overlapping,
    /// or not reaching `payload_bytes`.
    SectionsNotTiled,
    /// A section's payload offset is not eight-byte aligned.
    SectionMisaligned,
    /// Known sections must appear in ascending canonical id order. Optional
    /// unknown sections may be interleaved only in ascending id order.
    NonCanonicalSectionOrder,
    DuplicateSection,
    /// An unknown section id that the writer did not mark optional.
    UnknownSection,
    /// A known section id is absent.
    MissingSection,
    /// The buffer holds bytes past the payload the header declares.
    TrailingData,
    /// A section's byte length is not a whole number of its records.
    BadRecordSize,
    /// A section holds a record count the format does not admit.
    BadSectionCardinality,
    /// A track's `(first, count)` range leaves the section it indexes.
    RangeOutOfBounds,
    /// Track and lane ranges must exclusively partition their sections in
    /// canonical track/lane order; aliases and unowned records are rejected.
    NonCanonicalRangeOwnership,
    /// A device lane names no placement in its owning track.
    AutomationTargetUnresolved,
    /// Two lanes in one track drive the same device parameter or mixer control.
    DuplicateAutomationTarget,
    /// A publication from the active producer regresses one lane generation.
    StaleLaneGeneration,
    /// A record holds a value outside its enumeration.
    InvalidEnum,
    /// A note modifier is not a combination the document model admits — a zero
    /// condition period, an offset past it, a ratchet count out of range. The
    /// renderer divides by the period, so this is a rejection and not a clamp.
    MalformedNoteModifier,
    /// An automation segment runs backwards or carries a non-finite value,
    /// either of which would propagate through every sample downstream.
    MalformedSegment,
    InvalidSampleRate,
    /// The carried automation ceilings are not a configuration the renderer
    /// accepts.
    InvalidLimits,
    /// The producer epoch is zero. A zero epoch would compare equal to every
    /// other zero, making two unrelated producers look like one, so it is
    /// refused at both ends rather than silently acting as a wildcard.
    InvalidProducerEpoch,
    /// An automation lane's instance token is zero. Same reasoning one level
    /// down: two lanes carrying zero would compare equal, so a consumer would
    /// report Unchanged for a program it has never seen. `detail` carries the
    /// lane's index within the section.
    InvalidInstanceToken,
};

struct ProgramWireError {
    ProgramWireErrorCode code = ProgramWireErrorCode::ShortBuffer;
    /// The section id the failure concerns, or zero when it concerns the header
    /// or the directory as a whole.
    std::uint32_t section = 0;
    /// Failure-specific context: a required byte count, an offset, a record
    /// index, an item id, or the offending value.
    std::uint64_t detail = 0;
};

#pragma pack(push, 8)

/// The fixed prefix every payload starts with. `body_checksum` covers the
/// section directory and the payload, which is everything whose meaning a
/// reader has to trust before it can walk them.
struct ProgramWireHeader {
    std::uint32_t magic = kProgramWireMagic;
    std::uint16_t version = kProgramWireVersion;
    std::uint16_t min_reader_version = kProgramWireMinReaderVersion;
    std::uint32_t header_bytes = 0;
    std::uint32_t section_count = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t body_checksum = 0;
    std::uint64_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;
    std::uint64_t reserved3 = 0;
};

struct ProgramWireSectionEntry {
    std::uint32_t id = 0;
    std::uint32_t flags = 0;
    /// Byte offset from the first payload byte, not from the buffer base.
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

/// The whole-program scalars, including the tempo map's sample rate and the
/// automation ceilings the adopting realm must render within.
struct ProgramWireProgramRecord {
    /// Identifies the producer that minted `generation`. Nonzero always; a
    /// payload carrying zero is rejected rather than treated as a wildcard that
    /// would match every producer. See the adoption contract above for what a
    /// consumer must do when this changes.
    std::uint64_t producer_epoch = 0;
    std::uint64_t generation = 0;
    std::uint64_t document_revision = 0;
    std::uint64_t project_id = 0;
    std::uint64_t sequence_id = 0;
    std::uint64_t generated_id_base = 0;
    std::uint64_t sample_rate_numerator = 0;
    std::uint64_t sample_rate_denominator = 0;
    std::uint32_t max_device_placements_per_track = 0;
    std::uint32_t max_lanes_per_track = 0;
    std::uint32_t max_points_per_lane = 0;
    std::uint32_t max_points_per_track = 0;
    std::uint32_t max_intersecting_segments_per_block = 0;
    std::uint32_t max_events_per_device_per_block = 0;
};

/// The editable tempo points, not the compiled segments. Compilation from
/// points is deterministic and already validated, so carrying the inputs gives
/// the reader the same map the writer had plus that validation for free, and
/// CompiledTempoMap::matches() is the identity check that keeps the two honest.
struct ProgramWireTempoPointRecord {
    std::int64_t tick = 0;
    /// `bpm` as its IEEE-754 bit pattern, so a payload round-trips bit-exactly.
    std::uint64_t bpm_bits = 0;
    std::uint8_t curve_to_next = 0;
    std::uint8_t pad[7] = {};
};

/// One track. Every span the TrackProgram exposes becomes a `(first, count)`
/// range into the flat section that holds every track's entries end to end.
struct ProgramWireTrackRecord {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint32_t clip_first = 0;
    std::uint32_t clip_count = 0;
    std::uint32_t note_event_first = 0;
    std::uint32_t note_event_count = 0;
    std::uint32_t note_modifier_first = 0;
    std::uint32_t note_modifier_count = 0;
    std::uint32_t device_placement_first = 0;
    std::uint32_t device_placement_count = 0;
    std::uint32_t automation_lane_first = 0;
    std::uint32_t automation_lane_count = 0;
    std::uint64_t expanded_clip_count = 0;
    std::uint64_t expanded_note_event_count = 0;
    std::uint64_t generated_id_start = 0;
    std::uint64_t generated_id_count = 0;
    float mixer_gain_linear = 1.0f;
    float mixer_pan = 0.0f;
    /// Index within this track's own automation lane range, or
    /// kProgramWireNoLane. Track-relative rather than global so a reader that
    /// has validated the range needs no second bounds check.
    std::uint32_t mixer_gain_lane = kProgramWireNoLane;
    std::uint32_t mixer_pan_lane = kProgramWireNoLane;
    std::uint8_t provider_selected = 0;
    std::uint8_t provider_available_mask = 0;
    std::uint8_t state_policy = 0;
    /// Bit 0: the track carries an automation program at all. A track with no
    /// program and a track with an empty one are different states, and the lane
    /// count alone cannot tell them apart.
    std::uint8_t flags = 0;
    std::uint32_t pad = 0;
};

inline constexpr std::uint8_t kProgramWireTrackHasAutomationProgram = 1u << 0u;

struct ProgramWireIdRecord {
    std::uint64_t value = 0;
};

/// This happens to hold the same fields in the same order as
/// `NoteProgramEvent`, and that correspondence is **coincidental and must not
/// be relied on**. Do not `reinterpret_cast` between the two, in a native
/// adapter or anywhere else: `NoteProgramEvent` is a program type free to gain
/// a field, while this is a wire type whose layout is frozen by the byte
/// golden, and the day they diverge the cast keeps compiling and starts
/// reading the wrong offsets. Copy field by field; the encoder does.
struct ProgramWireNoteEventRecord {
    std::int64_t sample = 0;
    std::int64_t tick = 0;
    std::uint64_t clip_id = 0;
    std::uint64_t note_id = 0;
    std::uint16_t velocity = 0;
    std::uint8_t pitch = 0;
    std::uint8_t channel = 0;
    std::uint8_t kind = 0;
    std::uint8_t pad[3] = {};
};

struct ProgramWireNoteModifierRecord {
    std::uint64_t draw_key = 0;
    std::uint64_t note_id = 0;
    std::uint16_t probability = 0;
    std::uint16_t condition_period = 0;
    std::uint16_t condition_offset = 0;
    std::uint16_t ratchet_count = 0;
    std::uint8_t condition = 0;
    std::uint8_t pad[7] = {};
};

/// `instance_token` is the producer's own `AutomationProgram::instance_token()`
/// carried verbatim, because lane identity is **not** `(lane_id, generation)`
/// alone. In-process, `AutomationCursor` decides Unchanged on the lane key
/// *and* the token together, so the token is what stops two different programs
/// that share a key from being mistaken for one; a consumer computing Unchanged
/// without it reaches the opposite answer to the cursor and renders a stale
/// lane, silently, with no decode error to notice.
///
/// The objection this answers is that the token is a producer-process-local
/// allocation counter, so a foreign one names nothing a consumer can look up.
/// True, and beside the point: a consumer never compares a foreign token to one
/// of its own. It compares two foreign tokens **to each other, within one
/// `producer_epoch`**, where they came from the same counter and mean exactly
/// what they mean in process. Across epochs they are indeed incomparable — and
/// across epochs the epoch has already decided, so the token is not consulted.
///
/// Equality only. A larger token does not mean *newer* on the wire: ordering is
/// `(producer_epoch, generation)`'s job, and a producer is free to publish
/// programs compiled out of order. Zero is refused for the reason a zero epoch
/// is — it would compare equal to every other zero and quietly merge two
/// distinct programs into one — but at the decoder only, not at both ends like
/// the epoch: `AutomationProgram`'s constructor is private to the compiler,
/// which always mints a nonzero token, so an encoder-side check would be
/// unreachable. The decoder is where a foreign payload arrives, which is the
/// end that needs it.
///
/// The incremental compiler reuses a lane's program when that lane did not
/// change, so its token is stable across a publish that touched only its
/// neighbours. Per-lane rather than per-publication is what preserves that:
/// a consumer re-adopts the lanes that actually moved and keeps its cursor
/// state for the rest.
struct ProgramWireAutomationLaneRecord {
    std::uint64_t lane_id = 0;
    std::uint64_t generation = 0;
    /// Nonzero always. Comparable only against another token carrying the same
    /// `producer_epoch`, and only for equality.
    std::uint64_t instance_token = 0;
    std::uint32_t segment_first = 0;
    std::uint32_t segment_count = 0;
    float leading_value = 0.0f;
    std::uint8_t target_kind = 0;
    std::uint8_t evaluation_rate = 0;
    std::uint16_t pad0 = 0;
    /// Zero unless `target_kind` is DeviceParameter.
    std::uint64_t device_placement_id = 0;
    /// Zero unless `target_kind` is DeviceParameter.
    std::uint32_t device_param_id = 0;
    /// Zero unless `target_kind` is TrackMixer.
    std::uint8_t mixer_parameter = 0;
    std::uint8_t pad1[3] = {};
};

/// Deliberately not laid out like `AutomationProgramSegment`: this groups the
/// three floats and puts `interpolation` last so the record packs to 48 bytes
/// with one run of padding, where the program type interleaves them. The
/// divergence is intentional, which is a second reason no wire record should
/// ever be cast to or from its program counterpart.
struct ProgramWireAutomationSegmentRecord {
    std::int64_t start_tick = 0;
    std::int64_t end_tick = 0;
    std::int64_t start_sample = 0;
    std::int64_t end_sample = 0;
    float start_value = 0.0f;
    float end_value = 0.0f;
    float curvature = 0.0f;
    std::uint8_t interpolation = 0;
    std::uint8_t pad[3] = {};
};

#pragma pack(pop)

// A record whose size changes is a wire-format change. These are the
// compile-time half of the format's stability guard; the byte-level golden in
// the tests is the other half.
static_assert(sizeof(ProgramWireHeader) == 64);
static_assert(sizeof(ProgramWireSectionEntry) == 24);
static_assert(sizeof(ProgramWireProgramRecord) == 88);
static_assert(sizeof(ProgramWireTempoPointRecord) == 24);
static_assert(sizeof(ProgramWireTrackRecord) == 112);
static_assert(sizeof(ProgramWireIdRecord) == 8);
static_assert(sizeof(ProgramWireNoteEventRecord) == 40);
static_assert(sizeof(ProgramWireNoteModifierRecord) == 32);
static_assert(sizeof(ProgramWireAutomationLaneRecord) == 56);
static_assert(sizeof(ProgramWireAutomationSegmentRecord) == 48);

/// Every record is a multiple of eight bytes, so tiling sections back to back
/// keeps each one eight-byte aligned without a padding section between them.
static_assert(sizeof(ProgramWireHeader) % 8 == 0);
static_assert(sizeof(ProgramWireSectionEntry) % 8 == 0);
static_assert(sizeof(ProgramWireProgramRecord) % 8 == 0);
static_assert(sizeof(ProgramWireTempoPointRecord) % 8 == 0);
static_assert(sizeof(ProgramWireTrackRecord) % 8 == 0);
static_assert(sizeof(ProgramWireIdRecord) % 8 == 0);
static_assert(sizeof(ProgramWireNoteEventRecord) % 8 == 0);
static_assert(sizeof(ProgramWireNoteModifierRecord) % 8 == 0);
static_assert(sizeof(ProgramWireAutomationLaneRecord) % 8 == 0);
static_assert(sizeof(ProgramWireAutomationSegmentRecord) % 8 == 0);

static_assert(std::is_trivially_copyable_v<ProgramWireHeader>);
static_assert(std::is_trivially_copyable_v<ProgramWireProgramRecord>);
static_assert(std::is_trivially_copyable_v<ProgramWireTrackRecord>);
static_assert(std::is_trivially_copyable_v<ProgramWireNoteEventRecord>);
static_assert(std::is_trivially_copyable_v<ProgramWireAutomationLaneRecord>);
static_assert(std::is_trivially_copyable_v<ProgramWireAutomationSegmentRecord>);

/// The alignment a payload's base address must satisfy for a decoder to borrow
/// typed spans out of it.
inline constexpr std::size_t kProgramWireAlignment = 8;

/// FNV-1a over 64 bits. Detects the truncation, splice, and single-field
/// corruption a transport can produce; it is not a defence against a chosen
/// forgery, which the realm boundary's own trust model has to answer.
constexpr std::uint64_t program_wire_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ull;
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte));
        hash *= 0x0000'0100'0000'01b3ull;
    }
    return hash;
}

/// A validated, zero-copy view over one encoded program generation.
///
/// Every span borrows from the bytes handed to decode_program_wire(), which
/// must outlive the view. Construction allocates nothing, which is the strongest
/// available answer to "decode allocates only from the provided arena": the
/// adopting realtime realm needs no arena because it needs no allocation.
class ProgramWireView {
  public:
    ProgramWireView() = default;

    std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    const ProgramWireHeader& header() const noexcept {
        return *header_;
    }
    const ProgramWireProgramRecord& program() const noexcept {
        return *program_;
    }
    std::uint64_t producer_epoch() const noexcept {
        return program_->producer_epoch;
    }
    timebase::RationalRate sample_rate() const noexcept {
        return {program_->sample_rate_numerator, program_->sample_rate_denominator};
    }
    AutomationPlaybackLimits automation_limits() const noexcept;

    std::span<const ProgramWireTempoPointRecord> tempo_points() const noexcept {
        return tempo_points_;
    }
    std::span<const ProgramWireTrackRecord> tracks() const noexcept {
        return tracks_;
    }
    std::span<const ProgramWireIdRecord> clip_ids() const noexcept {
        return clip_ids_;
    }
    std::span<const ProgramWireNoteEventRecord> note_events() const noexcept {
        return note_events_;
    }
    std::span<const ProgramWireNoteModifierRecord> note_modifiers() const noexcept {
        return note_modifiers_;
    }
    std::span<const ProgramWireIdRecord> device_placement_ids() const noexcept {
        return device_placement_ids_;
    }
    std::span<const ProgramWireAutomationLaneRecord> automation_lanes() const noexcept {
        return automation_lanes_;
    }
    std::span<const ProgramWireAutomationSegmentRecord> automation_segments() const noexcept {
        return automation_segments_;
    }

    // Every range below was bounds-checked at decode, so these never read past
    // their section.
    std::span<const ProgramWireIdRecord>
    clip_ids_for(const ProgramWireTrackRecord& track) const noexcept {
        return clip_ids_.subspan(track.clip_first, track.clip_count);
    }
    std::span<const ProgramWireNoteEventRecord>
    note_events_for(const ProgramWireTrackRecord& track) const noexcept {
        return note_events_.subspan(track.note_event_first, track.note_event_count);
    }
    std::span<const ProgramWireNoteModifierRecord>
    note_modifiers_for(const ProgramWireTrackRecord& track) const noexcept {
        return note_modifiers_.subspan(track.note_modifier_first, track.note_modifier_count);
    }
    std::span<const ProgramWireIdRecord>
    device_placement_ids_for(const ProgramWireTrackRecord& track) const noexcept {
        return device_placement_ids_.subspan(track.device_placement_first,
                                             track.device_placement_count);
    }
    std::span<const ProgramWireAutomationLaneRecord>
    automation_lanes_for(const ProgramWireTrackRecord& track) const noexcept {
        return automation_lanes_.subspan(track.automation_lane_first, track.automation_lane_count);
    }
    std::span<const ProgramWireAutomationSegmentRecord>
    segments_for(const ProgramWireAutomationLaneRecord& lane) const noexcept {
        return automation_segments_.subspan(lane.segment_first, lane.segment_count);
    }

  private:
    friend struct detail::ProgramWireDecoder;

    std::span<const std::byte> bytes_;
    const ProgramWireHeader* header_ = nullptr;
    const ProgramWireProgramRecord* program_ = nullptr;
    std::span<const ProgramWireTempoPointRecord> tempo_points_;
    std::span<const ProgramWireTrackRecord> tracks_;
    std::span<const ProgramWireIdRecord> clip_ids_;
    std::span<const ProgramWireNoteEventRecord> note_events_;
    std::span<const ProgramWireNoteModifierRecord> note_modifiers_;
    std::span<const ProgramWireIdRecord> device_placement_ids_;
    std::span<const ProgramWireAutomationLaneRecord> automation_lanes_;
    std::span<const ProgramWireAutomationSegmentRecord> automation_segments_;
};

/// A nonzero value that is stable for this process and differs between
/// processes. Producers with no better source of identity can stamp payloads
/// with this; one that owns a longer-lived notion of "producer" (a store, a
/// session) should mint its own and keep it for that lifetime. It is never
/// zero, so it can be handed straight to `encode_program_wire`.
std::uint64_t program_wire_process_epoch() noexcept;

/// Bytes `encode_program_wire` needs for this program. Runs the same refusals
/// the encoder does, so a caller that sizes a buffer learns about an
/// unrepresentable program before it allocates one. The producer epoch does not
/// affect the size and is therefore not required here.
runtime::Result<std::size_t, ProgramWireError>
program_wire_encoded_size(const PlaybackProgram& program,
                          std::span<const timebase::TempoPoint> tempo_points) noexcept;

/// Writes `program` into `out` and returns the bytes written. A destination
/// larger than the encoding keeps its trailing bytes untouched, and those bytes
/// are not part of the payload — hand the decoder `out.first(written)`, since a
/// buffer longer than its declared payload is rejected as trailing data.
/// `tempo_points`
/// must be the points that compiled the program's tempo map; the compiled
/// segments are private and derived, so the wire carries the inputs and this
/// check is what proves they are the right ones. Allocates nothing: the caller
/// owns the destination, which is what lets a producer write straight into a
/// shared ring.
/// `producer_epoch` identifies whoever minted this program's generation and
/// must be nonzero and stable for that producer's lifetime; it is a required
/// argument rather than a defaulted one so a caller cannot omit producer
/// identity without noticing.
runtime::Result<std::size_t, ProgramWireError>
encode_program_wire(const PlaybackProgram& program,
                    std::span<const timebase::TempoPoint> tempo_points,
                    std::uint64_t producer_epoch, std::span<std::byte> out) noexcept;

/// Validates `bytes` and returns a borrowing view. This is untrusted-input
/// surface the moment a payload crosses a realm, so nothing is assumed: the
/// header, the checksum, the section tiling, every record count, every
/// `(first, count)` range, and every enumerated field are checked before any
/// accessor can be called.
runtime::Result<ProgramWireView, ProgramWireError>
decode_program_wire(std::span<const std::byte> bytes) noexcept;

/// Whether `view` carries exactly `program` compiled from `tempo_points`.
/// Structural equality by value — every field the wire carries compared against
/// its source — because the two share no pointers and shared-pointer identity
/// is precisely what the wire exists to leave behind. The tempo points are
/// passed rather than recovered so the comparison allocates nothing. A program
/// the encoder would refuse compares false.
bool program_wire_matches(const ProgramWireView& view, const PlaybackProgram& program,
                          std::span<const timebase::TempoPoint> tempo_points,
                          std::uint64_t producer_epoch) noexcept;

/// A unique lease over immutable publication bytes. Moving the pin transfers
/// the right to keep the bytes alive; rejected candidates and retired active
/// publications are returned to the caller in ProgramWireAdoptionResult.
class ProgramWireBytePin {
  public:
    ProgramWireBytePin() = default;
    explicit ProgramWireBytePin(std::span<const std::byte> bytes,
                                std::uintptr_t owner_token = 0) noexcept
        : bytes_(bytes), owner_token_(owner_token) {}
    ProgramWireBytePin(const ProgramWireBytePin&) = delete;
    ProgramWireBytePin& operator=(const ProgramWireBytePin&) = delete;
    ProgramWireBytePin(ProgramWireBytePin&& other) noexcept
        : bytes_(other.bytes_), owner_token_(other.owner_token_) {
        other.bytes_ = {};
        other.owner_token_ = 0;
    }
    ProgramWireBytePin& operator=(ProgramWireBytePin&& other) noexcept {
        if (this != &other) {
            bytes_ = other.bytes_;
            owner_token_ = other.owner_token_;
            other.bytes_ = {};
            other.owner_token_ = 0;
        }
        return *this;
    }

    std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    std::uintptr_t owner_token() const noexcept {
        return owner_token_;
    }
    explicit operator bool() const noexcept {
        return !bytes_.empty();
    }

  private:
    std::span<const std::byte> bytes_;
    std::uintptr_t owner_token_ = 0;
};

enum class ProgramWireConsumerCode : std::uint8_t {
    Ok,
    DecodeRejected,
    TempoMapMismatch,
    StateCapacityExceeded,
    StalePublication,
    MissingProgram,
    InvalidTransport,
    OutputCapacityExceeded,
    CursorRejected,
};

enum class ProgramWireAdoption : std::uint8_t {
    Adopted,
    Unchanged,
    Rejected,
};

struct ProgramWireAdoptionResult {
    ProgramWireConsumerCode code = ProgramWireConsumerCode::Ok;
    ProgramWireAdoption adoption = ProgramWireAdoption::Unchanged;
    ProgramWireError wire_error{};
    /// Candidate on reject/unchanged, or the previous active pin on adoption.
    ProgramWireBytePin returned_pin;
};

struct ProgramWireLaneIdentity {
    std::uint64_t producer_epoch = 0;
    ProgramGeneration program_generation = 0;
    timeline::ItemId track_id;
    ProgramGeneration track_generation = 0;
    timeline::ItemId lane_id;
    ProgramGeneration lane_generation = 0;
    AutomationProgramInstanceToken instance_token;
    constexpr bool operator==(const ProgramWireLaneIdentity&) const = default;
};

/// One caller-provided fixed-capacity state slot. A consumer never allocates
/// lane state and never retains a PlaybackProgram.
struct ProgramWireLaneState {
    ProgramWireLaneIdentity identity;
    AutomationCursor cursor;
    AutomationCursor next_cursor;
    std::array<AutomationBlockEvent, AutomationPlaybackLimits::kMaximumEventsPerDevicePerBlock>
        scratch{};
    AutomationCursorResult cursor_result;
    std::uint32_t event_count = 0;
    std::uint32_t merge_position = 0;
    std::uint32_t selected_count = 0;
    std::uint32_t write_position = 0;
    std::size_t output_index = 0;
    bool group_coalesced = false;
};

struct ProgramWireAutomationLaneOutput {
    timeline::ItemId track_id;
    timeline::ItemId lane_id;
    std::span<AutomationBlockEvent> events;
    AutomationCursorResult result;
};

struct ProgramWireAutomationRenderResult {
    ProgramWireConsumerCode code = ProgramWireConsumerCode::Ok;
    std::uint32_t rendered_lanes = 0;
};

/// Allocation-free automation consumer over one uniquely pinned wire payload.
/// Adoption validates the exact prepared tempo-map identity and commits only
/// after every lane and ceiling has passed. Rendering delegates to the same
/// AutomationCursor algorithm used by direct PlaybackProgram consumers.
class ProgramWireAutomationConsumer {
  public:
    static constexpr std::size_t kDefaultWireByteCapacity = 16u * 1'024u * 1'024u;

    explicit ProgramWireAutomationConsumer(std::span<ProgramWireLaneState> lane_state,
                                           std::size_t track_capacity,
                                           std::size_t wire_byte_capacity) noexcept
        : lane_state_(lane_state), track_capacity_(track_capacity),
          wire_byte_capacity_(wire_byte_capacity) {}
    explicit ProgramWireAutomationConsumer(std::span<ProgramWireLaneState> lane_state,
                                           std::size_t track_capacity) noexcept
        : ProgramWireAutomationConsumer(lane_state, track_capacity,
                                        kDefaultWireByteCapacity) {}
    explicit ProgramWireAutomationConsumer(std::span<ProgramWireLaneState> lane_state) noexcept
        : ProgramWireAutomationConsumer(lane_state, lane_state.size()) {}
    ProgramWireAutomationConsumer(const ProgramWireAutomationConsumer&) = delete;
    ProgramWireAutomationConsumer& operator=(const ProgramWireAutomationConsumer&) = delete;

    ProgramWireAdoptionResult adopt(ProgramWireBytePin candidate,
                                    const timebase::CompiledTempoMap& tempo_map,
                                    std::span<const timebase::TempoPoint> tempo_points) noexcept;
    ProgramWireAutomationRenderResult
    render(const TransportSnapshot& transport,
           std::span<ProgramWireAutomationLaneOutput> output) noexcept;

    std::span<const std::byte> active_bytes() const noexcept {
        return active_pin_.bytes();
    }
    std::size_t lane_count() const noexcept {
        return lane_count_;
    }

  private:
    std::span<ProgramWireLaneState> lane_state_;
    ProgramWireBytePin active_pin_;
    ProgramWireView active_view_;
    const timebase::CompiledTempoMap* tempo_map_ = nullptr;
    std::size_t lane_count_ = 0;
    std::size_t track_capacity_ = 0;
    std::size_t wire_byte_capacity_ = 0;
    std::array<ProgramWireLaneState*, AutomationPlaybackLimits::kMaximumLanesPerTrack>
        merge_heap_{};
};

} // namespace pulp::playback
