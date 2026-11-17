#include "graph_patch_constructors.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>

namespace pulp::host::patch_constructors {
namespace {
using Code = GraphPatchDiagnostic::Code;

constexpr std::string_view audio_in = "pulp/audio_in", audio_out = "pulp/audio_out",
                           analysis = "pulp/spectral_analysis", stage = "pulp/spectral_stage",
                           synthesis = "pulp/spectral_synthesis",
                           asset_morph = "pulp/spectral_asset_morph";
// Notation boundaries and spectral identities share the SpectralIdentity
// channel domain, so any declared layout can feed a spectral node.
constexpr int max_channels = 16;
constexpr const char* identity_arguments = "(fft N, hop H, channels C, rate R)";

GraphPatchDiagnostic fail(Code code, std::string rule, std::string fix, std::string node) {
    return {code, 0, std::move(node), {}, std::move(rule), std::move(fix)};
}
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}
const std::string* find(const Arguments& arguments, std::string_view key) {
    for (const auto& [k, v] : arguments.items)
        if (k == key)
            return &v;
    return nullptr;
}
GraphPatchDiagnostic admit_keys(const Arguments& arguments,
                                std::initializer_list<std::string_view> allowed,
                                const std::string& name, std::string_view key) {
    for (const auto& [k, v] : arguments.items)
        if (std::find(allowed.begin(), allowed.end(), k) == allowed.end()) {
            std::string list;
            for (const auto a : allowed)
                list += (list.empty() ? "" : ", ") + std::string(a);
            return fail(Code::SyntaxError,
                        "unknown constructor argument '" + k + "' for " + std::string(key),
                        "use only: " + list, name);
        }
    return {};
}
bool integer(const std::string& text, int& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
GraphPatchDiagnostic channel_count(const std::string& text, const std::string& name, int& out) {
    if (!integer(text, out))
        return fail(Code::SyntaxError, "channels must be an integer",
                    "write channels as a decimal integer in [1, 16]", name);
    if (out < 1 || out > max_channels)
        return fail(Code::UnsupportedChannelLayout,
                    "unsupported channel layout: " + text +
                        " channels; notation admits 1 to 16 indexed channels",
                    "use channels in [1, 16], or build the layout with the typed API", name);
    return {};
}
std::string spell_rate(double rate) {
    char buffer[64];
    const auto r = std::to_chars(buffer, buffer + sizeof(buffer), rate);
    return {buffer, r.ptr};
}
const char* stage_kind_refusal() {
    return "keep mask gains in saved-graph JSON (GraphSerializer) or author the stage with "
           "QualifiedSpectralStage::real_mask_v1 and SignalGraph::add_spectral_stage";
}
GraphPatchDiagnostic check_rate(DocumentRate& rate, std::uint64_t bits, const std::string& name) {
    if (rate.bits && *rate.bits != bits)
        return fail(Code::SampleRateMismatch,
                    "spectral node '" + name + "' declares rate " +
                        spell_rate(std::bit_cast<double>(bits)) + " but '" + rate.node +
                        "' declares " + spell_rate(std::bit_cast<double>(*rate.bits)) +
                        "; a document has one sample rate",
                    "declare the same rate on every spectral node", name);
    if (!rate.bits) {
        rate.bits = bits;
        rate.node = name;
    }
    return {};
}
template <typename Ports> NodeId added(const SpectralResult<Ports>& result) {
    return result ? result.value->node : NodeId{0};
}
} // namespace

GraphPatchDiagnostic split_arguments(std::string_view type_text, std::string& type,
                                     Arguments& arguments) {
    const auto text = trim(type_text);
    arguments = {};
    type = text;
    if (text.empty() || text.back() != ')')
        return {};
    char quoted = 0;
    bool escaped = false;
    std::size_t open = text.npos;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted) {
            if (quoted == '"' && c == '\\')
                escaped = true;
            else if (c == quoted)
                quoted = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            quoted = c;
        else if (c == '(' || c == ')') {
            if (c == ')' || open != text.npos)
                return fail(Code::SyntaxError, "nested or unbalanced constructor arguments",
                            std::string("use one list, e.g. ") + identity_arguments, {});
            open = i;
        }
    }
    if (open == text.npos)
        return fail(Code::SyntaxError, "unbalanced constructor arguments",
                    "open the argument list with '('", {});
    type = trim(text.substr(0, open));
    arguments.present = true;
    const auto inner = text.substr(open + 1, text.size() - open - 2);
    if (trim(inner).empty())
        return fail(Code::SyntaxError, "empty constructor arguments",
                    "omit the parentheses or name each argument", {});
    std::size_t start = 0;
    while (start <= inner.size()) {
        auto end = inner.find(',', start);
        if (end == inner.npos)
            end = inner.size();
        const auto item = trim(inner.substr(start, end - start));
        const auto gap = item.find_first_of(" \t");
        const auto value = gap == item.npos ? std::string{} : trim(item.substr(gap));
        const auto key = item.substr(0, gap == item.npos ? item.size() : gap);
        const bool word = !key.empty() && std::all_of(key.begin(), key.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || c == '_';
        });
        if (!word || value.empty() || value.find_first_of(" \t\"'") != value.npos)
            return fail(Code::SyntaxError, "invalid constructor argument '" + item + "'",
                        "write each argument as 'key value', e.g. fft 1024", {});
        if (find(arguments, key))
            return fail(Code::Duplicate, "constructor argument '" + key + "' is given twice",
                        "give each argument once", {});
        arguments.items.emplace_back(key, value);
        start = end + 1;
    }
    return {};
}

