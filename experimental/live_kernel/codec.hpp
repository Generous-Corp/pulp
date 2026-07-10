#pragma once
//
// Pulp Live Kernel — S0 spike — binary graph codec (IR v0, "LKB0").
//
// A bounded, self-describing binary blob describing a DSP graph: a node table,
// an edge table, and a param table. It is DELIBERATELY not JSON — decode runs on
// the audio-render thread (worklet port.onmessage), so the format is a flat,
// fixed-stride, bounds-checked binary that decodes with zero allocation into a
// preallocated PlanDesc (see HARD RT CONTRACT in the build plan §4 / R1).
//
// Layout (little-endian):
//   0   char[4]  magic = 'L','K','B','0'
//   4   u8       version (0)
//   5   u8       num_nodes   (<= LK_MAX_NODES)
//   6   u8       num_edges   (<= LK_MAX_EDGES)
//   7   u8       num_params  (<= LK_MAX_PARAMS)
//   8   u16      output_node (index into the node table)
//   10  u16      reserved (0)
//   12  node[num_nodes]      : {u8 type; u8 num_in; u8 num_out; u8 flags}   (4B)
//   ..  edge[num_edges]      : {u16 src; u8 sport; u8 pad; u16 dst; u8 dport; u8 fb} (8B)
//   ..  param[num_params]    : {u16 node; u16 param_id; f32 value}          (8B)
//
// Every multi-byte field is read byte-wise so the decoder is alignment- and
// endianness-explicit and never over-reads (each read is bounds-checked against
// len before it happens).

#include <cstdint>
#include <cstddef>

