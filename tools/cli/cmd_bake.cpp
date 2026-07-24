// pulp bake — freeze a .pulpgraph into a signed, distributable .pulpbake artifact,
// and verify one. The signed on-disk codec is an SDK feature; this is its
// command-line front door. Key handling reuses the reload-trust key_store (the same
// KeyMaterial / --sign-key file the ship swap-pack path uses) — never hand-rolled.

#include "cli_common.hpp"

#include <pulp/format/reload/key_store.hpp>
#include <pulp/host/baked_codec.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/base64.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace reload = pulp::format::reload;

const char* lower_reason_str(pulp::host::LowerRejectReason r) {
    using R = pulp::host::LowerRejectReason;
    switch (r) {
        case R::None: return "none";
        case R::NotPrepared: return "graph not prepared";
        case R::NotExecutorEligible: return "not executor-eligible (outside the lowerable subset)";
        case R::HostedPluginNotSelfContained: return "contains a hosted Plugin node (not self-contained)";
        case R::CustomNotYetLowerable: return "a Custom node is unresolved or shape-mismatched";
        case R::CustomNotLowerable: return "a Custom type is not opted into baking (lowerable=false)";
        case R::CustomTransportNotLowerable: return "a transport-sensitive Custom node";
        case R::NonAudioLaneNotLowerable: return "a MIDI/automation/sidechain node or edge";
        case R::CodecRejected: return "codec/verify rejected";
        case R::StatefulCustomNotYetLoadable:
            return "a stateful Custom node could not be restored";
    }
    return "unknown";
}

std::optional<std::string> read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<std::vector<std::uint8_t>> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// Load a public key from a --trust file (a KeyMaterial blob; we take its public half).
std::optional<std::vector<std::uint8_t>> trust_pubkey_from_file(const fs::path& p) {
    auto blob = read_text(p);
    if (!blob) return std::nullopt;
    auto km = reload::parse_key_blob(*blob);
    if (!km || km->public_key.empty()) return std::nullopt;
    return km->public_key;
}

int bake_verify(const std::vector<std::string>& args) {
    std::string artifact;
    std::vector<std::string> trust_files;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--trust" && i + 1 < args.size()) {
            trust_files.push_back(args[++i]);
        } else if (artifact.empty() && !args[i].empty() && args[i][0] != '-') {
            artifact = args[i];
        } else {
            std::cerr << "pulp bake verify: unexpected argument '" << args[i] << "'\n";
            return 2;
        }
    }
    if (artifact.empty()) {
        std::cerr << "usage: pulp bake verify <artifact.pulpbake> --trust <pubkey-file> [...]\n";
        return 2;
    }
    auto bytes = read_bytes(artifact);
    if (!bytes) {
        std::cerr << "pulp bake verify: cannot read '" << artifact << "'\n";
        return 1;
    }
    pulp::host::BakedTrust trust;
    for (const auto& f : trust_files) {
        auto pk = trust_pubkey_from_file(f);
        if (!pk) {
            std::cerr << "pulp bake verify: cannot read a trusted public key from '" << f << "'\n";
            return 2;
        }
        trust.trusted_public_keys.push_back(std::move(*pk));
    }
    // Pure verify: signature + bounded parse only. Never executes or loads state.
    auto plan = pulp::host::verify_and_extract_plan(*bytes, trust);
    if (!plan) {
        std::cout << "REJECTED: signature/trust/parse check failed for '" << artifact << "'\n";
        return 1;
    }
    std::cout << "ACCEPTED: '" << artifact << "'  bus=" << plan->input_channels << "in/"
              << plan->output_channels << "out  nodes=" << plan->nodes.size()
              << "  connections=" << plan->connections.size() << "\n";
    return 0;
}

