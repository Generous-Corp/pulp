#include "spectral_graph_persistence.hpp"
#include "graph_load_refusal.hpp"
#include "spectral_asset_morph_internal.hpp"
#include "spectral_asset_source_internal.hpp"
#include "spectral_finite_stream_internal.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace pulp::host::spectral_persistence {
namespace {
using Value = choc::value::Value;
using View = choc::value::ValueView;
constexpr std::size_t budget = 64u * 1024u * 1024u;
constexpr const char* asset_schema = "pulp.analyzed-spectral-asset";
constexpr const char* saved_identity_v7 = "pulp.graph.saved-identity.v7";
using Policy = SpectralFiniteStreamRule::Policy;
// The asset morph's control clock needs a hop that divides the FFT; the
// single-stream source admits every hop the analysis grid does.
constexpr Policy morph_policy = Policy::dividing_hop_v1;
constexpr Policy source_policy = Policy::analysis_grid_v2;
static_assert(source_policy == SpectralFiniteStreamRule::current_policy,
              "the asset source factory admits only the current finite-stream policy");
std::atomic<bool> fail_next_candidate{false};

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result.push_back(alphabet[b >> 4]);
        result.push_back(alphabet[b & 15]);
    }
    return result;
}
std::string digest(std::span<const std::uint8_t> bytes) {
    const auto hash = runtime::sha256(bytes.data(), bytes.size());
    return hex(hash);
}
std::string asset_id(std::uint64_t length, const std::string& contract,
                     const std::string& bytes_hash) {
    // Domain separation binds the analyzed-asset schema, original extent and
    // frame contract, which canonical SFRM bytes intentionally do not carry.
    const auto material = std::string(asset_schema) + ".v1\n" + std::to_string(length) + "\n" +
                          contract + "\n" + bytes_hash;
    return digest({reinterpret_cast<const std::uint8_t*>(material.data()), material.size()});
}
bool text(View v, std::string& out) {
    if (!v.isString())
        return false;
    out = std::string(v.getString());
    return true;
}
bool uint(View v, std::uint64_t& out, std::uint64_t max = INT64_MAX) {
    if (!v.isInt() || v.getInt64() < 0 || std::uint64_t(v.getInt64()) > max)
        return false;
    out = static_cast<std::uint64_t>(v.getInt64());
    return true;
}
bool real(View v, double& out) {
    if (v.isInt())
        out = static_cast<double>(v.getInt64());
    else if (v.isFloat32() || v.isFloat64())
        out = v.getFloat64();
    else
        return false;
    return std::isfinite(out);
}
bool fields(View value, std::initializer_list<std::string_view> allowed) {
    if (!value.isObject())
        return false;
    for (std::uint32_t i = 0; i < value.size(); ++i)
        if (std::find(allowed.begin(), allowed.end(), value.getObjectMemberAt(i).name) ==
            allowed.end())
            return false;
    return true;
}
std::optional<SpectralIdentity> identity(View v) {
    std::string encoded;
    if (!text(v, encoded) || encoded.size() != 64)
        return {};
    std::array<std::uint8_t, 32> bytes{};
    auto nibble = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto a = nibble(encoded[i * 2]), b = nibble(encoded[i * 2 + 1]);
        if (a < 0 || b < 0)
            return {};
        bytes[i] = static_cast<std::uint8_t>((a << 4) | b);
    }
    const auto contract = graph::SpectralFrameContract::decode(bytes);
    if (!contract)
        return {};
    const auto& c = *contract.value;
    const auto id = SpectralIdentity::make(c.fft_size, c.hop, c.channels,
                                           std::bit_cast<double>(c.sample_rate_bits));
    return id.value;
}
std::string hex64(std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * (7 - i)));
    return hex(bytes);
}
// The readable form of a node's frame contract that a v7 document records next
// to contract_hex: geometry and the sample rate's exact IEEE-754 bits, written
// as 16 lowercase hex digits so no decimal rounding can touch them.
Value identity_record(const SpectralIdentity& shape) {
    auto record = choc::value::createObject("SpectralIdentity");
    record.addMember("fft_size", int64_t(shape.fft_size()));
    record.addMember("hop", int64_t(shape.hop()));
    record.addMember("channels", int64_t(shape.channels()));
    record.addMember("sample_rate_bits", hex64(shape.sample_rate_bits()));
    return record;
}
bool identity_matches(View v, const SpectralIdentity& shape) {
    std::uint64_t n = 0, h = 0, c = 0;
    std::string bits;
    return fields(v, {"fft_size", "hop", "channels", "sample_rate_bits"}) &&
           uint(v["fft_size"], n) && uint(v["hop"], h) && uint(v["channels"], c) &&
           text(v["sample_rate_bits"], bits) && n == std::uint64_t(shape.fft_size()) &&
           h == std::uint64_t(shape.hop()) && c == std::uint64_t(shape.channels()) &&
           bits == hex64(shape.sample_rate_bits());
}
// The saved frame contract and original extent determine the finite stream
// under `policy`: the same rule the factories and runtime use, so a saved
// asset carries no geometry the runtime would derive differently. A format 6
// document derives its asset frame counts with dividing_hop_v1; a v7 document
// with analysis_grid_v2, which agrees wherever dividing_hop_v1 admits the
// geometry.
SpectralFiniteStreamRule stream_rule(const SpectralIdentity& shape, std::uint64_t length,
                                     Policy policy) {
    return SpectralFiniteStreamRule::make(
        static_cast<std::uint64_t>(shape.fft_size()), static_cast<std::uint64_t>(shape.hop()),
        static_cast<std::uint64_t>(shape.channels()), length, policy);
}
const char* type_name(NodeType type) {
    switch (type) {
    case NodeType::AudioInput:
        return "audio_in";
    case NodeType::AudioOutput:
        return "audio_out";
    case NodeType::SpectralAssetMorph:
        return "spectral_asset_morph";
    case NodeType::SpectralAssetSource:
        return "spectral_asset_source";
    case NodeType::SpectralAnalysis:
        return "spectral_analysis";
    case NodeType::SpectralStage:
        return "spectral_stage";
    case NodeType::SpectralSynthesis:
        return "spectral_synthesis";
    default:
        return nullptr;
    }
}
// The authored ID alphabet the saved-identity v7 reader admits.
bool valid_authored_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '~' || c == '-';
    });
}
bool charge(std::size_t& used, std::size_t count) {
    if (count > budget - used)
        return false;
    used += count;
    return true;
}
struct Asset {
    std::vector<std::uint8_t> bytes;
    std::uint64_t length = 0;
    std::optional<SpectralIdentity> shape;
    bool used = false;
};
} // namespace

