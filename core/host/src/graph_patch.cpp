#include <choc/text/choc_JSON.h>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/graph_patch.hpp>

#include "graph_patch_grammar.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace pulp::host {
namespace {
using Code = GraphPatchDiagnostic::Code;
struct Refusal {
    GraphPatchDiagnostic diagnostic;
};
[[noreturn]] void refuse(Code code, std::string rule, std::string fix, std::string node = {},
                         std::string port = {}, std::size_t line = 0) {
    throw Refusal{{code, line, std::move(node), std::move(port), std::move(rule), std::move(fix)}};
}
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \r\n\t");
    if (first == text.npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1));
}
std::string number(float v) {
    char buffer[64];
    const auto r = std::to_chars(buffer, buffer + sizeof(buffer), v);
    if (r.ec != std::errc{} || !std::isfinite(v))
        refuse(Code::ParameterOutOfRange, "value is not finite", "use a finite catalog value");
    return {buffer, r.ptr};
}
float numeric(std::string_view text) {
    auto value = trim(text);
    if (!value.empty() && value.front() == '+')
        value.erase(0, 1);
    float result = 0;
    std::istringstream parser(value);
    parser.imbue(std::locale::classic());
    parser >> std::noskipws >> result;
    if (!parser || !parser.eof() || !std::isfinite(result))
        refuse(Code::ParameterOutOfRange, "value is not a finite number",
               "use a finite catalog value");
    return result;
}
std::string unquote(std::string text) {
    if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'')
        return text.substr(1, text.size() - 2);
    if (text.size() >= 2 && text.front() == '"') {
        try {
            return std::string(choc::json::parseValue(text).getString());
        } catch (...) {
            refuse(Code::SyntaxError, "invalid quoted name",
                   "escape double quotes and backslashes");
        }
    }
    return text;
}
std::string quote(std::string_view text) {
    if (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '/' || c == '-';
        }))
        return std::string(text);
    return choc::json::toString(choc::value::Value(std::string(text)));
}
// Separators and comments only act outside quoted names. The same scan is used
// for chains, fanout, values, versions and modulation ranges.
std::vector<std::string> split(std::string_view text, std::string_view delimiter) {
    std::vector<std::string> result;
    char quoted = 0;
    bool escaped = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && c == '\\' && quoted == '"') {
            escaped = true;
            continue;
        }
        if (quoted) {
            if (c == quoted)
                quoted = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quoted = c;
            continue;
        }
        if (text.substr(i).starts_with(delimiter)) {
            if (delimiter == "#" && i > 0 && text[i - 1] != ' ' && text[i - 1] != '\t')
                continue;
            result.push_back(trim(text.substr(start, i - start)));
            i += delimiter.size() - 1;
            start = i + 1;
        }
    }
    if (quoted)
        refuse(Code::SyntaxError, "unterminated quoted name", "close the quoted name");
    result.push_back(trim(text.substr(start)));
    return result;
}
struct Endpoint {
    std::string node, port;
};
Endpoint endpoint(const std::string& text) {
    static const std::regex grammar(patch_grammar::endpoint);
    std::smatch m;
    if (!std::regex_match(text, m, grammar))
        refuse(Code::SyntaxError, "invalid endpoint '" + text + "'",
               "use node.port or a quoted port name");
    std::string port;
    if (m[2].matched)
        port = unquote("\"" + m[2].str() + "\"");
    else if (m[3].matched)
        port = m[3].str();
    else
        port = m[4].str();
    return {m[1].str(), port};
}
bool midi_capable(const SignalGraph& graph, NodeId id, bool output) {
    const auto* n = graph.node(id);
    if (!n)
        return false;
    const auto& list = output ? n->output_ports : n->input_ports;
    return std::any_of(list.begin(), list.end(),
                       [](const auto& p) { return p.kind == PortKind::Midi; });
}
std::vector<PortHandle> ports(const SignalGraph& graph, NodeId id, bool output) {
    const auto* n = graph.node(id);
    if (!n)
        refuse(Code::UnknownNode, "unknown node", "declare the node before using it");
    return output ? n->output_ports : n->input_ports;
}
PortHandle resolve_port(const SignalGraph& graph, NodeId id, const std::string& name, bool output) {
    const auto available = ports(graph, id, output);
    if (name.empty() && available.size() == 1)
        return available.front();
    std::string choices;
    for (const auto& p : available) {
        if (p.name == name)
            return p;
        choices += (choices.empty() ? "" : ", ") + p.name;
    }
    const auto& node_name = graph.node(id)->name;
    if (name.empty() && !available.empty()) {
        const std::string side = output ? "output" : "input";
        refuse(Code::UnknownPort,
               "ambiguous port: node '" + node_name + "' has " + std::to_string(available.size()) +
                   " " + side +
                   " ports and a chain hop infers a port only when the node has exactly one " +
                   side,
               "write the port explicitly, e.g. " + node_name + "." + available.front().name +
                   (output ? " >> next.audio" : " as the destination") + "; use one of: " + choices,
               node_name, name);
    }
    refuse(Code::UnknownPort, "unknown port '" + name + "'",
           "use one of: " + (choices.empty() ? std::string("none") : choices), node_name, name);
}
bool has_path(const SignalGraph& graph, NodeId from, NodeId to, std::set<NodeId>& seen) {
    if (from == to)
        return true;
    if (!seen.insert(from).second)
        return false;
    for (const auto& c : graph.connections())
        if (!c.feedback && c.source_node == from && has_path(graph, c.dest_node, to, seen))
            return true;
    return false;
}
void check_cycle(const SignalGraph& graph, NodeId source, NodeId dest) {
    std::set<NodeId> seen;
    if (has_path(graph, dest, source, seen))
        refuse(Code::IllegalCycle, "illegal instantaneous cycle", "insert a feedback delay",
               graph.node(dest)->name);
}
void wire(SignalGraph& graph, const PortHandle& source, const PortHandle& dest) {
    if (source.kind != dest.kind)
        refuse(Code::KindMismatch,
               "port kind mismatch: '" + source.name + "' to '" + dest.name + "'",
               "connect ports with the same kind", graph.node(dest.node)->name, dest.name);
    const auto r = graph.connect(source, dest);
    if (!r) {
        Code code = Code::InvalidDocument;
        switch (r.code) {
        case GraphResult::Code::Ok:
            break;
        case GraphResult::Code::UnknownNode:
            code = Code::UnknownNode;
            break;
        case GraphResult::Code::UnknownPort:
            code = Code::UnknownPort;
            break;
        case GraphResult::Code::KindMismatch:
            code = Code::KindMismatch;
            break;
        case GraphResult::Code::IllegalCycle:
            code = Code::IllegalCycle;
            break;
        case GraphResult::Code::Duplicate:
            code = Code::Duplicate;
            break;
        }
        refuse(code, r.rule, r.suggested_fix, graph.node(dest.node)->name, dest.name);
    }
}
const state::ParamInfo& parameter(const SignalGraph& graph, NodeId id, const std::string& name) {
    const auto instance = graph.processor_node_instance(id);
    if (instance) {
        const state::ParamInfo* found = nullptr;
        for (const auto& p : instance->parameter_catalog()) {
            if (p.name != name)
                continue;
            if (found)
                refuse(Code::UnknownPort, "ambiguous parameter '" + name + "'",
                       "register unique parameter names", graph.node(id)->name, name);
            found = &p;
        }
        if (found)
            return *found;
    }
    if (graph.node(id)->type == NodeType::Gain && name == "gain")
        refuse(Code::UnknownPort,
               "built-in pulp/gain exposes 'gain' as a node setting, not an automatable parameter",
               "set it with " + graph.node(id)->name +
                   ".gain = <value>, or automate a registered Processor parameter",
               graph.node(id)->name, name);
    if (!instance)
        refuse(Code::UnknownPort,
               "node '" + graph.node(id)->name + "' has no registered Processor parameters",
               "automation targets only parameters of a registered Processor node; register one "
               "with GraphPatchCatalog",
               graph.node(id)->name, name);
    refuse(Code::UnknownPort, "unknown parameter '" + name + "'",
           "use a parameter name from the catalog", graph.node(id)->name, name);
}
void set_value(SignalGraph& graph, NodeId id, const std::string& name, float value) {
    if (graph.node(id)->type == NodeType::Gain && name == "gain") {
        graph.set_node_gain(id, value);
        return;
    }
    const auto& p = parameter(graph, id, name);
    if (value < p.range.min || value > p.range.max)
        refuse(Code::ParameterOutOfRange, "parameter '" + name + "' is outside its catalog range",
               "use a value in [" + number(p.range.min) + ", " + number(p.range.max) + "]",
               graph.node(id)->name, name);
    auto instance = graph.processor_node_instance(id);
    instance->state_store().set_value(p.id, value);
    if (instance->state_store().get_value(p.id) != value)
        refuse(Code::ParameterOutOfRange,
               "parameter value is not representable by its declared range/step",
               "use an exact catalog value", graph.node(id)->name, name);
}
void modulate(SignalGraph& graph, const PortHandle& source, NodeId dest, const std::string& param,
              float depth) {
    const auto& p = parameter(graph, dest, param);
    if (source.kind != PortKind::Audio && source.kind != PortKind::Control)
        refuse(Code::KindMismatch, "modulation source is not audio/control",
               "use an audio/control output", graph.node(source.node)->name, source.name);
    if (depth < 0 || depth > 1)
        refuse(Code::ParameterOutOfRange, "modulation depth is outside [0, 1]",
               "use a depth in [0, 1]", graph.node(dest)->name, param);
    check_cycle(graph, source.node, dest);
    if (!graph.connect_automation(source.node, source.index, dest, p.id, p.range.min,
                                  p.range.min + depth * (p.range.max - p.range.min)))
        refuse(Code::UnsupportedContract,
               "parameter does not accept sparse automation or route is duplicate",
               "use a continuous control-rate writable parameter and one Replace route",
               graph.node(dest)->name, param);
}
using Factory = NodeId (*)(SignalGraph&, const std::string&);
// These are runtime constructors, not copies of their type/port vocabulary.
const Factory builtins[] = {
    +[](SignalGraph& g, const std::string& n) { return g.add_input_node(2, n); },
    +[](SignalGraph& g, const std::string& n) { return g.add_output_node(2, n); },
    +[](SignalGraph& g, const std::string& n) { return g.add_gain_node(n); },
    +[](SignalGraph& g, const std::string& n) { return g.add_midi_input_node(n); },
    +[](SignalGraph& g, const std::string& n) { return g.add_midi_output_node(n); }};
