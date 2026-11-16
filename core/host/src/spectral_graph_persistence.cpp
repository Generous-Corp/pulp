#include "spectral_graph_persistence.hpp"
#include "spectral_asset_morph_internal.hpp"
#include "spectral_finite_stream_internal.hpp"

#include <choc/text/choc_JSON.h>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
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
// The saved frame contract and original extent determine the finite stream:
// the same rule the source factory and runtime use, so a saved asset carries
// no geometry the runtime would derive differently.
SpectralFiniteStreamRule stream_rule(const SpectralIdentity& shape, std::uint64_t length) {
    return SpectralFiniteStreamRule::make(static_cast<std::uint64_t>(shape.fft_size()),
                                          static_cast<std::uint64_t>(shape.hop()),
                                          static_cast<std::uint64_t>(shape.channels()), length);
}
const char* type_name(NodeType type) {
    switch (type) {
    case NodeType::AudioInput:
        return "audio_in";
    case NodeType::AudioOutput:
        return "audio_out";
    case NodeType::SpectralAssetMorph:
        return "spectral_asset_morph";
    case NodeType::SpectralStage:
        return "spectral_stage";
    case NodeType::SpectralSynthesis:
        return "spectral_synthesis";
    default:
        return nullptr;
    }
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
                  const std::unordered_map<NodeId, std::pair<float, float>>& layout) {
    try {
        if (!graph.sample_regions().empty() || graph.nodes().size() > 128 ||
            graph.connections().size() > 512)
            return {};
        auto root = choc::value::createObject("PulpGraph");
        root.addMember("format_version", int64_t(6));
        auto nodes = choc::value::createEmptyArray();
        std::map<std::string, Value> assets;
        std::size_t encoded_charge = 0, retained_charge = 0;
        auto add_asset = [&](const signal::OwnedSpectralStream& stream, std::uint64_t length,
                             const std::array<std::uint8_t, 32>& expected_hash,
                             const SpectralIdentity& shape,
                             const std::string& contract) -> std::string {
            const auto rule = stream_rule(shape, length);
            if (!rule || stream.frame_count != rule.frame_count)
                return {};
            const auto encoded = signal::encode_spectral_stream(stream, budget);
            if (!encoded || digest(encoded.value->view()) != hex(expected_hash))
                return {};
            const auto hash = hex(expected_hash);
            const auto key = asset_id(length, contract, hash);
            if (assets.contains(key))
                return key;
            // Include base64 expansion before allocating its encoded string.
            if (encoded.value->size > budget ||
                !charge(encoded_charge, ((encoded.value->size + 2) / 3) * 4))
                return {};
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
            if (!type || n.gain != 1 || graph.is_processor_node(n.id))
                return {};
            auto node = choc::value::createObject("Node");
            node.addMember("id", int64_t(n.id));
            node.addMember("type", type);
            node.addMember("name", n.name);
            node.addMember("num_input_ports", int64_t(n.num_input_ports));
            node.addMember("num_output_ports", int64_t(n.num_output_ports));
            node.addMember("gain", 1.0);
            if (n.spectral_identity) {
                const auto contract = hex(n.spectral_identity->frame_contract().encode());
                auto spectral = choc::value::createObject("SpectralNode");
                spectral.addMember("contract_hex", contract);
                if (n.type == NodeType::SpectralAssetMorph) {
                    const auto& config = n.spectral_asset_morph;
                    if (!config || !charge(retained_charge, config->retained_bytes()))
                        return {};
                    const auto& shape = *n.spectral_identity;
                    const auto a = add_asset(config->a, config->source_samples, config->hash_a,
                                             shape, contract);
                    const auto b = add_asset(config->b, config->source_samples, config->hash_b,
                                             shape, contract);
                    if (a.empty() || b.empty())
                        return {};
                    spectral.addMember("asset_a", a);
                    spectral.addMember("asset_b", b);
                } else if (n.type == NodeType::SpectralStage) {
                    if (!n.spectral_stage || n.spectral_stage->identity() != *n.spectral_identity ||
                        !charge(retained_charge, n.spectral_stage->retained_config_bytes()))
                        return {};
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
                node.addMember("spectral", spectral);
            } else if (n.type != NodeType::AudioInput && n.type != NodeType::AudioOutput)
                return {};
            if (const auto it = layout.find(n.id); it != layout.end()) {
                if (!std::isfinite(it->second.first) || !std::isfinite(it->second.second))
                    return {};
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
                return {};
            auto edge = choc::value::createObject("Conn");
            edge.addMember("source_node", int64_t(c.source_node));
            edge.addMember("source_port", int64_t(c.source_port));
            edge.addMember("dest_node", int64_t(c.dest_node));
            edge.addMember("dest_port", int64_t(c.dest_port));
            for (const char* key :
                 {"feedback", "midi", "automation", "audio_rate_modulation", "sidechain"})
                edge.addMember(key, false);
            edge.addMember("spectral", c.spectral);
            edges.addArrayElement(edge);
        }
        root.addMember("connections", edges);
        auto json = choc::json::toString(root, true);
        if (json.size() > GraphSerializer::max_file_bytes)
            return {};
        // Saving must not publish a document that the strict typed reader
        // cannot reconstruct, including malformed edge-kind metadata.
        if (!read(json, admission_block_for(graph)).result.ok)
            return {};
        return json;
    } catch (const std::exception&) {
        return {};
    }
}

Candidate read(std::string_view json, int admission_block) {
    Candidate candidate;
    auto fail = [&](const char* reason) {
        candidate.handled = true;
        candidate.result.error = reason;
        candidate.graph.reset();
        return std::move(candidate);
    };
    try {
        if (json.size() > GraphSerializer::max_file_bytes)
            return fail("spectral graph exceeds 64 MiB file limit");
        const auto root = choc::json::parse(std::string(json));
        if (!root.isObject())
            return fail("root is not an object");
        candidate.handled = root.hasObjectMember("spectral_assets");
        if (root["nodes"].isArray())
            for (const auto n : root["nodes"])
                if (n["type"].isString() && n["type"].getString().starts_with("spectral"))
                    candidate.handled = true;
        if (!candidate.handled)
            return candidate;
        std::uint64_t version = 0;
        if (!uint(root["format_version"], version) || version != 6)
            return fail("spectral graph requires format_version 6");
        if (!fields(root, {"format_version", "nodes", "connections", "spectral_assets",
                           "sample_regions", "processors"}))
            return fail("unsupported spectral graph envelope member");
        if (!root["nodes"].isArray() || !root["connections"].isArray() ||
            !root["spectral_assets"].isArray() || root["nodes"].size() > 128 ||
            root["connections"].size() > 512 || root["spectral_assets"].size() > 256)
            return fail("invalid or oversized spectral graph collections");
        if ((root.hasObjectMember("sample_regions") && root["sample_regions"].size()) ||
            (root.hasObjectMember("processors") && root["processors"].size()))
            return fail("spectral graph cannot contain region or Processor state contracts");
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
            const auto rule = stream_rule(*shape, length);
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
        auto graph = std::make_unique<SignalGraph>();
        std::map<std::uint64_t, NodeId> ids;
        for (const auto n : root["nodes"]) {
            if (!fields(n, {"id", "type", "name", "num_input_ports", "num_output_ports", "gain",
                            "spectral", "layout"}))
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
                if (type == "spectral_asset_morph") {
                    if (!fields(spec, {"contract_hex", "asset_a", "asset_b"}))
                        return fail("unsupported spectral source contract member");
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
                    if (!added)
                        return fail("canonical spectral asset validation/factory refused");
                    id = added.value->node;
                    aa.used = bb.used = true;
                    if (!charge(retained_bytes,
                                graph->node(id)->spectral_asset_morph->retained_bytes()))
                        return fail("aggregate retained spectral asset budget exceeded");
                } else if (type == "spectral_synthesis") {
                    if (!fields(spec, {"contract_hex"}))
                        return fail("unsupported spectral synthesis contract member");
                    const auto added = graph->add_spectral_synthesis(*shape, name);
                    if (added)
                        id = added.value->node;
                } else if (type == "spectral_stage") {
                    if (!fields(spec, {"contract_hex", "stage", "gains"}))
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
        for (const auto& [key, asset] : assets)
            if (!asset.used)
                return fail("unreferenced analyzed spectral asset");
        for (const auto edge : root["connections"]) {
            if (!fields(edge,
                        {"source_node", "source_port", "dest_node", "dest_port", "feedback", "midi",
                         "automation", "audio_rate_modulation", "sidechain", "spectral"}))
                return fail("unsupported spectral connection member");
            std::uint64_t src = 0, dst = 0, sp = 0, dp = 0;
            if (!uint(edge["source_node"], src) || !uint(edge["dest_node"], dst) ||
                !uint(edge["source_port"], sp, 65535) || !uint(edge["dest_port"], dp, 65535) ||
                !ids.contains(src) || !ids.contains(dst) || !edge["spectral"].isBool())
                return fail("invalid spectral connection endpoint/metadata");
            for (const auto* key :
                 {"feedback", "midi", "automation", "audio_rate_modulation", "sidechain"})
                if (!edge[key].isBool() || edge[key].getBool())
                    return fail("unsupported mixed spectral connection contract");
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
        candidate.result.ok = true;
        candidate.graph = std::move(graph);
        return candidate;
    } catch (const std::exception&) {
        return fail("invalid spectral graph JSON or allocation failure");
    }
}
} // namespace pulp::host::spectral_persistence