bool contains_spectral_nodes(const SignalGraph& graph) {
    return std::any_of(graph.nodes().begin(), graph.nodes().end(),
                       [](const auto& n) { return bool(n.spectral_identity); });
}

int admission_block_for(const SignalGraph& graph) {
    const int prepared = graph.prepared_max_block_size();
    const int block = prepared > 0 ? prepared : graph.limits().max_block_size;
    return std::clamp(block, 1, max_admission_block);
}

std::string write(const SignalGraph& graph,
                  const std::unordered_map<NodeId, std::pair<float, float>>& layout,
                  std::string& reason, bool saved_identity) {
    reason.clear();
    const auto refuse = [&](std::string why) {
        reason = std::move(why);
        return std::string{};
    };
    try {
        if (!graph.sample_regions().empty())
            return refuse("a spectral graph cannot carry sample regions");
        if (graph.nodes().size() > 128)
            return refuse("a spectral graph holds at most 128 nodes");
        if (graph.connections().size() > 512)
            return refuse("a spectral graph holds at most 512 connections");
        // A v7 asset row derives its frame count with the grid rule every
        // finite source shares; a format 6 row keeps the dividing-hop rule its
        // reader applies.
        const auto row_policy = saved_identity ? source_policy : morph_policy;
        auto root = choc::value::createObject("PulpGraph");
        root.addMember("format_version", int64_t(saved_identity ? 7 : 6));
        auto nodes = choc::value::createEmptyArray();
        std::map<std::string, Value> assets;
        std::size_t encoded_charge = 0, retained_charge = 0;
        auto add_asset = [&](const signal::OwnedSpectralStream& stream, std::uint64_t length,
                             const std::array<std::uint8_t, 32>& expected_hash,
                             const SpectralIdentity& shape,
                             const std::string& contract) -> std::string {
            const auto rule = stream_rule(shape, length, row_policy);
            if (!rule || stream.frame_count != rule.frame_count)
                return refuse("a spectral asset's frame count does not match its stream contract");
            const auto encoded = signal::encode_spectral_stream(stream, budget);
            if (!encoded || digest(encoded.value->view()) != hex(expected_hash))
                return refuse("a spectral asset cannot be encoded or does not match its hash");
            const auto hash = hex(expected_hash);
            const auto key = asset_id(length, contract, hash);
            if (assets.contains(key))
                return key;
            // Include base64 expansion before allocating its encoded string.
            if (encoded.value->size > budget ||
                !charge(encoded_charge, ((encoded.value->size + 2) / 3) * 4))
                return refuse("spectral assets exceed the encoded byte budget");
            auto record = choc::value::createObject("SpectralAsset");
            record.addMember("id", key);
            record.addMember("schema", asset_schema);
            record.addMember("version", int64_t(1));
            record.addMember("source_samples", static_cast<int64_t>(length));
            record.addMember("frame_count", static_cast<int64_t>(rule.frame_count));
            record.addMember("contract_hex", contract);
            record.addMember("bytes_sha256", hash);
            record.addMember("bytes_b64", runtime::base64_encode(encoded.value->bytes.get(),
                                                                 encoded.value->size));
            assets.emplace(key, std::move(record));
            return key;
        };
        std::vector<const GraphNode*> ordered;
        for (const auto& n : graph.nodes())
            ordered.push_back(&n);
        std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->id < b->id; });
        for (const auto* ptr : ordered) {
            const auto& n = *ptr;
            const auto* type = type_name(n.type);
            if (graph.is_processor_node(n.id))
                return refuse("node " + n.name + " is a Processor node");
            if (!saved_identity && n.type == NodeType::SpectralAssetSource)
                return refuse("node " + n.name +
                              " is a single-stream spectral asset source; the format 6 document "
                              "cannot carry it, save the saved-identity v7 document "
                              "(to_saved_document)");
            if (!saved_identity && n.type == NodeType::SpectralAnalysis)
                return refuse("node " + n.name +
                              " is a SpectralAnalysis node; the format 6 document cannot carry "
                              "its declaration, save the saved-identity v7 document "
                              "(to_saved_document)");
            if (!type)
                return refuse("node " + n.name +
                              " is not an audio input, audio output or spectral node");
            if (n.gain != 1)
                return refuse("node " + n.name + " has a gain other than 1");
            auto node = choc::value::createObject("Node");
            node.addMember("id", int64_t(n.id));
            if (saved_identity) {
                if (!valid_authored_id(n.authored_id.str()))
                    return refuse("node " + n.name + " has no valid authored ID");
                node.addMember("authored_id", n.authored_id.str());
            }
            node.addMember("type", type);
            node.addMember("name", n.name);
            node.addMember("num_input_ports", int64_t(n.num_input_ports));
            node.addMember("num_output_ports", int64_t(n.num_output_ports));
            node.addMember("gain", 1.0);
            if (n.spectral_identity) {
                const auto& shape = *n.spectral_identity;
                const auto contract = hex(shape.frame_contract().encode());
                auto spectral = choc::value::createObject("SpectralNode");
                spectral.addMember("contract_hex", contract);
                if (saved_identity)
                    spectral.addMember("identity", identity_record(shape));
                if (n.type == NodeType::SpectralAssetMorph) {
                    const auto& config = n.spectral_asset_morph;
                    if (!config)
                        return refuse("spectral asset morph node " + n.name + " has no config");
                    if (!charge(retained_charge, config->retained_bytes()))
                        return refuse("spectral assets exceed the retained byte budget");
                    const auto a = add_asset(config->a, config->source_samples, config->hash_a,
                                             shape, contract);
                    const auto b = add_asset(config->b, config->source_samples, config->hash_b,
                                             shape, contract);
                    if (a.empty() || b.empty())
                        return std::string{};
                    spectral.addMember("asset_a", a);
                    spectral.addMember("asset_b", b);
                    if (saved_identity)
                        spectral.addMember("finite_policy",
                                           int64_t(static_cast<std::uint32_t>(morph_policy)));
                } else if (n.type == NodeType::SpectralAssetSource) {
                    const auto& config = n.spectral_asset_source;
                    if (!config)
                        return refuse("spectral asset source node " + n.name + " has no config");
                    if (config->rule.policy != source_policy)
                        return refuse("spectral asset source node " + n.name +
                                      " uses finite-source policy version " +
                                      std::to_string(config->rule.policy_version()) +
                                      ", which this writer does not record");
                    if (!charge(retained_charge, config->retained_bytes()))
                        return refuse("spectral assets exceed the retained byte budget");
                    const auto key = add_asset(config->stream, config->rule.source_samples,
                                               config->hash, shape, contract);
                    if (key.empty())
                        return std::string{};
                    spectral.addMember("asset", key);
                    spectral.addMember("finite_policy", int64_t(config->rule.policy_version()));
                } else if (n.type == NodeType::SpectralStage) {
                    if (!n.spectral_stage || n.spectral_stage->identity() != shape)
                        return refuse("spectral stage node " + n.name +
                                      " has no stage or a stage of another frame contract");
                    if (!charge(retained_charge, n.spectral_stage->retained_config_bytes()))
                        return refuse("spectral stages exceed the retained byte budget");
                    const auto& stage = *n.spectral_stage;
                    spectral.addMember("stage",
                                       stage.kind() == QualifiedSpectralStage::Kind::IdentityV1
                                           ? "identity_v1"
                                           : "real_mask_v1");
                    auto gains = choc::value::createEmptyArray();
                    for (auto gain : stage.gains())
                        gains.addArrayElement(double(gain));
                    spectral.addMember("gains", gains);
                }
                // An analysis node saves its declaration (the identity above)
                // only: never captured input audio or live frame history.
                node.addMember("spectral", spectral);
            } else if (n.type != NodeType::AudioInput && n.type != NodeType::AudioOutput)
                return refuse("spectral node " + n.name + " has no spectral identity");
            if (const auto it = layout.find(n.id); it != layout.end()) {
                if (!std::isfinite(it->second.first) || !std::isfinite(it->second.second))
                    return refuse("node " + n.name + " has a non-finite editor position");
                auto position = choc::value::createObject("Pos");
                position.addMember("x", double(it->second.first));
                position.addMember("y", double(it->second.second));
                node.addMember("layout", position);
            }
            nodes.addArrayElement(node);
        }
        root.addMember("nodes", nodes);
        auto asset_rows = choc::value::createEmptyArray();
        for (const auto& [key, record] : assets)
            asset_rows.addArrayElement(record);
        root.addMember("spectral_assets", asset_rows);
        std::vector<Connection> ordered_edges(graph.connections().begin(),
                                              graph.connections().end());
        std::sort(ordered_edges.begin(), ordered_edges.end(), [](const auto& a, const auto& b) {
            return std::tie(a.source_node, a.source_port, a.dest_node, a.dest_port, a.spectral) <
                   std::tie(b.source_node, b.source_port, b.dest_node, b.dest_port, b.spectral);
        });
        auto edges = choc::value::createEmptyArray();
        for (const auto& c : ordered_edges) {
            if (c.feedback || c.midi || c.automation || c.audio_rate_modulation || c.sidechain ||
                c.control_signal)
                return refuse("connection " + std::to_string(c.source_node) + ":" +
                              std::to_string(c.source_port) + " -> " + std::to_string(c.dest_node) +
                              ":" + std::to_string(c.dest_port) +
                              " is a feedback, MIDI, automation, modulation, sidechain or "
                              "control edge");
            auto edge = choc::value::createObject("Conn");
            edge.addMember("source_node", int64_t(c.source_node));
            edge.addMember("source_port", int64_t(c.source_port));
            edge.addMember("dest_node", int64_t(c.dest_node));
            edge.addMember("dest_port", int64_t(c.dest_port));
            for (const char* key :
                 {"feedback", "midi", "automation", "audio_rate_modulation", "sidechain"})
                edge.addMember(key, false);
            // v7 writes every lane flag the common connection record has, so
            // the edge type is explicit and compares like any other edge.
            if (saved_identity)
                edge.addMember("control_signal", false);
            edge.addMember("spectral", c.spectral);
            edges.addArrayElement(edge);
        }
        root.addMember("connections", edges);
        if (saved_identity) {
            // The saved-identity v7 envelope, as the plain v7 writer spells
            // it: each node's authored ID bound to its persisted node id.
            auto identities = choc::value::createEmptyArray();
            auto bindings = choc::value::createObject("Bindings");
            for (const auto* n : ordered) {
                auto item = choc::value::createObject("Identity");
                item.addMember("id", n->authored_id.str());
                item.addMember("node_id", int64_t(n->id));
                identities.addArrayElement(item);
                bindings.addMember(n->authored_id.str(), int64_t(n->id));
            }
            root.addMember("identity_schema", saved_identity_v7);
            auto logical = choc::value::createObject("LogicalIdentities");
            logical.addMember("profile", saved_identity_v7);
            logical.addMember("identities", identities);
            root.addMember("logical_identities", logical);
            root.addMember("bindings", bindings);
        }
        auto json = choc::json::toString(root, true);
        if (json.size() > GraphSerializer::max_file_bytes)
            return refuse("the spectral document exceeds the file size limit");
        // Saving must not publish a document that the strict typed reader
        // cannot reconstruct, including malformed edge-kind metadata.
        if (const auto check = read(json, admission_block_for(graph)); !check.result.ok)
            return refuse("the spectral reader refuses the written document: " +
                          check.result.error);
        return json;
    } catch (const std::exception& error) {
        return refuse(std::string("spectral serialization failed: ") + error.what());
    }
}