std::string serialized_type(const SignalGraph& g) {
    const auto text = GraphSerializer::to_json(g);
    if (text.empty())
        refuse(Code::UnsupportedContract, "node cannot be serialized",
               "use a persistable registered node");
    return std::string(choc::json::parse(text)["nodes"][0]["type"].getString());
}
std::string type_key(const SignalGraph& g, const GraphNode& n) {
    if (auto p = g.processor_node_instance(n.id))
        return std::string(p->graph_type_id());
    if (n.type == NodeType::Custom)
        return n.custom_type_id;
    const auto document = choc::json::parse(GraphSerializer::to_json(g));
    for (const auto node : document["nodes"])
        if (node["id"].getInt64() == n.id)
            return "pulp/" + std::string(node["type"].getString());
    refuse(Code::UnsupportedContract, "node cannot be serialized",
           "use a persistable registered node", n.name);
}
std::int64_t integer(const choc::value::ValueView& object, const char* key) {
    const auto value = object[key];
    if (!value.isInt())
        refuse(Code::InvalidDocument, std::string(key) + " must be an integer",
               "use the serialized graph schema");
    return value.getInt64();
}
bool flag(const choc::value::ValueView& object, const char* key) {
    if (!object.hasObjectMember(key))
        return false;
    if (!object[key].isBool())
        refuse(Code::InvalidDocument, std::string(key) + " must be boolean", "use true or false");
    return object[key].getBool();
}
float real(const choc::value::ValueView& object, const char* key) {
    const auto v = object[key];
    if (v.isInt())
        return numeric(std::to_string(v.getInt64()));
    if (v.isFloat32() || v.isFloat64())
        return numeric(choc::json::toString(v));
    refuse(Code::InvalidDocument, std::string(key) + " must be numeric", "use a finite number");
}
} // namespace