int bake_write(const std::vector<std::string>& args) {
    std::string input, output, sign_key;
    double sr = 48000.0;
    int block = 512;
    bool force = false;
    bool no_mint = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-o" || a == "--out") {
            if (i + 1 >= args.size()) { std::cerr << "pulp bake: " << a << " needs a value\n"; return 2; }
            output = args[++i];
        } else if (a == "--sign-key") {
            if (i + 1 >= args.size()) { std::cerr << "pulp bake: --sign-key needs a value\n"; return 2; }
            sign_key = args[++i];
        } else if (a == "--sr") {
            if (i + 1 >= args.size()) { std::cerr << "pulp bake: --sr needs a value\n"; return 2; }
            try {
                sr = std::stod(args[++i]);
            } catch (const std::exception&) {
                std::cerr << "pulp bake: --sr expects a number, got '" << args[i] << "'\n";
                return 2;
            }
            if (sr <= 0.0) { std::cerr << "pulp bake: --sr must be positive\n"; return 2; }
        } else if (a == "--block") {
            if (i + 1 >= args.size()) { std::cerr << "pulp bake: --block needs a value\n"; return 2; }
            try {
                block = std::stoi(args[++i]);
            } catch (const std::exception&) {
                std::cerr << "pulp bake: --block expects an integer, got '" << args[i] << "'\n";
                return 2;
            }
            if (block <= 0) { std::cerr << "pulp bake: --block must be positive\n"; return 2; }
        } else if (a == "--force") {
            force = true;
        } else if (a == "--no-mint") {
            no_mint = true;
        } else if (!a.empty() && a[0] != '-' && input.empty()) {
            input = a;
        } else {
            std::cerr << "pulp bake: unexpected argument '" << a << "'\n";
            return 2;
        }
    }
    if (input.empty() || output.empty() || sign_key.empty()) {
        std::cerr << "usage: pulp bake <input.pulpgraph> -o <out.pulpbake> --sign-key <key-file> "
                     "[--sr 48000] [--block 512] [--force] [--no-mint]\n";
        return 2;
    }
    if (fs::exists(output) && !force) {
        std::cerr << "pulp bake: '" << output << "' exists; pass --force to overwrite\n";
        return 1;
    }
    // --no-mint: for CI / non-interactive use, refuse to silently mint a NEW signing
    // identity when the key is absent (which would otherwise exit 0 with only a stderr
    // note — an automation footgun for a distributable, signed artifact).
    if (no_mint && !fs::exists(sign_key)) {
        std::cerr << "pulp bake: --no-mint set but no signing key at '" << sign_key
                  << "'. Provide an existing key (or drop --no-mint to mint one).\n";
        return 2;
    }

    auto json = read_text(input);
    if (!json) { std::cerr << "pulp bake: cannot read '" << input << "'\n"; return 1; }

    pulp::host::SignalGraph graph;
    const auto load = pulp::host::GraphSerializer::from_json(graph, *json);
    if (!load.ok) {
        std::cerr << "pulp bake: failed to load graph: " << load.error << "\n";
        return 1;
    }
    graph.set_canonical_executor_routing_enabled(true);
    if (!graph.prepare(sr, block)) {
        std::cerr << "pulp bake: graph.prepare(" << sr << ", " << block << ") failed\n";
        return 1;
    }
    const auto plan = pulp::host::bake_to_plan(graph);
    if (!plan.accepted) {
        std::cerr << "pulp bake: graph is not bakeable — " << lower_reason_str(plan.reason);
        if (plan.offending_node != 0) std::cerr << " (node " << plan.offending_node << ")";
        std::cerr << "\n";
        return 1;
    }

    // Reuse the reload-trust key file (KeyMaterial): load an existing key or mint one
    // at the path. `minted` guards the footgun — a typo'd --sign-key path would
    // otherwise silently sign the artifact under a brand-new identity, so we say so
    // loudly (mirrors ship swap-pack's "a fresh key screams" rule).
    bool minted = false;
    auto km = reload::load_or_generate_key_file(sign_key, minted);
    if (!km || !km->valid()) {
        std::cerr << "pulp bake: cannot load/create the signing key at '" << sign_key << "'\n";
        return 1;
    }
    if (minted) {
        std::cerr << "pulp bake: NOTE — no key existed at '" << sign_key
                  << "'; minted a NEW signing key there. If that path was a typo, delete it and "
                     "re-run with your intended key — this artifact is signed under the new identity.\n";
    }
    const auto bytes = pulp::host::write_baked_signed(
        *plan.plan, {km->private_key.data(), km->private_key.size()});
    if (bytes.empty()) {
        std::cerr << "pulp bake: signing failed (bad key or oversized plan)\n";
        return 1;
    }
    // Write to a temp path then atomically rename, so a mid-write failure never leaves
    // a truncated .pulpbake behind the output name (which would then block the next
    // non-force bake with a confusing "exists").
    const fs::path final_out = output;
    const fs::path tmp_out = fs::path(output + ".tmp");
    {
        std::ofstream out(tmp_out, std::ios::binary | std::ios::trunc);
        if (!out) { std::cerr << "pulp bake: cannot write '" << tmp_out.string() << "'\n"; return 1; }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            std::cerr << "pulp bake: write to '" << tmp_out.string() << "' failed\n";
            std::error_code rmec;
            fs::remove(tmp_out, rmec);
            return 1;
        }
    }
    std::error_code renec;
    fs::rename(tmp_out, final_out, renec);
    if (renec) {
        std::cerr << "pulp bake: could not finalize '" << output << "': " << renec.message() << "\n";
        std::error_code rmec;
        fs::remove(tmp_out, rmec);
        return 1;
    }

    const std::string pub_b64 =
        pulp::runtime::base64_encode(km->public_key.data(), km->public_key.size());
    std::cout << "baked '" << input << "' → '" << output << "' (" << bytes.size()
              << " bytes)\n  signer public key: " << pub_b64 << "\n";
    return 0;
}

}  // namespace

int cmd_bake(const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "verify") return bake_verify(args);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::cout << "pulp bake — freeze a graph into a signed .pulpbake artifact\n\n"
                     "  pulp bake <input.pulpgraph> -o <out.pulpbake> --sign-key <key-file>\n"
                     "            [--sr 48000] [--block 512] [--force]\n"
                     "  pulp bake verify <artifact.pulpbake> --trust <pubkey-file> [...]\n";
        return args.empty() ? 2 : 0;
    }
    return bake_write(args);
}