void fail_next_candidate_build() noexcept {
    fail_next_candidate.store(true, std::memory_order_release);
}

Candidate read(std::string_view json, int admission_block) {
    Candidate candidate;
    auto fail = [&](std::string reason) {
        candidate.handled = true;
        candidate.graph.reset();
        detail::refuse_load(candidate.result, nullptr, std::move(reason));
        return std::move(candidate);
    };
    // Only a document that parses as an object and names spectral content is
    // claimed here. Anything else is left unhandled so the ordinary reader
    // reports its own error (parser position, size limit, root shape).
    if (json.size() > GraphSerializer::max_file_bytes)
        return candidate;
    choc::value::Value root;
    try {
        root = choc::json::parse(std::string(json));
    } catch (const std::exception&) {
        return candidate;
    }
    if (!root.isObject())
        return candidate;
    candidate.handled = root.hasObjectMember("spectral_assets");
    if (root["nodes"].isArray())
        for (const auto n : root["nodes"])
            if (n.isObject() && n["type"].isString() &&
                n["type"].getString().starts_with("spectral"))
                candidate.handled = true;
    if (!candidate.handled)
        return candidate;
    try {
        std::uint64_t version = 0;
        if (!uint(root["format_version"], version) || (version != 6 && version != 7))
            return fail("spectral graph requires format_version 6 or 7 (the saved-identity v7 "
                        "document)");
        const bool v7 = version == 7;
        if (v7) {
            if (!fields(root, {"format_version", "nodes", "connections", "spectral_assets",
                               "sample_regions", "processors", "identity_schema",
                               "logical_identities", "bindings"}))
                return fail("unsupported spectral graph envelope member");
            // The common saved-document rules: every record carries a valid
            // authored_id, IDs are unique, and identities and bindings name
            // exactly the records whose authored_id they spell. Data only.
            const auto inspection = GraphSerializer::inspect_document(json);
            if (!inspection.ok())
                return fail(std::string("spectral saved-identity envelope refused: ") +
                            graph_document_refusal_name(inspection.refusal) + ": " +
                            inspection.detail);
            if (inspection.document.profile() != saved_identity_v7)
                return fail("a format 7 spectral graph requires the saved-identity v7 envelope");
        } else if (!fields(root, {"format_version", "nodes", "connections", "spectral_assets",
                                  "sample_regions", "processors"})) {
            return fail("unsupported spectral graph envelope member");
        }
        if (!root["nodes"].isArray() || !root["connections"].isArray() ||
            !root["spectral_assets"].isArray() || root["nodes"].size() > 128 ||
            root["connections"].size() > 512 || root["spectral_assets"].size() > 256)
            return fail("invalid or oversized spectral graph collections");
        if ((root.hasObjectMember("sample_regions") && root["sample_regions"].size()) ||
            (root.hasObjectMember("processors") && root["processors"].size()))
            return fail("spectral graph cannot contain region or Processor state contracts");
        const auto row_policy = v7 ? source_policy : morph_policy;
        std::map<std::string, Asset> assets;
        std::size_t decoded_bytes = 0, retained_bytes = 0;
        for (const auto row : root["spectral_assets"]) {
            if (!fields(row, {"id", "schema", "version", "source_samples", "frame_count",
                              "contract_hex", "bytes_sha256", "bytes_b64"}))
                return fail("unsupported analyzed spectral asset member");
            std::string key, schema, bytes_hash, b64, contract;
            std::uint64_t v = 0, length = 0;
            auto shape = identity(row["contract_hex"]);
            if (!text(row["id"], key) || !text(row["schema"], schema) || schema != asset_schema ||
                !uint(row["version"], v) || v != 1 || !uint(row["source_samples"], length) ||
                !length || !text(row["bytes_sha256"], bytes_hash) || !text(row["bytes_b64"], b64) ||
                !text(row["contract_hex"], contract) || !shape ||
                key != asset_id(length, contract, bytes_hash) || assets.contains(key))
                return fail("invalid, duplicate or unsupported analyzed spectral asset metadata");
            // Refuse geometry or extent the finite stream rule does not admit,
            // and a stored frame count it does not derive, before decoding.
            std::uint64_t frames = 0;
            const auto rule = stream_rule(*shape, length, row_policy);
            if (!rule || !uint(row["frame_count"], frames) || frames != rule.frame_count)
                return fail("analyzed spectral asset geometry refused by the finite stream rule");
            // Canonical base64 admits no whitespace or alternative padding.
            // Bound its decoded allocation before calling the shared decoder.
            if (b64.size() % 4 || b64.size() > budget ||
                !charge(decoded_bytes, (b64.size() / 4) * 3))
                return fail("spectral asset byte budget exceeded");
            auto bytes = runtime::base64_decode(b64);
            if (!bytes || runtime::base64_encode(bytes->data(), bytes->size()) != b64 ||
                digest(*bytes) != bytes_hash)
                return fail("spectral asset canonical bytes or SHA256 mismatch");
            assets.emplace(key, Asset{std::move(*bytes), length, shape});
        }
        // The finite-source policy a v7 source node records must be the one
        // its factory applies; an unknown or different version is refused
        // rather than reinterpreted.
        const auto policy_is = [](View v, Policy policy) {
            std::uint64_t recorded = 0;
            return uint(v, recorded) && recorded == static_cast<std::uint32_t>(policy);
        };
        auto graph = std::make_unique<SignalGraph>();
        std::map<std::uint64_t, NodeId> ids;
        for (const auto n : root["nodes"]) {
            const bool known =
                v7 ? fields(n, {"id", "authored_id", "type", "name", "num_input_ports",
                                "num_output_ports", "gain", "spectral", "layout"})
                   : fields(n, {"id", "type", "name", "num_input_ports", "num_output_ports", "gain",
                                "spectral", "layout"});
            if (!known)
                return fail("unsupported spectral graph node member");
            std::uint64_t old = 0, in = 0, out = 0;
            std::string type, name;
            double gain = 0;
            if (!uint(n["id"], old, std::numeric_limits<NodeId>::max()) || !old ||
                !text(n["type"], type) || !text(n["name"], name) ||
                !uint(n["num_input_ports"], in, 64) || !uint(n["num_output_ports"], out, 64) ||
                !real(n["gain"], gain) || gain != 1 || ids.contains(old))
                return fail("invalid or duplicate spectral graph node identity/shape");
            NodeId id = 0;
            if (type == "audio_in" && in == 0 && out > 0 && !n.hasObjectMember("spectral"))
                id = graph->add_input_node(static_cast<int>(out), name);
            else if (type == "audio_out" && in > 0 && out == 0 && !n.hasObjectMember("spectral"))
                id = graph->add_output_node(static_cast<int>(in), name);
            else {
                const auto spec = n["spectral"];
                auto shape = identity(spec["contract_hex"]);
                if (!spec.isObject() || !shape)
                    return fail("missing or unsupported spectral frame contract");
                if (v7 && !identity_matches(spec["identity"], *shape))
                    return fail("spectral node identity does not match its frame contract, "
                                "including the exact sample rate bits");
                if (type == "spectral_asset_morph") {
                    if (!(v7 ? fields(spec, {"contract_hex", "identity", "asset_a", "asset_b",
                                             "finite_policy"})
                             : fields(spec, {"contract_hex", "asset_a", "asset_b"})))
                        return fail("unsupported spectral source contract member");
                    if (v7 && !policy_is(spec["finite_policy"], morph_policy))
                        return fail("unsupported finite-source policy version for an asset "
                                    "morph");
                    std::string a, b;
                    if (!text(spec["asset_a"], a) || !text(spec["asset_b"], b) ||
                        !assets.contains(a) || !assets.contains(b))
                        return fail("missing analyzed spectral asset reference");
                    auto& aa = assets.at(a);
                    auto& bb = assets.at(b);
                    if (aa.length != bb.length || aa.shape != shape || bb.shape != shape)
                        return fail("analyzed spectral asset extent/contract mismatch");
                    const auto added =
                        graph->add_spectral_asset_morph_v1(aa.bytes, bb.bytes, aa.length, name);
                    // The factory derives the identity from the embedded bytes;
                    // a row that claims another contract is refused.
                    if (!added || graph->node(added.value->node)->spectral_identity != shape)
                        return fail("canonical spectral asset validation/factory refused");
                    id = added.value->node;
                    aa.used = bb.used = true;
                    if (!charge(retained_bytes,
                                graph->node(id)->spectral_asset_morph->retained_bytes()))
                        return fail("aggregate retained spectral asset budget exceeded");
                } else if (v7 && type == "spectral_asset_source") {
                    if (!fields(spec, {"contract_hex", "identity", "asset", "finite_policy"}))
                        return fail("unsupported spectral asset source contract member");
                    if (!policy_is(spec["finite_policy"], source_policy))
                        return fail("unsupported finite-source policy version for an asset "
                                    "source");
                    std::string a;
                    if (!text(spec["asset"], a) || !assets.contains(a))
                        return fail("missing analyzed spectral asset reference");
                    auto& asset = assets.at(a);
                    if (asset.shape != shape)
                        return fail("analyzed spectral asset extent/contract mismatch");
                    const auto added =
                        graph->add_spectral_asset_source_v1(asset.bytes, asset.length, name);
                    if (!added || graph->node(added.value->node)->spectral_identity != shape)
                        return fail("canonical spectral asset validation/factory refused");
                    id = added.value->node;
                    asset.used = true;
                    if (!charge(retained_bytes,
                                graph->node(id)->spectral_asset_source->retained_bytes()))
                        return fail("aggregate retained spectral asset budget exceeded");
                } else if (v7 && type == "spectral_analysis") {
                    // The declaration is the identity; no input audio or frame
                    // history is admitted in the record.
                    if (!fields(spec, {"contract_hex", "identity"}))
                        return fail("unsupported spectral analysis contract member");
                    const auto added = graph->add_spectral_analysis(*shape, name);
                    if (added)
                        id = added.value->node;
                } else if (type == "spectral_synthesis") {
                    if (!(v7 ? fields(spec, {"contract_hex", "identity"})
                             : fields(spec, {"contract_hex"})))
                        return fail("unsupported spectral synthesis contract member");
                    const auto added = graph->add_spectral_synthesis(*shape, name);
                    if (added)
                        id = added.value->node;
                } else if (type == "spectral_stage") {
                    if (!(v7 ? fields(spec, {"contract_hex", "identity", "stage", "gains"})
                             : fields(spec, {"contract_hex", "stage", "gains"})))
                        return fail("unsupported spectral stage contract member");
                    std::string kind;
                    if (!text(spec["stage"], kind) || !spec["gains"].isArray() ||
                        spec["gains"].size() > 16u * 8193u)
                        return fail("invalid spectral stage contract");
                    std::vector<float> gains;
                    for (const auto g : spec["gains"]) {
                        double value = 0;
                        if (!real(g, value) || value < 0 || value > 1 ||
                            double(float(value)) != value)
                            return fail("invalid spectral mask gain");
                        gains.push_back(static_cast<float>(value));
                    }
                    SpectralResult<QualifiedSpectralStage> stage;
                    if (kind == "identity_v1" && gains.empty())
                        stage = QualifiedSpectralStage::identity_v1(*shape);
                    else if (kind == "real_mask_v1")
                        stage = QualifiedSpectralStage::real_mask_v1(*shape, gains);
                    if (!stage || !charge(retained_bytes, stage.value->retained_config_bytes()))
                        return fail("unsupported or oversized spectral stage");
                    const auto added = graph->add_spectral_stage(*shape, *stage.value, name);
                    if (added)
                        id = added.value->node;
                }
            }
            if (!id || graph->node(id)->num_input_ports != int(in) ||
                graph->node(id)->num_output_ports != int(out))
                return fail("unsupported spectral node type or serialized port shape");
            ids.emplace(old, id);
            if (n.hasObjectMember("layout")) {
                if (!fields(n["layout"], {"x", "y"}))
                    return fail("unsupported spectral layout member");
                double x = 0, y = 0;
                if (!real(n["layout"]["x"], x) || !real(n["layout"]["y"], y) ||
                    !std::isfinite(float(x)) || !std::isfinite(float(y)))
                    return fail("invalid spectral graph editor layout");
                candidate.result.editor_layout.emplace(id, std::pair{float(x), float(y)});
            }
        }
        if (fail_next_candidate.exchange(false, std::memory_order_acq_rel))
            throw std::bad_alloc();
        for (const auto& [key, asset] : assets)
            if (!asset.used)
                return fail("unreferenced analyzed spectral asset");
        for (const auto edge : root["connections"]) {
            const bool known =
                v7 ? fields(edge, {"source_node", "source_port", "dest_node", "dest_port",
                                   "feedback", "midi", "automation", "audio_rate_modulation",
                                   "sidechain", "control_signal", "spectral"})
                   : fields(edge, {"source_node", "source_port", "dest_node", "dest_port",
                                   "feedback", "midi", "automation", "audio_rate_modulation",
                                   "sidechain", "spectral"});
            if (!known)
                return fail("unsupported spectral connection member");
            std::uint64_t src = 0, dst = 0, sp = 0, dp = 0;
            if (!uint(edge["source_node"], src) || !uint(edge["dest_node"], dst) ||
                !uint(edge["source_port"], sp, 65535) || !uint(edge["dest_port"], dp, 65535) ||
                !ids.contains(src) || !ids.contains(dst) || !edge["spectral"].isBool())
                return fail("invalid spectral connection endpoint/metadata");
            for (const auto* key : {"feedback", "midi", "automation", "audio_rate_modulation",
                                    "sidechain", "control_signal"}) {
                if (!v7 && std::string_view(key) == "control_signal")
                    continue;
                if (!edge[key].isBool() || edge[key].getBool())
                    return fail("unsupported mixed spectral connection contract");
            }
            const bool spectral = edge["spectral"].getBool();
            auto port = [&](NodeId node, std::uint64_t index,
                            bool output) -> std::optional<PortHandle> {
                const auto* n = graph->node(node);
                for (const auto& p : output ? n->output_ports : n->input_ports)
                    if (p.index == index &&
                        p.kind == (spectral ? PortKind::Spectral : PortKind::Audio))
                        return p;
                return {};
            };
            const auto a = port(ids.at(src), sp, true), b = port(ids.at(dst), dp, false);
            if (!a || !b || !graph->connect(*a, *b))
                return fail("spectral connection kind/identity/cycle validation refused");
        }
        if (!contains_spectral_nodes(*graph))
            return fail("spectral envelope has no spectral nodes");
        // Exercise the existing compiler/bank admission on the isolated
        // candidate, including complete input topology and the 64 MiB bank
        // bound at the admission block size. Edge storage grows with the block
        // size, so a graph admitted at one block can be refused at a larger one.
        // Preparation runs at the recorded rate of the first spectral node, so
        // a node whose recorded rate differs is refused here.
        // Return a stopped unprepared authoring graph.
        double rate = 0;
        for (const auto& n : graph->nodes())
            if (n.spectral_identity) {
                rate = n.spectral_identity->sample_rate();
                break;
            }
        if (!graph->prepare(rate, admission_block))
            return fail("spectral graph compiler/preparation admission refused");
        if (graph->stop_spectral_controller() != SpectralError::None)
            return fail("spectral candidate could not stop after validation");
        graph->release();
        for (const auto& [persisted, live] : ids)
            candidate.result.persisted_node_ids.emplace(static_cast<NodeId>(persisted), live);
        candidate.result.ok = true;
        candidate.graph = std::move(graph);
        return candidate;
    } catch (const std::exception&) {
        return fail("invalid spectral graph JSON or allocation failure");
    }
}
} // namespace pulp::host::spectral_persistence