std::string GraphPatchDiagnostic::message() const {
    return (line ? "line " + std::to_string(line) + ": " : "") + rule +
           (suggested_fix.empty() ? "" : "; " + suggested_fix);
}
const char* graph_patch_code_name(Code code) noexcept {
    switch (code) {
#define PATCH_CODE(x)                                                                              \
    case Code::x:                                                                                  \
        return #x
        PATCH_CODE(Ok);
        PATCH_CODE(UnknownNode);
        PATCH_CODE(UnknownPort);
        PATCH_CODE(KindMismatch);
        PATCH_CODE(IllegalCycle);
        PATCH_CODE(Duplicate);
        PATCH_CODE(SyntaxError);
        PATCH_CODE(InvalidDocument);
        PATCH_CODE(UnsupportedContract);
        PATCH_CODE(ParameterOutOfRange);
        PATCH_CODE(RestoreFailed);
        PATCH_CODE(ResourceLimit);
        PATCH_CODE(FileUnreadable);
#undef PATCH_CODE
    }
    return "InvalidDocument";
}
std::string graph_patch_diagnostic_json(const GraphPatchDiagnostic& d) {
    auto value = choc::value::createObject("GraphPatchDiagnostic");
    value.addMember("valid", bool(d));
    value.addMember("code", graph_patch_code_name(d.code));
    value.addMember("line", static_cast<std::int64_t>(d.line));
    value.addMember("node", d.node);
    value.addMember("port", d.port);
    value.addMember("rule", d.rule);
    value.addMember("suggested_fix", d.suggested_fix);
    value.addMember("message", d.message());
    return choc::json::toString(value);
}
ProcessorRestoreCode GraphPatchCatalog::register_processor(ProcessorRestoreDescriptor descriptor) {
    for (const auto& t : custom_)
        if (t.type_id == descriptor.type.type_id && t.version == descriptor.type.type_version)
            return ProcessorRestoreCode::DuplicateProcessorType;
    const auto result = processors_.register_type(descriptor);
    if (result == ProcessorRestoreCode::Restored)
        descriptors_.push_back(std::move(descriptor));
    return result;
}
bool GraphPatchCatalog::register_custom(CustomNodeType type) {
    for (const auto& d : descriptors_)
        if (d.type.type_id == type.type_id && d.type.type_version == type.version)
            return false;
    SignalGraph probe;
    if (type.type_id.starts_with("pulp/") || !probe.register_custom_node_type(type))
        return false;
    for (const auto& t : custom_)
        if (t.type_id == type.type_id && t.version == type.version)
            return false;
    custom_.push_back(std::move(type));
    return true;
}
std::unique_ptr<SignalGraph> GraphPatchCatalog::make_graph() const {
    auto g = std::make_unique<SignalGraph>();
    for (const auto& t : custom_)
        g->register_custom_node_type(t);
    return g;
}
NodeId GraphPatchCatalog::instantiate(SignalGraph& g, std::string_view key, int version,
                                      const std::string& name) const {
    if (version != 1 && key.starts_with("pulp/"))
        return 0;
    for (const auto factory : builtins) {
        SignalGraph probe;
        factory(probe, "probe");
        if ("pulp/" + serialized_type(probe) == key)
            return factory(g, name);
    }
    for (const auto& d : descriptors_) {
        if (d.type.type_id != key || d.type.type_version != version)
            continue;
        auto seed = d.create();
        if (!seed || seed.use_count() != 1)
            return 0;
        const auto parameters = seed->state_store().serialize();
        const auto custom = seed->processor().serialize_plugin_state();
        seed.reset();
        const ProcessorRestoreRecord record{
            1,          d.type, d.state_schema_version, d.input_channels, d.output_channels,
            parameters, custom};
        const auto restored =
            processors_.restore_batch(std::span(&record, 1), std::span(&d.type, 1));
        if (!restored.ok())
            return 0;
        return g.add_processor_node(restored.processors.front().instance, name);
    }
    return g.add_custom_node(key, version, name);
}
GraphLoadOptions GraphPatchCatalog::load_options() const {
    GraphLoadOptions options;
    options.load_plugins = false;
    options.processor_registry = &processors_;
    for (const auto& d : descriptors_)
        options.processor_allowlist.push_back(d.type);
    return options;
}
std::string GraphPatchCatalog::to_json() const {
    auto root = choc::value::createObject("GraphPatchCatalog");
    root.addMember("schema", "pulp.graph-patch-catalog.v1");
    auto rows = choc::value::createEmptyArray();
    auto add = [&](const SignalGraph& g, NodeId id, const std::string& key, int version) {
        auto row = choc::value::createObject("Type");
        row.addMember("type_id", key);
        row.addMember("version", version);
        for (bool output : {false, true}) {
            auto list = choc::value::createEmptyArray();
            for (const auto& p : ports(g, id, output)) {
                auto v = choc::value::createObject("Port");
                v.addMember("name", p.name);
                const char* kind = "audio";
                switch (p.kind) {
                case PortKind::Audio:
                    break;
                case PortKind::Sidechain:
                    kind = "sidechain";
                    break;
                case PortKind::Midi:
                    kind = "midi";
                    break;
                case PortKind::Event:
                    kind = "event";
                    break;
                case PortKind::Control:
                    kind = "control";
                    break;
                case PortKind::Param:
                    kind = "parameter";
                    break;
                case PortKind::Spectral:
                    kind = "spectral";
                    break;
                }
                v.addMember("kind", kind);
                list.addArrayElement(v);
            }
            row.addMember(output ? "outputs" : "inputs", list);
        }
        auto parameters = choc::value::createEmptyArray();
        if (auto p = g.processor_node_instance(id)) {
            for (const auto& info : p->parameter_catalog()) {
                auto v = choc::value::createObject("Parameter");
                v.addMember("name", info.name);
                v.addMember("min", double(info.range.min));
                v.addMember("max", double(info.range.max));
                v.addMember("default", double(info.range.default_value));
                v.addMember("id", static_cast<std::int64_t>(info.id));
                v.addMember("step", double(info.range.step));
                v.addMember("unit", info.unit);
                v.addMember("read_only", state::is_read_only_param(info));
                v.addMember("automatable", state::is_automatable_param(info));
                v.addMember("rate",
                            info.rate == state::ParamRate::ControlRate ? "control" : "audio");
                auto labels = choc::value::createEmptyArray();
                for (const auto& label : info.value_labels)
                    labels.addArrayElement(label);
                v.addMember("value_labels", labels);
                parameters.addArrayElement(v);
            }
        } else if (g.node(id)->type == NodeType::Gain) {
            auto v = choc::value::createObject("Parameter");
            v.addMember("name", "gain");
            v.addMember("default", double(g.node(id)->gain));
            parameters.addArrayElement(v);
        }
        row.addMember("parameters", parameters);
        const auto processor = g.processor_node_instance(id);
        const auto* custom_type = g.node(id)->type == NodeType::Custom
                                      ? g.custom_node_type(g.node(id)->custom_type_id, version)
                                      : nullptr;
        const bool opaque =
            (processor && !processor->processor().serialize_plugin_state().empty()) ||
            (custom_type && bool(custom_type->save_state));
        row.addMember("notation_state", opaque ? "unsupported_opaque_state" : "parameters_only");
        rows.addArrayElement(row);
    };
    for (const auto factory : builtins) {
        SignalGraph g;
        const auto id = factory(g, "probe");
        add(g, id, "pulp/" + serialized_type(g), 1);
    }
    for (const auto& d : descriptors_) {
        auto g = make_graph();
        auto id = instantiate(*g, d.type.type_id, d.type.type_version, "probe");
        if (id)
            add(*g, id, d.type.type_id, d.type.type_version);
    }
    for (const auto& t : custom_) {
        auto g = make_graph();
        auto id = g->add_custom_node(t.type_id, t.version, "probe");
        if (id)
            add(*g, id, t.type_id, t.version);
    }
    root.addMember("types", rows);
    return choc::json::toString(root);
}

