/*
 * pulp/native_components/native_core.h — Pulp Native Component Core ABI (v1)
 *
 * The language-neutral C contract a native DSP core (Rust first, also C / Zig /
 * generated FAUST·Cmajor) implements, and a C++ `pulp::format::Processor`
 * adapter owns. This header is the *canonical source of truth*: in-tree Rust
 * examples currently hand-mirror the POD structs they use, and any generated
 * Rust bindings must be generated from this header, never the reverse.
 *
 * STATUS: experimental, source-built. This is shaped *like* a binary ABI (POD
 * structs, leading size/version, opaque handles, status codes, no STL /
 * exceptions / unwind) so the future public freeze is a *relabel*, not a
 * rewrite — but it is NOT a frozen binary ABI yet. Rebuild your core against
 * the SDK you ship with. See docs/reference/native-components.md and
 * docs/reference/node-abi.md.
 *
 * This is the *Processor-level* FFI. It is deliberately independent of
 * `SignalGraph` (contract decision 6): no graph, node, or host-chaining types
 * appear here. The `pulp_node_v1` C ABI reuses these primitives but does not
 * share this header.
 *
 * ── Contract decisions encoded here (see the spec) ──
 *   1  ABI-shaped: every struct leads with `size` + `abi_version`.
 *   2  Host owns all process buffers; the core only BORROWS them for the call.
 *   3  State is an opaque, versioned byte span; validate-before-commit; never
 *      unwind on malformed input; empty span == defaults.
 *   4  Parameter identity is a stable string id + a stable 64-bit hash, with
 *      plain-domain values and explicit ramp + sample-offset semantics.
 *   5  Additive evolution only: `size`/`abi_version` checks + capability flags.
 *   6  Independent of SignalGraph (no host-graph assumptions).
 *   7  Explicit lifecycle states: ACTIVE vs SUSPENDED, plus reset. Alloc-heavy
 *      work happens only while SUSPENDED.
 *   8  Sample-rate / block-size renegotiation == re-`prepare()`.
 *   9  Multi-instance isolation: per-instance opaque handle; no process-wide
 *      mutable globals.
 *   10 Bus / sidechain layout negotiated as POD; the core may accept/reject.
 *   11 Parameter modulation and automation are DISTINCT, not collapsed.
 *   12 Allocator ownership is explicit: every owned pointer names its paired
 *      free function; the core's allocator never frees host memory or vice
 *      versa.
 */
#ifndef PULP_NATIVE_COMPONENTS_NATIVE_CORE_H
#define PULP_NATIVE_COMPONENTS_NATIVE_CORE_H

#include <stddef.h> /* size_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ───────────────────────── ABI version ───────────────────────── */

/* Bumped only for source-incompatible changes within draft 1. Additive field
 * growth is signalled by per-struct `size`, not by this number. */
#define PULP_NATIVE_CORE_ABI_VERSION 1u

/* ───────────────────────── Status codes ──────────────────────── */

typedef int32_t pulp_native_status;
enum {
    PULP_NATIVE_OK = 0,
    PULP_NATIVE_ERR_UNSUPPORTED = 1,     /* capability/layout not supported   */
    PULP_NATIVE_ERR_INVALID_ARGUMENT = 2,
    PULP_NATIVE_ERR_OUT_OF_MEMORY = 3,
    PULP_NATIVE_ERR_INVALID_STATE = 4,   /* e.g. process() before prepare()   */
    PULP_NATIVE_ERR_MALFORMED_STATE = 5, /* load_state given bad bytes        */
    PULP_NATIVE_ERR_VERSION_MISMATCH = 6,
    PULP_NATIVE_ERR_INTERNAL = 7
};

/* ───────────────────────── Capability flags ──────────────────────────
 * Decision 5/11: optional behavior is negotiated by additive bits, never by
 * widening a required struct. A host ANDs what it needs against what the core
 * reports in the descriptor. */
typedef uint32_t pulp_native_caps;
enum {
    PULP_NATIVE_CAP_MIDI_INPUT = 1u << 0,
    PULP_NATIVE_CAP_MIDI_OUTPUT = 1u << 1,
    PULP_NATIVE_CAP_MPE = 1u << 2,
    PULP_NATIVE_CAP_UMP = 1u << 3,
    PULP_NATIVE_CAP_SIDECHAIN = 1u << 4,
    PULP_NATIVE_CAP_IS_INSTRUMENT = 1u << 5,
    PULP_NATIVE_CAP_PARAM_MODULATION = 1u << 6, /* decision 11: supports mod   */
    PULP_NATIVE_CAP_STATE = 1u << 7,            /* implements save/load_state  */
    PULP_NATIVE_CAP_TAIL = 1u << 8,             /* reports a non-zero tail     */
    PULP_NATIVE_CAP_EDITOR_COMMAND = 1u << 9    /* non-RT editor_command path  */
};

