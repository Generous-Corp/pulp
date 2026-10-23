#include "ship_bundle_strip.hpp"

#include "cli_common.hpp"

#include <pulp/ship/codesign.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <system_error>

namespace pulp::cli {

namespace {

std::string trim_line_end(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

} // namespace

bool is_macho_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::array<unsigned char, 4> b{};
    if (!in.read(reinterpret_cast<char*>(b.data()), b.size()))
        return false;
    const std::uint32_t big = (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) |
                              (std::uint32_t{b[2]} << 8) | std::uint32_t{b[3]};
    switch (big) {
    case 0xfeedfaceu:
    case 0xfeedfacfu: // big-endian thin
    case 0xcefaedfeu:
    case 0xcffaedfeu: // little-endian thin, as read big-endian
    case 0xcafebabeu:
    case 0xcafebabfu: // universal
        return true;
    default:
        return false;
    }
}

std::string bundle_executable_name(const fs::path& bundle) {
    const auto plist = bundle / "Contents" / "Info.plist";
    std::error_code ec;
    if (fs::is_regular_file(plist, ec)) {
        auto name =
            trim_line_end(exec_output("/usr/bin/plutil -extract CFBundleExecutable raw -o - " +
                                      shell_quote(plist) + " 2>/dev/null"));
        if (!name.empty() && name.find('/') == std::string::npos)
            return name;
    }
    return bundle.stem().string();
}

std::optional<fs::path> stage_stripped_bundle(const fs::path& bundle, const fs::path& staging_root,
                                              const ShipStripOptions& options) {
    const auto exec_name = bundle_executable_name(bundle);
    if (!is_macho_file(bundle / "Contents" / "MacOS" / exec_name))
        return bundle;

    // An ad hoc signature carries no Authority line, so an identity here means
    // the bundle went through `pulp ship sign` and its copy must be signed the
    // same way to stay notarizable.
    const auto signing = pulp::ship::check_codesign(bundle.string());
    const bool developer_signed = signing.is_signed && !signing.identity.empty();
    if (developer_signed && options.identity.empty()) {
        std::cerr << "  " << bundle.filename().string()
                  << ": shipping with local symbols; it is signed but no signing "
                     "identity is configured to re-sign a stripped copy "
                     "(pass --identity or set signing.apple.identity)\n";
        return bundle;
    }

    const auto staged = staging_root / bundle.parent_path().filename() / bundle.filename();
    std::error_code ec;
    fs::remove_all(staged, ec);
    fs::create_directories(staged.parent_path(), ec);
    if (ec || run("/usr/bin/ditto " + shell_quote(bundle) + " " + shell_quote(staged)) != 0) {
        std::cerr << "  FAILED to stage " << bundle.filename().string() << " for stripping\n";
        return std::nullopt;
    }
    const auto binary = staged / "Contents" / "MacOS" / exec_name;
    // The dSYM comes from the unstripped executable, so it keeps every symbol
    // strip removes, under the UUID the stripped copy still carries.
    if (!options.symbols_dir.empty()) {
        const auto dsym = options.symbols_dir / bundle.parent_path().filename() /
                          (bundle.filename().string() + ".dSYM");
        fs::remove_all(dsym, ec);
        fs::create_directories(dsym.parent_path(), ec);
        if (ec ||
            run("/usr/bin/xcrun dsymutil " +
                shell_quote(bundle / "Contents" / "MacOS" / exec_name) + " -o " +
                shell_quote(dsym) + " >/dev/null 2>&1") != 0 ||
            !fs::is_directory(dsym)) {
            std::cerr << "  FAILED to write a dSYM for " << bundle.filename().string()
                      << " before stripping it\n";
            return std::nullopt;
        }
    }
    // strip warns that the existing signature is invalidated; the copy is
    // re-signed below.
    if (run("/usr/bin/strip -x " + shell_quote(binary) + " 2>/dev/null") != 0) {
        std::cerr << "  FAILED to strip local symbols from " << bundle.filename().string() << "\n";
        return std::nullopt;
    }
    // Re-signing replaces the signature, entitlements included, so carry the
    // original's entitlements over unless the caller named a file.
    std::string entitlements = options.entitlements;
    if (entitlements.empty()) {
        const auto extracted =
            staged.parent_path() / (bundle.filename().string() + ".entitlements");
        fs::remove(extracted, ec);
        if (run("/usr/bin/codesign -d --entitlements " + shell_quote(extracted) + " --xml " +
                shell_quote(bundle) + " >/dev/null 2>&1") == 0 &&
            fs::is_regular_file(extracted, ec) && fs::file_size(extracted, ec) > 0)
            entitlements = extracted.string();
    }
    if (developer_signed) {
        if (options.signing_preflight && !options.signing_preflight())
            return std::nullopt;
        if (!pulp::ship::codesign(staged.string(), options.identity, entitlements)) {
            std::cerr << "  FAILED to re-sign the stripped copy of " << bundle.filename().string()
                      << "\n";
            return std::nullopt;
        }
    } else if (run("/usr/bin/codesign --force --sign - " +
                   (entitlements.empty() ? std::string()
                                         : "--entitlements " + shell_quote(entitlements) + " ") +
                   shell_quote(staged) + " >/dev/null 2>&1") != 0) {
        std::cerr << "  FAILED to ad hoc sign the stripped copy of " << bundle.filename().string()
                  << "\n";
        return std::nullopt;
    }
    std::cout << "  stripped local symbols from " << bundle.filename().string()
              << " (shipped copy; the build output keeps them";
    if (!options.symbols_dir.empty())
        std::cout << "; dSYM in "
                  << (options.symbols_dir / bundle.parent_path().filename()).string();
    std::cout << ")\n";
    return staged;
}

} // namespace pulp::cli