GraphPatchResult parse_graph_patch(std::string_view text, const GraphPatchCatalog& catalog) {
    std::size_t line_number = 0;
    try {
        if (text.size() > 1024 * 1024)
            refuse(Code::ResourceLimit, "notation exceeds 1 MiB",
                   "split the patch into smaller graphs");
        auto graph = catalog.make_graph();
        std::map<std::string, NodeId> names;
        auto find_node = [&](const std::string& name) {
            const auto i = names.find(name);
            if (i == names.end())
                refuse(Code::UnknownNode, "unknown node '" + name + "'",
                       "declare the node before using it", name);
            return i->second;
        };
        static const std::regex declaration(patch_grammar::declaration);
        static const std::regex local_name("[A-Za-z_][A-Za-z0-9_]*");
        static const std::regex setting(patch_grammar::setting);
        bool header = false;
        std::istringstream input{std::string(text)};
        std::string raw;
        while (std::getline(input, raw)) {
            ++line_number;
            if (raw.size() > 16384)
                refuse(Code::ResourceLimit, "line exceeds 16384 bytes", "shorten the line");
            const auto original = trim(raw);
            if (original.starts_with("# flow ")) {
                if (header || !names.empty() || original != "# flow 1")
                    refuse(Code::SyntaxError, "unsupported or misplaced flow version",
                           "start with exactly # flow 1");
                header = true;
                continue;
            }
            const auto line = split(raw, "#").front();
            if (line.empty())
                continue;
            if (!header)
                refuse(Code::SyntaxError, "missing notation version", "start with # flow 1");
            const auto declarations = split(line, ":");
            if (declarations.size() == 2) {
                const auto& name = declarations[0];
                if (!std::regex_match(name, local_name))
                    refuse(Code::SyntaxError, "invalid node name", "use an ASCII identifier", name);
                if (names.contains(name))
                    refuse(Code::Duplicate, "node '" + name + "' is declared twice",
                           "use a unique node name", name);
                if (names.size() >= 128)
                    refuse(Code::ResourceLimit, "more than 128 nodes", "split the patch");
                const auto typed = split(declarations[1], "@");
                if (typed.size() > 2)
                    refuse(Code::SyntaxError, "invalid type version", "use type @ integer");
                int version = 1;
                if (typed.size() == 2) {
                    const auto& v = typed[1];
                    auto r = std::from_chars(v.data(), v.data() + v.size(), version);
                    if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || version < 1)
                        refuse(Code::SyntaxError, "invalid type version",
                               "use a positive integer version");
                }
                if (typed[0].empty())
                    refuse(Code::SyntaxError, "missing type key", "use a type from the catalog");
                const auto key = unquote(typed[0]);
                std::smatch match;
                if (typed[0].front() != '"' && typed[0].front() != '\'' &&
                    !std::regex_match(name + " : " + key, declaration))
                    refuse(Code::SyntaxError, "invalid type key",
                           "use Plugin/Module or quote the exact catalog type id");
                const auto id = catalog.instantiate(*graph, key, version, name);
                if (!id)
                    refuse(Code::UnsupportedContract, "unregistered type/version '" + key + "'",
                           "register the exact Processor or CustomNodeType from the catalog", name);
                const auto* node = graph->node(id);
                const auto* custom =
                    node->type == NodeType::Custom
                        ? graph->custom_node_type(node->custom_type_id, node->custom_type_version)
                        : nullptr;
                const auto processor = graph->processor_node_instance(id);
                if ((custom && custom->save_state) ||
                    (processor && !processor->processor().serialize_plugin_state().empty()))
                    refuse(Code::UnsupportedContract, "opaque node state has no notation contract",
                           "use GraphSerializer for this registered state contract", name);
                names.emplace(name, id);
                continue;
            }
            if (declarations.size() > 2)
                refuse(Code::SyntaxError, "invalid declaration", "use name : Plugin/Module");
            const auto modulation = split(line, "~>");
            if (modulation.size() == 2) {
                const auto depth = split(modulation[1], "@");
                if (depth.size() != 2)
                    refuse(Code::SyntaxError, "modulation needs a depth",
                           "use source.port ~> destination.parameter @ depth");
                const auto src = endpoint(modulation[0]), dst = endpoint(depth[0]);
                modulate(*graph, resolve_port(*graph, find_node(src.node), src.port, true),
                         find_node(dst.node), dst.port, numeric(depth[1]));
            } else if (modulation.size() > 2) {
                refuse(Code::SyntaxError, "modulation cannot be chained",
                       "use one modulation route per line");
            } else if (const auto hops = split(line, ">>"); hops.size() > 1) {
                auto src = endpoint(hops.front());
                for (std::size_t h = 1; h < hops.size(); ++h) {
                    const auto destinations = split(hops[h], ",");
                    if (destinations.size() > 1 && h + 1 != hops.size())
                        refuse(Code::SyntaxError, "fanout must end a chain",
                               "place fanout only at the final hop");
                    for (const auto& target : destinations) {
                        const auto dst = endpoint(target);
                        wire(*graph, resolve_port(*graph, find_node(src.node), src.port, true),
                             resolve_port(*graph, find_node(dst.node), dst.port, false));
                    }
                    src = endpoint(destinations.front());
                    src.port.clear();
                }
            } else {
                std::smatch m;
                if (!std::regex_match(line, m, setting))
                    refuse(Code::SyntaxError, "cannot read '" + line + "'",
                           "use a declaration, edge or numeric parameter setting");
                const auto target = endpoint(trim(line.substr(0, line.find('='))));
                set_value(*graph, find_node(target.node), target.port, numeric(m[5].str()));
            }
            if (graph->connections().size() > 512)
                refuse(Code::ResourceLimit, "more than 512 connections", "split the patch");
        }
        if (!header)
            refuse(Code::SyntaxError, "missing notation version", "start with # flow 1");
        return {{}, std::move(graph)};
    } catch (Refusal& e) {
        if (!e.diagnostic.line)
            e.diagnostic.line = line_number;
        return {std::move(e.diagnostic), {}};
    } catch (const std::exception& e) {
        return {{Code::InvalidDocument,
                 line_number,
                 {},
                 {},
                 e.what(),
                 "use the catalog and patch-language grammar"},
                {}};
    }
}