/* ───────────────────────── Lifecycle (decision 7) ─────────────────────
 * The instance is created SUSPENDED. prepare()/release() and any allocation
 * happen only while SUSPENDED. resume() arms the RT path; suspend() disarms it.
 * process() is legal only while ACTIVE. */
typedef int32_t pulp_native_lifecycle;
enum {
    PULP_NATIVE_LIFECYCLE_SUSPENDED = 0,
    PULP_NATIVE_LIFECYCLE_ACTIVE = 1
};

/* ───────────────────────── Parameters (decision 4/11) ─────────────────
 * Identity is the stable string id AND its 64-bit hash. The hash is the
 * FNV-1a (64-bit) of the UTF-8 id bytes, computed case-sensitively over the
 * exact byte sequence (offset basis 0xcbf29ce484222325, prime
 * 0x100000001b3). The host computes the same hash; collisions are a hard error
 * the host rejects at registration. Values are PLAIN DOMAIN (engineering
 * units), never normalized. */
typedef uint32_t pulp_native_param_flags;
enum {
    PULP_NATIVE_PARAM_AUTOMATABLE = 1u << 0,
    PULP_NATIVE_PARAM_MODULATABLE = 1u << 1, /* decision 11: mod != automation */
    PULP_NATIVE_PARAM_STEPPED = 1u << 2,     /* integer steps over [min,max]   */
    PULP_NATIVE_PARAM_BOOLEAN = 1u << 3,
    PULP_NATIVE_PARAM_LOGARITHMIC = 1u << 4
};

typedef struct pulp_native_param_v1 {
    uint32_t size;
    uint32_t abi_version;
    /* Stable UTF-8 id. `id`+`id_len` is the byte span; NOT required to be NUL-
     * terminated. id==NULL with id_len==0 is invalid for a parameter (every
     * param must have an id). Lifetime: owned by the core, must stay valid
     * until release(). */
    const char* id;
    size_t id_len;
    uint64_t id_hash; /* FNV-1a/64 of the id bytes; see above. */
    /* Optional display name (UTF-8 span). name==NULL,name_len==0 == "use id". */
    const char* name;
    size_t name_len;
    double min_value;
    double max_value;
    double default_value;
    pulp_native_param_flags flags;
    uint32_t step_count; /* meaningful only with PULP_NATIVE_PARAM_STEPPED */
} pulp_native_param_v1;

/* Decision 11: an automation point and a modulation offset are different
 * things and must not be collapsed. Both are sample-accurate. */
typedef int32_t pulp_native_param_event_kind;
enum {
    PULP_NATIVE_EVENT_AUTOMATION = 0, /* sets the base value (host automation) */
    PULP_NATIVE_EVENT_MODULATION = 1  /* signed offset added to the base value */
};

/* One sample-accurate parameter event. Mirrors Pulp's ParameterEventQueue:
 * plain-domain `value`, `sample_offset` within the block, `ramp_frames` linear
 * ramp duration (0 == immediate). Events in a view are SORTED ascending by
 * sample_offset (decision 4). */
typedef struct pulp_native_param_event_v1 {
    uint64_t param_id_hash;
    double value;             /* automation: target base; modulation: offset    */
    uint32_t sample_offset;   /* [0, frame_count)                               */
    uint32_t ramp_frames;     /* linear ramp length in frames; 0 == step        */
    pulp_native_param_event_kind kind;
    uint32_t reserved;        /* keep 8-byte alignment; must be 0               */
} pulp_native_param_event_v1;

/* Borrowed, read-only view over the host's sorted event queue for this block.
 * Decision 2: host-owned, valid only for the duration of the process() call.
 * `events`==NULL is semantically distinct from count==0-with-non-NULL: NULL
 * means "host supplied no queue this block" (== empty + no capacity info),
 * while a non-NULL pointer with count==0 means "empty queue, capacity known".
 * Pulp's queue is fixed-capacity (1024); `overflowed` is set when the host
 * dropped events because the queue was full. */
