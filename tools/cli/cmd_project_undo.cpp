// cmd_project_undo.cpp — `pulp project undo` (roadmap item P11-1).
//
//   pulp project undo                     Revert the newest bump batch
//   pulp project undo <timestamp>         Revert a specific batch
//
// Reads `~/.pulp/bump-undo-<timestamp>.json` written by a previous
// `pulp project bump` and rewrites each pin back to its pre-bump
// value, skipping any project whose pin drifted since.

#include "cmd_project_internal.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace pb = pulp::cli::project_bump;

namespace pulp_cli::project_detail {

// ── Dispatch: undo ──────────────────────────────────────────────────────────

int do_undo(const std::vector<std::string>& args) {
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) {
        print_undo_help();
        return 0;
    }

    auto home = pulp_home();
    if (home.empty()) {
        std::cerr << "pulp project undo: could not determine pulp home (HOME / USERPROFILE unset)\n";
        return 1;
    }

    fs::path target;
    if (!args.empty()) {
        auto stamp = args[0];
        target = pb::undo_batch_path(home, stamp);
        if (!fs::exists(target)) {
            std::cerr << "pulp project undo: no batch at " << target.string() << "\n";
            return 1;
        }
    } else {
        auto batches = pb::list_undo_batches(home);
        if (batches.empty()) {
            std::cerr << "pulp project undo: no bump batches on disk under " << home.string() << "\n";
            return 1;
        }
        target = batches.front();
    }

    auto batch = pb::read_undo_batch(target);
    if (!batch) {
        std::cerr << "pulp project undo: could not parse " << target.string() << "\n";
        return 1;
    }

    std::cout << color::bold() << "Reverting bump batch "
              << batch->timestamp << " (target was "
              << batch->target_version << ")" << color::reset() << "\n";

    int reverted = 0, skipped = 0, failed = 0;
    for (const auto& e : batch->entries) {
        if (e.status != "bumped") {
            ++skipped;
            continue;
        }

        if (e.edits.empty()) {
            std::cerr << "  " << color::yellow() << "skipped" << color::reset()
                      << " " << e.project_name << "  (undo batch has no edits)\n";
            ++skipped;
            continue;
        }

        std::map<fs::path, std::string> staged;
        std::map<fs::path, std::string> originals;
        std::string skip_reason;
        std::string failure_reason;

        for (const auto& edit : e.edits) {
            if (edit.path.empty() || !fs::exists(edit.path)) {
                failure_reason = "missing " + edit.path.string();
                break;
            }

            auto it = staged.find(edit.path);
            if (it == staged.end()) {
                auto source = read_text(edit.path);
                if (source.empty()) {
                    failure_reason = "could not read " + edit.path.string();
                    break;
                }
                it = staged.emplace(edit.path, std::move(source)).first;
            }

            auto site = site_for_kind(it->second, edit.kind);
            if (site.kind != edit.kind) {
                skip_reason = "pin kind changed since bump";
                break;
            }

            bool current_matches = false;
            if (!edit.new_value.empty()) {
                if (site.current_pin == edit.new_value) {
                    current_matches = true;
                } else {
                    auto have = pb::normalize_pin(site.current_pin);
                    auto want = pb::normalize_pin(edit.new_value);
                    current_matches = !have.empty() && have == want;
                }
            } else if (edit.kind != pb::PinKind::PulpTomlSdkPath) {
                current_matches = pb::normalize_pin(site.current_pin) ==
                                  batch->target_version;
            }
            if (!current_matches) {
                skip_reason = "current value no longer matches bumped value";
                break;
            }

            auto restored_value = edit.old_value;
            auto normalized_old = pb::normalize_pin(edit.old_value);
            if (!normalized_old.empty()) restored_value = normalized_old;
            auto restored = pb::rewrite_pin(it->second, site,
                                            restored_value,
                                            edit.old_value_style_has_v);
            if (!restored) {
                failure_reason = "rewrite failed";
                break;
            }
            it->second = *restored;
        }

        if (!failure_reason.empty()) {
            std::cerr << "  " << color::red() << "failed" << color::reset()
                      << " " << e.project_name << "  (" << failure_reason << ")\n";
            ++failed;
            continue;
        }
        if (!skip_reason.empty()) {
            std::cerr << "  " << color::yellow() << "skipped" << color::reset()
                      << " " << e.project_name << "  (" << skip_reason << ")\n";
            ++skipped;
            continue;
        }

        for (const auto& [path, body] : staged) {
            originals[path] = read_text(path);
        }
        std::vector<fs::path> written;
        for (const auto& [path, body] : staged) {
            if (!write_text_atomic(path, body)) {
                for (const auto& wrote : written) {
                    (void)write_text_atomic(wrote, originals[wrote]);
                }
                failure_reason = "write failed";
                break;
            }
            written.push_back(path);
        }
        if (!failure_reason.empty()) {
            std::cerr << "  " << color::red() << "failed" << color::reset()
                      << " " << e.project_name << "  (" << failure_reason << ")\n";
            ++failed;
            continue;
        }

        std::cout << "  " << color::green() << "reverted" << color::reset()
                  << " " << e.project_name
                  << "  " << batch->target_version
                  << " -> " << fmt_pin(e.old_pin, e.old_pin_style_has_v) << "\n";
        ++reverted;
    }

    std::cout << "\nSummary: " << reverted << " reverted, "
              << skipped << " skipped, " << failed << " failed\n";

    if (failed == 0) {
        std::error_code ec;
        fs::remove(target, ec);
        std::cout << "Removed undo file " << target.string() << "\n";
    } else {
        std::cout << "Undo file retained (" << target.string()
                  << ") — inspect failures and retry.\n";
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace pulp_cli::project_detail