namespace pulp::live_kernel {

// Bounds (the "explicit max nodes/edges/params/bytes" of the RT contract).
inline constexpr int LK_MAX_NODES  = 64;
inline constexpr int LK_MAX_EDGES  = 128;
inline constexpr int LK_MAX_PARAMS = 255;
inline constexpr int LK_MAX_BYTES  = 4096;
inline constexpr int LK_MAX_IN     = 2;   // max input ports per node
inline constexpr int LK_MAX_OUT    = 1;   // max output ports per node (all six have 1)
inline constexpr int LK_MAX_BLOCK  = 128; // one AudioWorklet quantum

// Node type ids (stable wire values).
enum class NodeType : uint8_t {
    Oscillator = 0,  // 0 in, 1 out. params: 0=freq_hz 1=waveform 2=amp
    Gain       = 1,  // 1 in, 1 out. params: 0=gain_db
    Biquad     = 2,  // 1 in, 1 out. params: 0=type 1=cutoff_hz 2=q 3=gain_db
    Ladder     = 3,  // 1 in, 1 out. params: 0=cutoff_hz 1=resonance
    Adsr       = 4,  // 1 in, 1 out (VCA). params: 0=a 1=d 2=s 3=r(seconds) 4=gate
    Delay      = 5,  // 1 in, 1 out. params: 0=time_s 1=feedback 2=mix
    Mixer      = 6,  // 2 in, 1 out. params: 0=mix (dry/wet)
    // ── iteration 2: node-breadth expansion (all mono, alloc-safe) ──
    Svf        = 7,  // 1 in, 1 out. params: 0=mode 1=cutoff_hz 2=res  (TPT state-variable, mod-stable)
    Shaper     = 8,  // 1 in, 1 out. params: 0=curve 1=drive          (WaveShaperT distortion)
    DcBlock    = 9,  // 1 in, 1 out. params: 0=pole                    (one-pole DC blocker)
    Noise      = 10, // 0 in, 1 out. params: 0=amp 1=color(0=white,1=pink)
    Chorus     = 11, // 1 in, 1 out. params: 0=rate_hz 1=depth 2=mix 3=delay_ms (pool-backed)
    Reverb     = 12, // 1 in, 1 out. params: 0=decay_s 1=damp 2=mix   (FDN reverb, pool-backed)
    Comp       = 13, // 1 in, 1 out. params: 0=thresh_db 1=ratio 2=attack_ms 3=release_ms (pool-backed)
    Count      = 14,
};

enum class DecodeError : int32_t {
    Ok             = 0,
    TooSmall       = -1,
    BadMagic       = -2,
    BadVersion     = -3,
    TooManyNodes   = -4,
    TooManyEdges   = -5,
    TooManyParams  = -6,
    TooManyBytes   = -7,
    Truncated      = -8,
    BadNodeType    = -9,
    BadNodeIndex   = -10,
    BadOutputNode  = -11,
};

struct NodeDesc { NodeType type; uint8_t num_in; uint8_t num_out; uint8_t flags; };
struct EdgeDesc { uint16_t src; uint8_t sport; uint16_t dst; uint8_t dport; uint8_t feedback; };
struct ParamDesc { uint16_t node; uint16_t param_id; float value; };

// Fully decoded, bounded plan description. All storage is inline (no heap), so a
// PlanDesc can live in preallocated kernel storage and be filled with zero
// allocation on the audio thread.
struct PlanDesc {
    int      num_nodes  = 0;
    int      num_edges  = 0;
    int      num_params = 0;
    int      output_node = 0;
    NodeDesc nodes[LK_MAX_NODES];
    EdgeDesc edges[LK_MAX_EDGES];
    ParamDesc params[LK_MAX_PARAMS];
};

namespace detail {
inline uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline float rd_f32(const uint8_t* p) {
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}
} // namespace detail

// Decode `len` bytes at `data` into `out`. Returns DecodeError::Ok on success.
// Never reads past `len`; never allocates.
inline DecodeError decode_plan(const uint8_t* data, int len, PlanDesc& out) {
    using namespace detail;
    if (len < 12) return DecodeError::TooSmall;
    if (len > LK_MAX_BYTES) return DecodeError::TooManyBytes;
    if (!(data[0] == 'L' && data[1] == 'K' && data[2] == 'B' && data[3] == '0'))
        return DecodeError::BadMagic;
    if (data[4] != 0) return DecodeError::BadVersion;

    const int num_nodes  = data[5];
    const int num_edges  = data[6];
    const int num_params = data[7];
    const int output_node = rd_u16(data + 8);

    if (num_nodes  > LK_MAX_NODES)  return DecodeError::TooManyNodes;
    if (num_edges  > LK_MAX_EDGES)  return DecodeError::TooManyEdges;
    if (num_params > LK_MAX_PARAMS) return DecodeError::TooManyParams;

    const long need = 12L + (long)num_nodes * 4 + (long)num_edges * 8 +
                      (long)num_params * 8;
    if (len < need) return DecodeError::Truncated;
    if (num_nodes == 0) return DecodeError::TooSmall;
    if (output_node < 0 || output_node >= num_nodes) return DecodeError::BadOutputNode;

    out.num_nodes = num_nodes;
    out.num_edges = num_edges;
    out.num_params = num_params;
    out.output_node = output_node;

    int off = 12;
    for (int i = 0; i < num_nodes; ++i, off += 4) {
        uint8_t t = data[off];
        if (t >= (uint8_t)NodeType::Count) return DecodeError::BadNodeType;
        out.nodes[i].type    = (NodeType)t;
        out.nodes[i].num_in  = data[off + 1];
        out.nodes[i].num_out = data[off + 2];
        out.nodes[i].flags   = data[off + 3];
    }
    for (int i = 0; i < num_edges; ++i, off += 8) {
        EdgeDesc& e = out.edges[i];
        e.src      = rd_u16(data + off);
        e.sport    = data[off + 2];
        e.dst      = rd_u16(data + off + 4);
        e.dport    = data[off + 6];
        e.feedback = data[off + 7];
        if (e.src >= num_nodes || e.dst >= num_nodes) return DecodeError::BadNodeIndex;
    }
    for (int i = 0; i < num_params; ++i, off += 8) {
        ParamDesc& p = out.params[i];
        p.node     = rd_u16(data + off);
        p.param_id = rd_u16(data + off + 2);
        p.value    = rd_f32(data + off + 4);
        if (p.node >= num_nodes) return DecodeError::BadNodeIndex;
    }
    return DecodeError::Ok;
}

} // namespace pulp::live_kernel