typedef struct pulp_native_param_event_view_v1 {
    uint32_t size;
    uint32_t abi_version;
    const pulp_native_param_event_v1* events; /* sorted by sample_offset, or NULL */
    uint32_t count;
    uint32_t capacity;   /* host queue capacity (1024 today), 0 if events==NULL */
    uint32_t overflowed; /* 1 if events were dropped this block, else 0         */
    uint32_t reserved;
} pulp_native_param_event_view_v1;

/* ───────────────────────── Audio buffers (decision 2) ─────────────────
 * Host-owned, non-interleaved float32 planar channels, borrowed for the call.
 * `channels[c]` points to `frame_count` contiguous floats. A channel pointer
 * is never NULL when channel_count > 0. In-place processing (an output bus
 * aliasing an input bus) is LEGAL; the core must tolerate it. Sidechain buses
 * are READ-ONLY. The core must not retain any channel pointer past the call
 * and must not free them (decision 12). */
typedef struct pulp_native_audio_bus_v1 {
    uint32_t size;
    uint32_t channel_count;
    float* const* channels; /* channel_count planar pointers; const for inputs  */
} pulp_native_audio_bus_v1;

typedef struct pulp_native_audio_io_v1 {
    uint32_t size;
    uint32_t abi_version;
    uint32_t frame_count; /* samples this block; <= prepared max_block_size      */
    uint32_t input_bus_count;
    uint32_t output_bus_count;
    uint32_t sidechain_bus_count;
    uint32_t reserved;
    const pulp_native_audio_bus_v1* inputs;     /* read-only                     */
    pulp_native_audio_bus_v1* outputs;          /* written by the core           */
    const pulp_native_audio_bus_v1* sidechains; /* read-only, may be NULL        */
} pulp_native_audio_io_v1;

/* ───────────────────────── MIDI / events ──────────────────────────────
 * Borrowed POD event stream. `data`+`size_bytes` is one raw MIDI message
 * (short message: 1–3 bytes; sysex: full F0..F7 span). For UMP-capable cores,
 * `is_ump` marks 32-bit-word UMP packets in `data`. The output buffer is
 * host-owned and bounded; push via the host services, never by retaining
 * `data`. */
typedef struct pulp_native_midi_event_v1 {
    uint32_t sample_offset;
    uint32_t size_bytes;
    const uint8_t* data; /* borrowed; valid only for the call */
    uint32_t is_ump;     /* 1 == UMP words, 0 == bytestream MIDI 1.0 */
    uint32_t reserved;
} pulp_native_midi_event_v1;

typedef struct pulp_native_midi_view_v1 {
    uint32_t size;
    uint32_t abi_version;
    const pulp_native_midi_event_v1* in_events; /* sorted by sample_offset, or NULL */
    uint32_t in_count;
    uint32_t reserved;
    /* Opaque host sink for output MIDI; pass to host_services.push_midi_out.
     * NULL when the core declared no MIDI output. */
    void* out_sink;
} pulp_native_midi_view_v1;

/* ───────────────────────── State span (decision 3/12) ─────────────────
 * Opaque, versioned bytes. On save the core allocates with ITS allocator and
 * the host copies then asks the core to free via free_state (decision 12). On
 * load the span is host-owned and read-only; the core validates before
 * committing and returns PULP_NATIVE_ERR_MALFORMED_STATE (never unwinds) on bad
 * input. `bytes`==NULL,`byte_len`==0 means "restore defaults". */
typedef struct pulp_native_state_span_v1 {
    uint32_t size;
    uint32_t abi_version;
    uint32_t state_version; /* core-defined state format version */
    uint32_t reserved;
    const uint8_t* bytes;
    size_t byte_len;
} pulp_native_state_span_v1;

/* Mutable variant the core fills in save_state. The host owns the lifetime
 * contract: it must call free_state with the same (bytes,byte_len). */
typedef struct pulp_native_state_out_v1 {
    uint32_t size;
    uint32_t abi_version;
    uint32_t state_version;
    uint32_t reserved;
    uint8_t* bytes;  /* allocated by the core; freed by core's free_state */
    size_t byte_len;
} pulp_native_state_out_v1;

/* ───────────────────────── Bus layout (decision 10) ───────────────────
 * Proposed channel layout the host offers in prepare()/set_bus_layout(); the
 * core returns PULP_NATIVE_OK to accept or PULP_NATIVE_ERR_UNSUPPORTED to
 * reject (the host then offers another). No stereo-in/stereo-out assumption. */