GraphPatchText format_graph_patch(const SignalGraph& graph, const GraphPatchCatalog& catalog) {
    try {
        if (!graph.sample_regions().empty())
            refuse(Code::UnsupportedContract, "sample regions have no notation contract",
                   "use the sample-region API and GraphSerializer");
        std::string declarations = "# flow 1\n", wiring, tuning;
        std::map<NodeId, std::string> names;
        std::set<std::string> unique;
        static const std::regex identifier("[A-Za-z_][A-Za-z0-9_]*");
        auto rows = graph.nodes();
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        for (const auto& n : rows) {
            if (!std::regex_match(n.name, identifier) || !unique.insert(n.name).second)
                refuse(Code::UnsupportedContract, "node names must be unique ASCII identifiers",
                       "rename nodes before formatting", n.name);
            names.emplace(n.id, n.name);
            if (n.type == NodeType::AudioInput && n.num_output_ports != 2)
                refuse(Code::UnsupportedContract, "notation input boundary is stereo",
                       "use the saved graph API for other channel layouts", n.name);
            if (n.type == NodeType::AudioOutput && n.num_input_ports != 2)
                refuse(Code::UnsupportedContract, "notation output boundary is stereo",
                       "use the saved graph API for other channel layouts", n.name);
            if (n.type == NodeType::Plugin && !graph.is_processor_node(n.id))
                refuse(Code::UnsupportedContract,
                       "hosted plugin identity/state is not a registered Processor",
                       "use GraphSerializer for hosted plugin state", n.name);
            if (n.spectral_identity ||
                (n.type == NodeType::Custom && !graph.custom_node_state(n.id).empty()))
                refuse(Code::UnsupportedContract,
                       "spectral or opaque custom state has no notation contract",
                       "use GraphSerializer and the owning typed API", n.name);
            const auto key = type_key(graph, n);
            int version = n.type == NodeType::Custom ? n.custom_type_version : 1;
            if (auto p = graph.processor_node_instance(n.id)) {
                if (!p->processor().serialize_plugin_state().empty())
                    refuse(Code::UnsupportedContract,
                           "opaque Processor state has no notation contract",
                           "use GraphSerializer for opaque Processor state", n.name);
                version = p->graph_type_version();
                for (const auto& info : p->parameter_catalog())
                    tuning += n.name + "." + quote(info.name) + " = " +
                              number(p->state_store().get_value(info.id)) + "\n";
            } else if (n.type == NodeType::Gain)
                tuning += n.name + ".gain = " + number(n.gain) + "\n";
            if (n.type != NodeType::Gain && n.gain != 1.0f)
                refuse(Code::UnsupportedContract, "non-Gain node scalar has no notation spelling",
                       "use GraphSerializer to retain this scalar", n.name);
            auto probe = catalog.make_graph();
            const auto probe_id = catalog.instantiate(*probe, key, version, n.name);
            if (!probe_id)
                refuse(Code::UnsupportedContract, "type is not in the supplied catalog",
                       "register the exact type before formatting", n.name);
            if (probe->node(probe_id)->num_input_ports != n.num_input_ports ||
                probe->node(probe_id)->num_output_ports != n.num_output_ports)
                refuse(Code::UnsupportedContract, "node shape differs from catalog",
                       "use the matching registered shape", n.name);
            declarations += n.name + " : " + quote(key) +
                            (version == 1 ? "" : " @ " + std::to_string(version)) + "\n";
        }
        std::vector<std::string> edge_lines;
        for (const auto& c : graph.connections()) {
            if (c.control_signal)
                refuse(Code::UnsupportedContract, "Control edge has no notation spelling",
                       "use GraphSerializer (format v5) to save Control edges");
            if (c.feedback || c.sidechain || c.spectral || c.audio_rate_modulation)
                refuse(Code::UnsupportedContract, "edge contract has no lossless notation spelling",
                       "use GraphSerializer for feedback, sidechain, spectral or dense modulation");
            if (c.midi) {
                if (!midi_capable(graph, c.source_node, true) ||
                    !midi_capable(graph, c.dest_node, false))
                    refuse(Code::KindMismatch, "MIDI route lacks declared capabilities",
                           "connect declared MIDI endpoints");
                auto midi_port = [&](NodeId id, bool output) {
                    for (const auto& p : ports(graph, id, output))
                        if (p.kind == PortKind::Midi)
                            return quote(p.name);
                    return std::string{};
                };
                edge_lines.push_back(
                    names.at(c.source_node) + "." + midi_port(c.source_node, true) + " >> " +
                    names.at(c.dest_node) + "." + midi_port(c.dest_node, false) + "\n");
                continue;
            }
            auto index_port = [&](NodeId id, PortIndex index, bool output) {
                for (const auto& p : ports(graph, id, output))
                    if (p.index == index && p.kind != PortKind::Midi)
                        return p.name;
                refuse(Code::UnknownPort, "connection port index is unavailable",
                       "connect an existing catalog port", names.at(id));
            };
            const auto src = names.at(c.source_node) + "." +
                             quote(index_port(c.source_node, c.source_port, true));
            if (c.automation) {
                const auto instance = graph.processor_node_instance(c.dest_node);
                const auto* p = instance ? instance->parameter(c.automation_param_id) : nullptr;
                if (!p || c.automation_mix != AutomationMix::Replace ||
                    c.automation_smoothing_ms != 0 || c.automation_range_lo != p->range.min ||
                    p->range.max <= p->range.min || c.automation_range_hi < p->range.min ||
                    c.automation_range_hi > p->range.max)
                    refuse(Code::UnsupportedContract,
                           "automation range/mix/smoothing has no lossless depth spelling",
                           "use one unsmoothed Replace route anchored at range.min");
                const auto depth =
                    (c.automation_range_hi - p->range.min) / (p->range.max - p->range.min);
                if (p->range.min + depth * (p->range.max - p->range.min) != c.automation_range_hi)
                    refuse(Code::UnsupportedContract,
                           "automation depth would round away range precision",
                           "use GraphSerializer to preserve exact range bytes");
                edge_lines.push_back(src + " ~> " + names.at(c.dest_node) + "." + quote(p->name) +
                                     " @ " + number(depth) + "\n");
            } else
                edge_lines.push_back(src + " >> " + names.at(c.dest_node) + "." +
                                     quote(index_port(c.dest_node, c.dest_port, false)) + "\n");
        }
        std::sort(edge_lines.begin(), edge_lines.end());
        for (const auto& line : edge_lines)
            wiring += line;
        const auto text = declarations + wiring + tuning;
        // Refuse graphs outside the parser's bounds and capabilities, including
        // a serialized instantaneous cycle or a value a StateStore had clamped.
        auto checked = parse_graph_patch(text, catalog);
        if (!checked)
            return {checked.diagnostic, {}};
        return {{}, text};
    } catch (const Refusal& e) {
        return {e.diagnostic, {}};
    } catch (const std::exception& e) {
        return {
            {Code::InvalidDocument, 0, {}, {}, e.what(), "use a fully connected registered graph"},
            {}};
    }
}

