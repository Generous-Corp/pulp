#include "shell_quote.hpp"

#include <cstddef>

std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    // pulp #776: on Windows, MSVCRT argv parsing treats backslashes
    // literally unless they immediately precede a double quote. Unix-
    // style escaping (`\` -> `\\`) breaks file paths: `git clone
    // "C:\\Users\\foo\\origin.git" feature` writes the doubled-backslash
    // URL into feature/.git/config; the next `git fetch origin main`
    // looks up a path that doesn't exist, fails, and `bump_one` skips
    // the origin/main redundancy gate it relied on for `"skipped"`.
    //
    // Implementation follows the canonical MSVCRT-correct algorithm
    // (Microsoft docs, Daniel Colascione's "Everyone quotes command
    // line arguments the wrong way"):
    //
    //   - Backslashes NOT followed by `"` are literal.
    //   - To embed a literal `"`, prefix with one extra backslash and
    //     double every backslash already in the run before it.
    //   - Trailing backslashes before the closing `"` must be doubled
    //     so they don't escape it.
    //
    // cmd.exe's own metacharacters (`&`, `|`, `<`, `>`, `%`, `!`, `^`)
    // are inert inside `"..."`, so wrapping is enough to neutralize them.
    std::string out = "\"";
    int backslashes = 0;
    for (char c : s) {
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            out.append(static_cast<std::size_t>(backslashes) * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(static_cast<std::size_t>(backslashes), '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(static_cast<std::size_t>(backslashes) * 2, '\\');
    out += "\"";
    return out;
#else
    std::string out = "\"";
    for (char c : s) {
        // Inside POSIX double quotes, backslash, quote, dollar, and backtick
        // retain special meaning unless escaped.
        if (c == '\\' || c == '"' || c == '$' || c == '`') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
#endif
}

std::string shell_quote(const std::filesystem::path& p) {
    return shell_quote(p.string());
}