typedef struct pulp_native_bus_layout_v1 {
    uint32_t size;
    uint32_t abi_version;
    const uint32_t* input_channel_counts;  /* one per input bus  */
    uint32_t input_bus_count;
    const uint32_t* output_channel_counts; /* one per output bus */
    uint32_t output_bus_count;
    uint32_t sidechain_channel_count; /* 0 == no sidechain offered */
    uint32_t reserved;
} pulp_native_bus_layout_v1;

/* ───────────────────────── prepare() config (decision 8) ──────────────
 * Any change to sample_rate or max_block_size is a NEW prepare() call; the
 * core must re-allocate scratch and must not assume a fixed configuration
 * across prepare() calls. */
typedef struct pulp_native_prepare_v1 {
    uint32_t size;
    uint32_t abi_version;
    double sample_rate;
    uint32_t max_block_size;
    uint32_t reserved;
    pulp_native_bus_layout_v1 layout;
} pulp_native_prepare_v1;

/* ───────────────────────── Process context ────────────────────────────
 * Read-only transport snapshot for the block. RT-thread; non-allocating. */
typedef struct pulp_native_process_context_v1 {
    uint32_t size;
    uint32_t abi_version;
    double sample_rate;
    double tempo_bpm;
    double ppq_position;       /* musical position at block start */
    int64_t playhead_frames;   /* sample position at block start  */
    uint32_t is_playing;
    uint32_t is_looping;
    uint32_t time_sig_numerator;
    uint32_t time_sig_denominator;
} pulp_native_process_context_v1;

/* Everything one process() call needs, bundled so the call signature is stable
 * and growable by `size`. All pointers are host-owned and borrowed. */
typedef struct pulp_native_process_v1 {
    uint32_t size;
    uint32_t abi_version;
    const pulp_native_audio_io_v1* audio;
    const pulp_native_param_event_view_v1* params;
    const pulp_native_midi_view_v1* midi;       /* may be NULL */
    const pulp_native_process_context_v1* context;
} pulp_native_process_v1;

/* ───────────────────────── Descriptor (decision 1/5/6) ────────────────
 * Static identity + capabilities. Strings are UTF-8 spans owned by the core
 * for its lifetime. */
typedef struct pulp_native_descriptor_v1 {
    uint32_t size;
    uint32_t abi_version;
    const char* id; /* reverse-DNS-ish stable plugin id */
    size_t id_len;
    const char* name;
    size_t name_len;
    const char* vendor;
    size_t vendor_len;
    uint32_t plugin_version; /* core-defined */
    pulp_native_caps capabilities;
    uint32_t default_input_bus_count;
    uint32_t default_output_bus_count;
    uint32_t latency_frames; /* initial; may change, see report_latency */
    uint32_t tail_frames;    /* 0 if no tail / unknown */
} pulp_native_descriptor_v1;

/* ───────────────────────── Host services (decision 12) ────────────────
 * Callbacks the host provides to the core. Each is explicitly labeled
 * RT-callable or NON-RT-ONLY. Calling a NON-RT-ONLY callback from process() is
 * a contract violation the RT-safety hook traps. `alloc`/`free` here are the
 * HOST allocator — the core must NOT pass host-allocated pointers to its own
 * free, nor vice versa (decision 12). */
typedef struct pulp_native_host_services_v1 {
    uint32_t size;
    uint32_t abi_version;
    void* host_context; /* opaque, passed back to each callback */

    /* NON-RT-ONLY. Host allocator for editor/state side-channels. */
    void* (*alloc)(void* host_context, size_t bytes);
    void (*free)(void* host_context, void* ptr);
    /* NON-RT-ONLY. Diagnostic log. Never call from process(). */
    void (*log)(void* host_context, int32_t level, const char* utf8, size_t len);

    /* RT-callable. Push one output MIDI event into the host sink obtained from
     * pulp_native_midi_view_v1.out_sink. Bounded; returns 0 if dropped. */
    int32_t (*push_midi_out)(void* out_sink,
                             const pulp_native_midi_event_v1* event);
    /* RT-callable. Tell the host latency changed (host re-queries report_*). */
    void (*notify_latency_changed)(void* host_context);
} pulp_native_host_services_v1;

/* ───────────────────────── The core vtable (decision 1/7/9) ───────────
 * A single exported entry returns a const pointer to this table plus a static
 * descriptor. Per-instance state lives behind the opaque handle from create()
 * (decision 9: no process-wide mutable globals). All function pointers are
 * stable; growth is additive via `size`. */
