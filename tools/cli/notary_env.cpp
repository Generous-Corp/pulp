// notary_env.cpp — App Store Connect notary credentials env-file parser
//
// See notary_env.hpp for the public contract. Implementation is a
// straightforward hand-rolled bash-`.env` subset parser; we considered
// CHOC's text utilities (`choc::text::trim`, `splitIntoLines`) but
// keeping this file dependency-free lets the test binary link without
// the full runtime + skia chain.

#include "notary_env.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace pulp::cli::notary {

namespace {

std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Expand $HOME / ${HOME}. We deliberately support only HOME — keeping
// the surface tiny means there are no other `$VAR` semantics to get
// wrong (the user-facing env file is hand-typed, not a full shell
// script).
std::string expand_home(const std::string& s, const std::string& home) {
    if (home.empty()) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '$') {
            // ${HOME}
            if (i + 6 < s.size() && s.compare(i, 7, "${HOME}") == 0) {
                out += home;
                i += 7;
                continue;
            }
            // $HOME (followed by non-alphanumeric/underscore, or EOL)
            if (i + 4 < s.size() && s.compare(i, 5, "$HOME") == 0) {
                char next = (i + 5 < s.size()) ? s[i + 5] : '\0';
                if (next == '\0' || next == '/' || next == ' ' || next == '\t'
                    || next == '\n' || next == ':' || next == '"' || next == '\'') {
                    out += home;
                    i += 5;
                    continue;
                }
            }
            if (i + 4 == s.size() && s.compare(i, 5, "$HOME") == 0) {
                out += home;
                i += 5;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Parse one `KEY=VALUE` line. Returns {empty,empty} on a comment, blank
// line, or malformed input. Caller filters those out.
std::pair<std::string, std::string> parse_line(const std::string& raw,
                                                const std::string& home) {
    std::string line = trim(raw);
    if (line.empty()) return {};
    if (line[0] == '#') return {};
    // Strip optional leading `export `
    if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));

    auto eq = line.find('=');
    if (eq == std::string::npos) return {};
    std::string key = trim(line.substr(0, eq));
    std::string value = line.substr(eq + 1);

    // Validate key shape: ASCII identifier chars only. Anything else
    // (e.g. a markdown list bullet) → skip silently.
    if (key.empty()) return {};
    for (char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return {};
        }
    }

    // Three value forms:
    //   "double quoted"  → strip quotes, expand $HOME
    //   'single quoted'  → strip quotes, NO expansion (literal)
    //   bare             → strip inline `# comment`, trim, expand $HOME
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        value = expand_home(value, home);
    } else if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        value = value.substr(1, value.size() - 2);
        // No expansion for single quotes (matches bash semantics).
    } else {
        // Strip a trailing inline comment: `value  # comment`. We only
        // recognize ` #` (space + hash) so that values containing `#`
        // (e.g. URL fragments) still parse — same heuristic as
        // python-dotenv.
        auto hash = value.find(" #");
        if (hash != std::string::npos) value = trim(value.substr(0, hash));
        value = expand_home(value, home);
    }
    return {key, value};
}

} // anonymous namespace

NotaryEnvFile parse_env_text(const std::string& text,
                              const std::string& home) {
    NotaryEnvFile out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto [k, v] = parse_line(line, home);
        if (k.empty()) continue;
        out.raw[k] = v;
        if (k == "PULP_NOTARY_KEY_PATH")  out.key_path = v;
        else if (k == "PULP_NOTARY_KEY_ID")    out.key_id = v;
        else if (k == "PULP_NOTARY_ISSUER_ID") out.issuer_id = v;
        else if (k == "PULP_SIGN_IDENTITY")    out.sign_identity = v;
        else if (k == "PULP_TEAM_ID")          out.team_id = v;
    }
    return out;
}

NotaryEnvFile parse_env_file(const std::filesystem::path& path,
                              const std::string& home) {
    std::ifstream in(path);
    if (!in.good()) return {};
    std::ostringstream buf;
    buf << in.rdbuf();
    return parse_env_text(buf.str(), home);
}

std::filesystem::path default_env_path() {
    if (const char* override = std::getenv("PULP_NOTARY_ENV"); override && *override)
        return std::filesystem::path(override);
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home) / ".config" / "pulp" / "secrets" / "notary.env";
}

ResolvedNotaryCreds resolve_creds(
    const std::string& cli_key_path,
    const std::string& cli_key_id,
    const std::string& cli_issuer_id,
    const NotaryEnvFile& file,
    const std::function<std::optional<std::string>(const std::string&)>& getenv) {
    ResolvedNotaryCreds out;

    auto layer = [&](const std::string& cli,
                     const std::string& env_name,
                     const std::string& from_file,
                     std::string& dst,
                     std::string& src) {
        if (!cli.empty()) { dst = cli; src = "cli"; return; }
        if (getenv) {
            if (auto e = getenv(env_name); e && !e->empty()) {
                dst = *e; src = "env"; return;
            }
        }
        if (!from_file.empty()) { dst = from_file; src = "file"; return; }
        dst.clear(); src.clear();
    };

    layer(cli_key_path,  "PULP_NOTARY_KEY_PATH",  file.key_path,  out.key_path,  out.key_path_source);
    layer(cli_key_id,    "PULP_NOTARY_KEY_ID",    file.key_id,    out.key_id,    out.key_id_source);
    layer(cli_issuer_id, "PULP_NOTARY_ISSUER_ID", file.issuer_id, out.issuer_id, out.issuer_id_source);
    return out;
}

std::string redact_path(const std::string& path) {
    if (path.size() <= 16) return path;
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size())
        return "…/" + path.substr(path.size() - 16);
    return "…/" + path.substr(slash + 1);
}

} // namespace pulp::cli::notary