GraphPatchResult load_graph_patch_json(std::string_view json, const GraphPatchCatalog& catalog) {
    try {
        if (json.size() > GraphSerializer::max_file_bytes)
            refuse(Code::ResourceLimit, "graph exceeds file limit", "use a smaller graph");
        auto root = choc::json::parse(std::string(json));
        if (!root.isObject() || !root["nodes"].isArray() || !root["connections"].isArray())
            refuse(Code::InvalidDocument, "graph requires nodes and connections arrays",
                   "use GraphSerializer's schema");
        if (root["nodes"].size() > 128 || root["connections"].size() > 512)
            refuse(Code::ResourceLimit, "strict profile admits 128 nodes and 512 edges",
                   "split the graph");
        if (root.hasObjectMember("sample_regions") && root["sample_regions"].size())
            refuse(Code::UnsupportedContract,
                   "sample-region definitions require their typed validation authority",
                   "use the sample-region API");
        const auto connections = choc::value::Value(root["connections"]);
        std::map<std::int64_t, std::string> old_ids;
        std::set<std::string> names;
        for (const auto n : root["nodes"]) {
            const auto id = integer(n, "id");
            if (id <= 0 || id > std::numeric_limits<NodeId>::max() || !n["name"].isString())
                refuse(Code::InvalidDocument, "invalid node identity",
                       "use a positive NodeId and a unique name");
            const std::string name(n["name"].getString());
            if (!old_ids.emplace(id, name).second || !names.insert(name).second)
                refuse(Code::Duplicate, "duplicate node identity", "use unique node IDs and names",
                       name);
            for (const auto* key : {"num_input_ports", "num_output_ports"}) {
                auto count = integer(n, key);
                if (count < 0 || count > 64)
                    refuse(Code::ResourceLimit, "port count is outside [0, 64]",
                           "use a bounded port count", name);
            }
            if (n.hasObjectMember("gain"))
                (void)real(n, "gain");
            if (n["type"].isString()) {
                const auto type = n["type"].getString();
                if (type == "plugin" || type.starts_with("spectral"))
                    refuse(Code::UnsupportedContract,
                           "node type requires a separate identity/state contract",
                           "use registered Processor or CustomNodeType nodes", name);
            }
        }
        root.setMember("connections", choc::value::createEmptyArray());
        auto graph = catalog.make_graph();
        const auto loaded =
            GraphSerializer::from_json(*graph, choc::json::toString(root), catalog.load_options());
        if (!loaded.ok)
            refuse(Code::RestoreFailed, loaded.error,
                   "register exact allowed versions and supply valid serialized state");
        if (!loaded.missing_plugins.empty() || !loaded.missing_custom_node_types.empty() ||
            !loaded.state_restore_failures.empty())
            refuse(Code::UnsupportedContract, "graph contains unresolved node/state contracts",
                   "register exact types and restore all state before validating");
        std::map<std::int64_t, NodeId> ids;
        for (const auto& n : graph->nodes()) {
            if (n.type == NodeType::Custom && n.custom_state_pending)
                refuse(Code::UnsupportedContract,
                       "custom state validation is deferred until prepare",
                       "use GraphSerializer and the owning custom-state API", n.name);
            for (const auto& [old, name] : old_ids)
                if (n.name == name)
                    ids.emplace(old, n.id);
        }
        if (ids.size() != old_ids.size())
            refuse(Code::UnsupportedContract, "loader did not retain every node",
                   "use supported exact node contracts");
        for (const auto edge : connections) {
            const auto source = ids.find(integer(edge, "source_node")),
                       dest = ids.find(integer(edge, "dest_node"));
            if (source == ids.end() || dest == ids.end())
                refuse(Code::UnknownNode, "connection endpoint is not declared",
                       "declare both endpoints or remove the dangling edge");
            const auto sp = integer(edge, "source_port"), dp = integer(edge, "dest_port");
            if (sp < 0 || dp < 0 || sp > std::numeric_limits<PortIndex>::max() ||
                dp > std::numeric_limits<PortIndex>::max())
                refuse(Code::UnknownPort, "connection port is outside the index domain",
                       "use a port index from the catalog");
            const bool midi = flag(edge, "midi"), automation = flag(edge, "automation"),
                       dense = flag(edge, "audio_rate_modulation"),
                       feedback = flag(edge, "feedback"), sidechain = flag(edge, "sidechain"),
                       spectral = flag(edge, "spectral");
            if (int(midi) + int(automation) + int(dense) + int(feedback) + int(sidechain) +
                    int(spectral) >
                1)
                refuse(Code::KindMismatch, "connection has conflicting edge kinds",
                       "select exactly one edge kind");
            if (feedback || sidechain || dense || spectral)
                refuse(Code::UnsupportedContract,
                       "edge requires a separate strict validation contract",
                       "use the owning typed API for this edge kind");
            auto by_index = [&](NodeId id, std::int64_t index, bool output) {
                for (const auto& p : ports(*graph, id, output))
                    if (p.index == index && ((p.kind == PortKind::Midi) == midi))
                        return p;
                refuse(Code::UnknownPort, "connection index has no port of the declared kind",
                       "use the catalog port index and edge kind", graph->node(id)->name);
            };
            const auto source_port = by_index(source->second, sp, true);
            if (automation) {
                const auto param_id = integer(edge, "auto_param_id");
                const auto instance = graph->processor_node_instance(dest->second);
                const auto* p = instance && param_id >= 0 &&
                                        param_id <= std::numeric_limits<state::ParamID>::max()
                                    ? instance->parameter(static_cast<state::ParamID>(param_id))
                                    : nullptr;
                if (!p)
                    refuse(Code::UnknownPort, "automation parameter is unavailable",
                           "use a parameter from the destination catalog",
                           graph->node(dest->second)->name);
                const auto lo = real(edge, "auto_range_lo"), hi = real(edge, "auto_range_hi"),
                           smoothing = real(edge, "auto_smoothing");
                if (lo < p->range.min || lo > p->range.max || hi < p->range.min ||
                    hi > p->range.max || smoothing < 0)
                    refuse(Code::ParameterOutOfRange, "automation range is outside the catalog",
                           "use the declared parameter range and nonnegative smoothing",
                           graph->node(dest->second)->name, p->name);
                const auto mix = integer(edge, "auto_mix");
                if (mix < 0 || mix > 1)
                    refuse(Code::InvalidDocument, "unknown automation mix", "use Replace or Add");
                check_cycle(*graph, source->second, dest->second);
                if (!graph->connect_automation(source->second, static_cast<PortIndex>(sp),
                                               dest->second, p->id, lo, hi, smoothing,
                                               static_cast<AutomationMix>(mix)))
                    refuse(Code::UnsupportedContract, "automation route refused",
                           "use a writable control-rate parameter and nonconflicting route");
            } else
                wire(*graph, source_port, by_index(dest->second, dp, false));
        }
        if (graph->connections().size() != connections.size())
            refuse(Code::InvalidDocument, "strict restore lost connections",
                   "repair every original connection");
        return {{}, std::move(graph)};
    } catch (const Refusal& e) {
        return {e.diagnostic, {}};
    } catch (const std::exception& e) {
        return {{Code::InvalidDocument, 0, {}, {}, e.what(), "use GraphSerializer's schema"}, {}};
    }
}
GraphPatchResult load_graph_patch_file(const std::filesystem::path& path,
                                       const GraphPatchCatalog& catalog) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {{Code::FileUnreadable,
                 0,
                 {},
                 {},
                 "cannot read graph file",
                 "use a readable regular file"},
                {}};
    const auto limit = path.extension() == ".pat" ? 1024 * 1024 : GraphSerializer::max_file_bytes;
    std::string text(limit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.size() > limit || file.bad())
        return {{Code::ResourceLimit,
                 0,
                 {},
                 {},
                 "graph file exceeds bounds or read failed",
                 "use a bounded readable file"},
                {}};
    return path.extension() == ".pat" ? parse_graph_patch(text, catalog)
                                      : load_graph_patch_json(text, catalog);
}
} // namespace pulp::host