typedef struct pulp_native_instance pulp_native_instance; /* opaque */

typedef struct pulp_native_core_v1 {
    uint32_t size;
    uint32_t abi_version;

    /* Static, instance-independent. */
    const pulp_native_descriptor_v1* (*descriptor)(void);
    /* Returns the parameter table; *out_count set to the entry count. The
     * array is owned by the core and stable for the process lifetime. */
    const pulp_native_param_v1* (*parameters)(uint32_t* out_count);

    /* Lifecycle. create() returns a SUSPENDED instance. NON-RT. */
    pulp_native_status (*create)(const pulp_native_host_services_v1* host,
                                 pulp_native_instance** out_instance);
    void (*destroy)(pulp_native_instance* instance);

    /* NON-RT, SUSPENDED only. Allocate scratch for this configuration. A new
     * sample_rate/max_block_size requires a fresh prepare() (decision 8). */
    pulp_native_status (*prepare)(pulp_native_instance* instance,
                                  const pulp_native_prepare_v1* config);
    /* NON-RT, SUSPENDED only. Release scratch. */
    void (*release)(pulp_native_instance* instance);

    /* NON-RT. Offer a bus layout; accept (OK) or reject (ERR_UNSUPPORTED). */
    pulp_native_status (*set_bus_layout)(pulp_native_instance* instance,
                                         const pulp_native_bus_layout_v1* layout);

    /* NON-RT. SUSPENDED <-> ACTIVE transitions (decision 7). */
    pulp_native_status (*resume)(pulp_native_instance* instance);
    pulp_native_status (*suspend)(pulp_native_instance* instance);
    /* RT-callable. Clear runtime history (filters, delay lines) without
     * reallocating. Legal while ACTIVE. */
    void (*reset)(pulp_native_instance* instance);

    /* RT-callable, ACTIVE only. The real-time audio callback. Must not
     * allocate, lock, log, do I/O, or panic. Host owns all buffers. On
     * internal failure return a status and leave outputs silent. */
    pulp_native_status (*process)(pulp_native_instance* instance,
                                  const pulp_native_process_v1* io);

    /* NON-RT. Opaque versioned state (decision 3/12). save_state allocates
     * with the core allocator; the host copies then calls free_state. */
    pulp_native_status (*save_state)(pulp_native_instance* instance,
                                     pulp_native_state_out_v1* out_state);
    void (*free_state)(pulp_native_instance* instance,
                       pulp_native_state_out_v1* out_state);
    pulp_native_status (*load_state)(pulp_native_instance* instance,
                                     const pulp_native_state_span_v1* state);

    /* NON-RT. Current latency / tail in frames (decision: latency/tail query). */
    uint32_t (*report_latency)(pulp_native_instance* instance);
    uint32_t (*report_tail)(pulp_native_instance* instance);

    /* NON-RT, optional (PULP_NATIVE_CAP_EDITOR_COMMAND). JSON-in / JSON-out
     * command channel for editor/domain logic, off the audio thread. The core
     * allocates the reply with its allocator; host frees via free_state-style
     * pairing using free_editor_reply. */
    pulp_native_status (*editor_command)(pulp_native_instance* instance,
                                         const uint8_t* request_json,
                                         size_t request_len,
                                         uint8_t** out_reply_json,
                                         size_t* out_reply_len);
    void (*free_editor_reply)(pulp_native_instance* instance,
                              uint8_t* reply_json,
                              size_t reply_len);
} pulp_native_core_v1;

/* ───────────────────────── Entry point ────────────────────────────────
 * The ONE symbol the host resolves. A staticlib core exports this; the C++
 * adapter calls it to obtain the vtable, checks `abi_version`/`size`, then
 * drives the lifecycle. Returning NULL means "incompatible / unavailable".
 * Referencing this symbol directly lets the C++ side link the staticlib
 * without `--whole-archive`. */
const pulp_native_core_v1* pulp_native_core_entry_v1(void);

/* The RT-safety hook (test builds) calls this from a checking allocator /
 * operator new when an allocation is attempted inside a no-alloc scope. The
 * default (production) implementation is a no-op; test harnesses override it to
 * trap. `kind` is a small tag (0=c++ new, 1=malloc, 2=rust-global-alloc, ...),
 * `bytes` the requested size. It must itself never allocate. */
void pulp_rt_trap_if_no_alloc_scope(int32_t kind, size_t bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PULP_NATIVE_COMPONENTS_NATIVE_CORE_H */