bool owns(std::string_view key) {
    return key == audio_in || key == audio_out || key == analysis || key == stage ||
           key == synthesis || key == asset_morph;
}

GraphPatchDiagnostic construct(SignalGraph& graph, std::string_view key, const Arguments& arguments,
                               const std::string& name, DocumentRate& rate, NodeId& id) {
    id = 0;
    if (key == asset_morph)
        return fail(Code::UnsupportedContract, "spectral asset payloads have no notation spelling",
                    "keep asset graphs in saved-graph JSON, where GraphSerializer embeds each "
                    "asset with its hash, or use SignalGraph::add_spectral_asset_morph_v1",
                    name);
    if (key == audio_in || key == audio_out) {
        if (auto d = admit_keys(arguments, {"channels"}, name, key); !d)
            return d;
        int channels = 2;
        if (const auto* text = find(arguments, "channels"))
            if (auto d = channel_count(*text, name, channels); !d)
                return d;
        id = key == audio_in ? graph.add_input_node(channels, name)
                             : graph.add_output_node(channels, name);
        return id ? GraphPatchDiagnostic{}
                  : fail(Code::UnsupportedContract, "boundary constructor refused",
                         "add boundaries before preparing a spectral graph", name);
    }
    const bool is_stage = key == stage;
    if (!arguments.present)
        return fail(Code::SyntaxError,
                    "spectral constructor '" + std::string(key) + "' needs its identity",
                    std::string("write ") + name + " : " + std::string(key) + " " +
                        identity_arguments + (is_stage ? " with kind identity" : ""),
                    name);
    if (auto d = is_stage
                     ? admit_keys(arguments, {"fft", "hop", "channels", "rate", "kind"}, name, key)
                     : admit_keys(arguments, {"fft", "hop", "channels", "rate"}, name, key);
        !d)
        return d;
    for (const char* required : {"fft", "hop", "channels"})
        if (!find(arguments, required))
            return fail(Code::SyntaxError,
                        std::string("missing constructor argument '") + required + "'",
                        std::string("write ") + identity_arguments, name);
    const auto* rate_text = find(arguments, "rate");
    if (!rate_text)
        return fail(Code::SyntaxError,
                    "missing sample rate: spectral constructors require an explicit 'rate'",
                    "add rate R (Hz) to the arguments; preparation refuses any other rate", name);
    int fft = 0, hop = 0, channels = 0;
    if (!integer(*find(arguments, "fft"), fft) || !integer(*find(arguments, "hop"), hop))
        return fail(Code::SyntaxError, "fft and hop must be integers",
                    "write fft and hop as decimal integers", name);
    if (auto d = channel_count(*find(arguments, "channels"), name, channels); !d)
        return d;
    double sample_rate = 0;
    {
        std::istringstream parser(*rate_text);
        parser.imbue(std::locale::classic());
        parser >> std::noskipws >> sample_rate;
        if (!parser || !parser.eof() || !std::isfinite(sample_rate) || !(sample_rate > 0))
            return fail(Code::ParameterOutOfRange, "rate must be a finite positive number",
                        "write the sample rate in Hz, e.g. rate 48000", name);
    }
    const auto identity = SpectralIdentity::make(fft, hop, channels, sample_rate);
    if (!identity)
        return fail(Code::ParameterOutOfRange,
                    "invalid spectral identity: fft " + std::to_string(fft) + ", hop " +
                        std::to_string(hop),
                    "use fft a power of two in [256, 16384] and hop in [1, fft / 2]", name);
    if (auto d = check_rate(rate, identity.value->sample_rate_bits(), name); !d)
        return d;
    if (key == analysis)
        id = added(graph.add_spectral_analysis(*identity.value, name));
    else if (key == synthesis)
        id = added(graph.add_spectral_synthesis(*identity.value, name));
    else {
        const auto* kind = find(arguments, "kind");
        if (!kind)
            return fail(Code::SyntaxError, "missing constructor argument 'kind'",
                        "write kind identity", name);
        if (*kind == "mask")
            return fail(Code::UnsupportedContract,
                        "a real-mask stage carries channels x bins gains, which have no notation "
                        "spelling",
                        stage_kind_refusal(), name);
        if (*kind != "identity")
            return fail(Code::SyntaxError, "unknown stage kind '" + *kind + "'",
                        "use kind identity", name);
        auto qualified = QualifiedSpectralStage::identity_v1(*identity.value);
        if (qualified)
            id =
                added(graph.add_spectral_stage(*identity.value, std::move(*qualified.value), name));
    }
    return id ? GraphPatchDiagnostic{}
              : fail(Code::UnsupportedContract, "spectral constructor refused",
                     "declare spectral nodes in a graph that has not been prepared", name);
}

GraphPatchDiagnostic spell(const GraphNode& node, DocumentRate& rate, std::string& spelled) {
    spelled.clear();
    if (node.type == NodeType::AudioInput || node.type == NodeType::AudioOutput) {
        const bool input = node.type == NodeType::AudioInput;
        const int channels = input ? node.num_output_ports : node.num_input_ports;
        if (channels == 2)
            return {};
        int checked = 0;
        if (auto d = channel_count(std::to_string(channels), node.name, checked); !d)
            return d;
        spelled = std::string(input ? audio_in : audio_out) + " (channels " +
                  std::to_string(channels) + ")";
        return {};
    }
    if (!node.spectral_identity)
        return {};
    const auto& identity = *node.spectral_identity;
    std::string_view key;
    std::string extra;
    switch (node.type) {
    case NodeType::SpectralAnalysis:
        key = analysis;
        break;
    case NodeType::SpectralSynthesis:
        key = synthesis;
        break;
    case NodeType::SpectralStage:
        if (!node.spectral_stage)
            return fail(Code::UnsupportedContract, "spectral stage has no qualified stage",
                        "construct stages through SignalGraph::add_spectral_stage", node.name);
        if (node.spectral_stage->kind() != QualifiedSpectralStage::Kind::IdentityV1)
            return fail(Code::UnsupportedContract,
                        "spectral stage '" + node.name + "' is a real mask of " +
                            std::to_string(node.spectral_stage->gains().size()) + " gains (" +
                            std::to_string(identity.channels()) + " channels x " +
                            std::to_string(identity.bins()) +
                            " bins); notation spells no mask payload and would drop it",
                        stage_kind_refusal(), node.name);
        key = stage;
        extra = ", kind identity";
        break;
    case NodeType::SpectralAssetMorph:
    case NodeType::SpectralAssetSource:
        return fail(Code::UnsupportedContract,
                    "spectral node '" + node.name +
                        "' owns codec asset payloads, which have no notation spelling",
                    "keep asset graphs in saved-graph JSON, where GraphSerializer embeds each "
                    "asset with its hash",
                    node.name);
    default:
        return fail(Code::UnsupportedContract, "node type has no spectral notation spelling",
                    "use the typed SignalGraph::add_spectral_* constructors", node.name);
    }
    if (auto d = check_rate(rate, identity.sample_rate_bits(), node.name); !d)
        return d;
    spelled = std::string(key) + " (fft " + std::to_string(identity.fft_size()) + ", hop " +
              std::to_string(identity.hop()) + ", channels " + std::to_string(identity.channels()) +
              ", rate " + spell_rate(identity.sample_rate()) + extra + ")";
    return {};
}

} // namespace pulp::host::patch_constructors
