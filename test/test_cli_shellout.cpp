// Shell-out CLI behaviour tests for `pulp`.
//
// Per CLAUDE.md: "CLI behavior changes — shell out to the built binary,
// assert exit code + stderr content." Before this file, the CLI had
// two targeted unit tests (test_cli_create_targets, test_cli_design_binding)
// but no end-to-end shell-out validation — the lesson from #295 was
// that silent-failure bugs slip through when no test actually launches
// the binary. This closes that gap with deterministic exit-code +
// stdout/stderr invariants for the non-destructive subcommands every
// user hits on day one.

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

namespace {

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const fs::path& next)
        : previous_(fs::current_path()) {
        fs::current_path(next);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        fs::current_path(previous_, ec);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    fs::path previous_;
};

std::string repo_project_version_for_shellout_tests() {
    const auto repo_root = fs::weakly_canonical(fs::current_path() / ".." / "..");
    auto cmake = read_file(repo_root / "CMakeLists.txt");
    auto project = cmake.find("project(Pulp");
    REQUIRE(project != std::string::npos);
    const std::string needle = "VERSION ";
    auto pos = cmake.find(needle, project);
    REQUIRE(pos != std::string::npos);
    pos += needle.size();
    auto end = cmake.find_first_of(" \r\n\t)", pos);
    REQUIRE(end != std::string::npos);
    return cmake.substr(pos, end - pos);
}

fs::path write_version_project_fixture(const std::string& prefix,
                                       const std::string& project_version,
                                       const std::string& plugin_version = "1.2.3",
                                       const std::string& marketplace_version = "1.2.3",
                                       const std::string& marketplace_plugin_version = "1.2.3",
                                       const std::string& changelog_version = "1.2.3",
                                       bool hardcoded_au_version = false) {
    auto root = unique_temp_dir(prefix);
    write_text(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\n"
               "project(VersionFixture VERSION " + project_version + ")\n"
               "pulp_add_plugin(Fixture VERSION \"" + plugin_version + "\")\n");
    fs::create_directories(root / "core");
    write_text(root / "CHANGELOG.md", "## [" + changelog_version + "]\n\nfixture\n");
    write_text(root / "tools" / "cmake" / "PulpInfoPlist.au.in",
               hardcoded_au_version
                   ? "<plist><integer>65536</integer></plist>\n"
                   : "<plist><integer>@PULP_BUNDLE_VERSION_INT@</integer></plist>\n");
    write_text(root / ".claude-plugin" / "plugin.json",
               "{\n  \"name\": \"pulp\",\n  \"version\": \"" + plugin_version + "\"\n}\n");
    write_text(root / ".claude-plugin" / "marketplace.json",
               "{\n"
               "  \"version\": \"" + marketplace_version + "\",\n"
               "  \"plugins\": [{\"name\": \"pulp\", \"version\": \"" +
                   marketplace_plugin_version + "\"}]\n"
               "}\n");
    return root;
}

ProcessResult run_pulp_in_directory(const fs::path& dir,
                                    const std::vector<std::string>& args,
                                    int timeout_ms = 10000) {
    const auto bin = fs::absolute(pulp_binary());
    REQUIRE(fs::exists(bin));
    ScopedCurrentPath cwd(dir);
    return exec(bin.string(), args, timeout_ms);
}

}  // namespace

TEST_CASE("pulp help exits 0 with a usage banner on stdout",
          "[cli][shellout]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }
    auto r = run_pulp({"help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Must advertise the word "pulp" and list the build subcommand so
    // first-time users aren't dropped into an empty help output — this
    // is the category of regression that the #295 silent-failure
    // lesson warned about.
    REQUIRE(r.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(r.stdout_output.find("build") != std::string::npos);
}

TEST_CASE("pulp --help is an alias for pulp help",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto a = run_pulp({"help"});
    auto b = run_pulp({"--help"});
    auto c = run_pulp({"-h"});
    // Every alias exits 0 and reaches the same top-level usage.
    REQUIRE(a.exit_code == 0);
    REQUIRE(b.exit_code == 0);
    REQUIRE(c.exit_code == 0);
    REQUIRE(a.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(b.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(c.stdout_output.find("pulp") != std::string::npos);
}

TEST_CASE("pulp with no arguments prints help and exits 0",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({});
    // Bare `pulp` is expected to show usage rather than error out — it
    // mustn't fake-succeed silently or hang.
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE((r.stdout_output.find("pulp") != std::string::npos ||
             r.stdout_output.find("Usage") != std::string::npos ||
             r.stdout_output.find("Commands") != std::string::npos));
}

TEST_CASE("pulp <unknown-command> exits non-zero with a diagnostic",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"thisisnotarealcommand"});
    REQUIRE(r.exit_code != 0);
    REQUIRE_FALSE(r.timed_out);
    // At least one of stdout/stderr must reference the unknown command
    // or a help pointer — otherwise the user is left guessing.
    auto combined = r.stdout_output + r.stderr_output;
    bool mentioned =
        combined.find("Unknown") != std::string::npos ||
        combined.find("unknown") != std::string::npos ||
        combined.find("help") != std::string::npos;
    REQUIRE(mentioned);
}

TEST_CASE("pulp audio usage and parser errors are deterministic",
          "[cli][shellout][audio][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto usage = run_pulp({"audio"});
    REQUIRE(usage.exit_code == 0);
    REQUIRE_FALSE(usage.timed_out);
    REQUIRE(usage.stdout_output.find("pulp audio") != std::string::npos);
    REQUIRE(usage.stdout_output.find("excerpt-find") != std::string::npos);
    REQUIRE(usage.stdout_output.find("read-bundle") != std::string::npos);

    struct Case {
        std::vector<std::string> args;
        std::string stderr_substring;
        std::string stdout_substring;
    };

    const std::vector<Case> cases = {
        {{"audio", "model"}, "Unknown audio model subcommand", "pulp audio"},
        {{"audio", "model", "activate"}, "model id is required", "pulp audio"},
        {{"audio", "model", "list", "--surprise"}, "Unknown option: --surprise", ""},
        {{"audio", "model", "status", "--surprise"}, "Unknown option: --surprise", ""},
        {{"audio", "excerpt-find", "--text"}, "--text requires a value", ""},
        {{"audio", "excerpt-find", "--text", "kick", "--input"}, "--input requires a value", ""},
        {{"audio", "excerpt-find", "--model"}, "--model requires a value", ""},
        {{"audio", "excerpt-find", "--top", "many"}, "invalid value for --top", ""},
        {{"audio", "excerpt-find", "--window-ms", "soon"}, "invalid value for --window-ms", ""},
        {{"audio", "excerpt-find", "--hop-ms", "soon"}, "invalid value for --hop-ms", ""},
        {{"audio", "excerpt-find", "--min-score", "loud"}, "invalid value for --min-score", ""},
        {{"audio", "excerpt-find", "--max-candidates-per-file", "lots"}, "invalid value for --max-candidates-per-file", ""},
        {{"audio", "excerpt-find", "--bundle-out"}, "--bundle-out requires a value", ""},
        {{"audio", "excerpt-find", "--unknown"}, "Unknown option: --unknown", ""},
        {{"audio", "read-bundle"}, "bundle path is required", "pulp audio"},
        {{"audio", "read-bundle", "bundle-a", "bundle-b"}, "Unknown argument: bundle-b", ""},
        {{"audio", "not-audio"}, "Unknown audio subcommand", "pulp audio"},
    };

    for (const auto& c : cases) {
        INFO("args size: " << c.args.size());
        auto r = run_pulp(c.args);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code != 0);
        REQUIRE(r.stderr_output.find(c.stderr_substring) != std::string::npos);
        if (!c.stdout_substring.empty())
            REQUIRE(r.stdout_output.find(c.stdout_substring) != std::string::npos);
    }
}

TEST_CASE("pulp audio read-bundle json reports missing bundle errors",
          "[cli][shellout][audio][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto missing = unique_temp_dir("pulp-audio-missing-bundle");
    fs::remove_all(missing);
    auto r = run_pulp({"audio", "read-bundle", missing.string(), "--json"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.empty());
    REQUIRE(r.stdout_output.find("\"ok\": false") != std::string::npos);
    REQUIRE(r.stdout_output.find("bundle path does not exist") != std::string::npos);
}

TEST_CASE("pulp audio model commands report install, activation, and status state",
          "[cli][shellout][audio][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-audio-model-shellout-home");
    ScopedEnvVar scoped_pulp_home("PULP_HOME");
    scoped_pulp_home.set(home.string());

    auto checkpoint = home / "models" / "clap.pt";
    write_text(checkpoint, "stub checkpoint");
    write_text(home / "audio" / "models" / "clap_music_audioset_v1.json",
               "{\n"
               "  \"model_id\": \"clap_music_audioset_v1\",\n"
               "  \"backend\": \"clap\",\n"
               "  \"checkpoint_ref\": \"hf://lukewys/laion_clap/music.pt\",\n"
               "  \"resolved_checkpoint_path\": \"" + checkpoint.generic_string() + "\"\n"
               "}\n");

    auto list_json = run_pulp({"audio", "model", "list", "--json"});
    REQUIRE_FALSE(list_json.timed_out);
    REQUIRE(list_json.exit_code == 0);
    REQUIRE(list_json.stderr_output.empty());
    REQUIRE(list_json.stdout_output.find("\"active_model_id\": \"\"") != std::string::npos);
    REQUIRE(list_json.stdout_output.find("\"model_id\": \"clap_music_audioset_v1\"") != std::string::npos);
    REQUIRE(list_json.stdout_output.find("\"status\": \"installed\"") != std::string::npos);
    REQUIRE(list_json.stdout_output.find("\"active\": false") != std::string::npos);

    auto status_json = run_pulp({"audio", "model", "status", "--json"});
    REQUIRE_FALSE(status_json.timed_out);
    REQUIRE(status_json.exit_code == 0);
    REQUIRE(status_json.stdout_output.find("\"state_file_found\": false") != std::string::npos);
    REQUIRE(status_json.stdout_output.find("no configured audio model") != std::string::npos);

    auto unknown = run_pulp({"audio", "model", "activate", "not_a_model", "--json"});
    REQUIRE_FALSE(unknown.timed_out);
    REQUIRE(unknown.exit_code != 0);
    REQUIRE(unknown.stderr_output.empty());
    REQUIRE(unknown.stdout_output.find("\"ok\": false") != std::string::npos);
    REQUIRE(unknown.stdout_output.find("unknown model_id: not_a_model") != std::string::npos);

    auto activate = run_pulp({"audio", "model", "activate", "clap_music_audioset_v1", "--json"});
    REQUIRE_FALSE(activate.timed_out);
    REQUIRE(activate.exit_code == 0);
    REQUIRE(activate.stderr_output.empty());
    REQUIRE(activate.stdout_output.find("\"ok\": true") != std::string::npos);
    REQUIRE(activate.stdout_output.find("\"active_model_id\": \"clap_music_audioset_v1\"") != std::string::npos);
    REQUIRE(activate.stdout_output.find("\"backend\": \"clap\"") != std::string::npos);
    REQUIRE(fs::exists(home / "audio" / "model-state.json"));

    auto status_text = run_pulp({"audio", "model", "status"});
    REQUIRE_FALSE(status_text.timed_out);
    REQUIRE(status_text.exit_code == 0);
    REQUIRE(status_text.stdout_output.find("Audio Model Status") != std::string::npos);
    REQUIRE(status_text.stdout_output.find("Configured model: clap_music_audioset_v1") != std::string::npos);
    REQUIRE(status_text.stdout_output.find("Loadable: yes") != std::string::npos);
    REQUIRE(status_text.stdout_output.find("configured model is loadable") != std::string::npos);

    auto list_text = run_pulp({"audio", "model", "list"});
    REQUIRE_FALSE(list_text.timed_out);
    REQUIRE(list_text.exit_code == 0);
    REQUIRE(list_text.stdout_output.find("Audio Models") != std::string::npos);
    REQUIRE(list_text.stdout_output.find("Active: clap_music_audioset_v1") != std::string::npos);
    REQUIRE(list_text.stdout_output.find("* clap_music_audioset_v1 [installed] backend=clap") != std::string::npos);

    fs::remove_all(home);
}

TEST_CASE("pulp audio read-bundle reports manifest metadata in text and json",
          "[cli][shellout][audio][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto root = unique_temp_dir("pulp-audio-bundle-shellout");
    auto bundle = root / "bundle";
    fs::create_directories(bundle / "metadata");
    write_text(bundle / "manifest.json",
               "{\n"
               "  \"tool\": \"pulp audio excerpt-find\",\n"
               "  \"bundle_version\": 1,\n"
               "  \"model_file\": \"metadata/model.json\",\n"
               "  \"ranked_results_file\": \"metadata/results.json\",\n"
               "  \"requested_model_id\": \"clap_music_audioset_v1\",\n"
               "  \"loaded_model_id\": \"clap_music_audioset_v1\",\n"
               "  \"backend\": \"null\",\n"
               "  \"result_count\": 2\n"
               "}\n");
    write_text(bundle / "metadata" / "model.json",
               "{\n"
               "  \"model_id\": \"clap_music_audioset_v1\",\n"
               "  \"backend\": \"null\"\n"
               "}\n");
    write_text(bundle / "metadata" / "results.json",
               "{\n"
               "  \"results\": [\n"
               "    {\"rank\": 1, \"score\": 0.9, \"source_file\": \"kick.wav\","
               "     \"start_ms\": 10, \"end_ms\": 110, \"excerpt_file\": \"excerpts/rank-01.wav\"},\n"
               "    {\"rank\": 2, \"score\": 0.5, \"source_file\": \"snare.wav\","
               "     \"start_ms\": 20, \"end_ms\": 220, \"excerpt_file\": \"excerpts/rank-02.wav\"}\n"
               "  ]\n"
               "}\n");

    auto text = run_pulp({"audio", "read-bundle", bundle.string()});
    REQUIRE_FALSE(text.timed_out);
    REQUIRE(text.exit_code == 0);
    REQUIRE(text.stderr_output.empty());
    REQUIRE(text.stdout_output.find("Audio Excerpt Bundle") != std::string::npos);
    REQUIRE(text.stdout_output.find("Tool: pulp audio excerpt-find") != std::string::npos);
    REQUIRE(text.stdout_output.find("Requested model: clap_music_audioset_v1") != std::string::npos);
    REQUIRE(text.stdout_output.find("Loaded model: clap_music_audioset_v1") != std::string::npos);
    REQUIRE(text.stdout_output.find("Backend: null") != std::string::npos);
    REQUIRE(text.stdout_output.find("Results: 2") != std::string::npos);
    REQUIRE(text.stdout_output.find("#1 score=0.9000 source=kick.wav") != std::string::npos);
    REQUIRE(text.stdout_output.find("#2 score=0.5000 source=snare.wav") != std::string::npos);

    auto json = run_pulp({"audio", "read-bundle", bundle.string(), "--json"});
    REQUIRE_FALSE(json.timed_out);
    REQUIRE(json.exit_code == 0);
    REQUIRE(json.stderr_output.empty());
    REQUIRE(json.stdout_output.find("\"ok\": true") != std::string::npos);
    REQUIRE(json.stdout_output.find("\"result_count\": 2") != std::string::npos);
    REQUIRE(json.stdout_output.find("\"source_file\": \"kick.wav\"") != std::string::npos);
    REQUIRE(json.stdout_output.find("\"excerpt_file\": \"excerpts/rank-02.wav\"") != std::string::npos);

    fs::remove_all(root);
}

TEST_CASE("pulp audio excerpt-find surfaces service errors through text and json",
          "[cli][shellout][audio][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-audio-excerpt-shellout-home");
    ScopedEnvVar scoped_pulp_home("PULP_HOME");
    scoped_pulp_home.set(home.string());

    auto checkpoint = home / "models" / "clap.pt";
    write_text(checkpoint, "stub checkpoint");
    write_text(home / "audio" / "models" / "clap_music_audioset_v1.json",
               "{\n"
               "  \"model_id\": \"clap_music_audioset_v1\",\n"
               "  \"resolved_checkpoint_path\": \"" + checkpoint.generic_string() + "\"\n"
               "}\n");
    auto notes = home / "notes.txt";
    write_text(notes, "not a wav");

    auto missing_model = run_pulp({"audio", "excerpt-find",
                                   "--text", "texture",
                                   "--input", notes.string(),
                                   "--model", "not_a_model",
                                   "--json"});
    REQUIRE_FALSE(missing_model.timed_out);
    REQUIRE(missing_model.exit_code != 0);
    REQUIRE(missing_model.stderr_output.empty());
    REQUIRE(missing_model.stdout_output.find("\"ok\": false") != std::string::npos);
    REQUIRE(missing_model.stdout_output.find("unknown model_id: not_a_model") != std::string::npos);

    auto json = run_pulp({"audio", "excerpt-find",
                          "--text", "texture",
                          "--input", notes.string(),
                          "--model", "clap_music_audioset_v1",
                          "--json"});
    REQUIRE_FALSE(json.timed_out);
    REQUIRE(json.exit_code != 0);
    REQUIRE(json.stderr_output.empty());
    REQUIRE(json.stdout_output.find("\"ok\": false") != std::string::npos);
    REQUIRE(json.stdout_output.find("\"query\": \"texture\"") != std::string::npos);
    REQUIRE(json.stdout_output.find("\"scanned_file_count\": 0") != std::string::npos);
    REQUIRE(json.stdout_output.find("no supported WAV inputs found") != std::string::npos);
    REQUIRE(json.stdout_output.find("unsupported; WAV only") != std::string::npos);

    auto text = run_pulp({"audio", "excerpt-find",
                          "--text", "texture",
                          "--input", notes.string(),
                          "--model", "clap_music_audioset_v1"});
    REQUIRE_FALSE(text.timed_out);
    REQUIRE(text.exit_code != 0);
    REQUIRE(text.stdout_output.empty());
    REQUIRE(text.stderr_output.find("Error: no supported WAV inputs found") != std::string::npos);

    fs::remove_all(home);
}

TEST_CASE("pulp cache usage and parser errors are deterministic",
          "[cli][shellout][cache][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-cache-shellout-home");
    ScopedEnvVar scoped_pulp_home("PULP_HOME");
    scoped_pulp_home.set(home.string());

    auto help = run_pulp({"cache"});
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("pulp cache") != std::string::npos);
    REQUIRE(help.stdout_output.find("status") != std::string::npos);

    auto status = run_pulp({"cache", "status"});
    REQUIRE_FALSE(status.timed_out);
    REQUIRE(status.exit_code == 0);
    REQUIRE(status.stdout_output.find("Pulp Cache") != std::string::npos);
    REQUIRE(status.stdout_output.find(home.string()) != std::string::npos);

    struct Case {
        std::vector<std::string> args;
        int exit_code;
        std::string stderr_substring;
    };
    const std::vector<Case> cases = {
        {{"cache", "status", "extra"}, 2, "Unexpected cache status argument"},
        {{"cache", "fetch"}, 1, "Usage: pulp cache fetch <asset>"},
        {{"cache", "fetch", "fonts"}, 1, "Unknown asset: fonts"},
        {{"cache", "fetch", "skia", "extra"}, 2, "Unexpected cache fetch argument"},
        {{"cache", "clean", "extra"}, 2, "Unexpected cache clean argument"},
        {{"cache", "mystery"}, 1, "Unknown cache subcommand"},
    };

    for (const auto& c : cases) {
        INFO("cache args size: " << c.args.size());
        auto r = run_pulp(c.args);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == c.exit_code);
        REQUIRE(r.stderr_output.find(c.stderr_substring) != std::string::npos);
    }

    fs::create_directories(home / "cache");
    write_text(home / "cache" / "asset.bin", "cached");
    auto clean = run_pulp({"cache", "clean"});
    REQUIRE_FALSE(clean.timed_out);
    REQUIRE(clean.exit_code == 0);
    REQUIRE(clean.stdout_output.find("Cache cleared") != std::string::npos);
    REQUIRE_FALSE(fs::exists(home / "cache"));

    std::error_code ec;
    fs::remove_all(home, ec);
}

TEST_CASE("pulp config <unknown-subcommand> exits non-zero with a diagnostic",
          "[cli][shellout][codex-562]") {
    // Codex 2026-04-21 wave 2 P2 on #562: `pulp config foo` previously
    // fell through to usage() which returned 0, so scripts/CI could
    // not detect a typo'd subcommand. The new behaviour returns
    // exit code 2 with an "Unknown config subcommand" diagnostic on
    // stderr. Known subcommands (get/set/list/help) keep exit 0.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"config", "thisisnotarealsubcommand"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("Unknown config subcommand") != std::string::npos);
}

TEST_CASE("pulp config supports pr.workflow",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-pr-workflow-config");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto set = run_pulp({"config", "set", "pr.workflow", "github"});
    auto get = run_pulp({"config", "get", "pr.workflow"});
    auto list = run_pulp({"config", "list"});
    auto bad = run_pulp({"config", "set", "pr.workflow", "svn"});

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp_home);

    REQUIRE_FALSE(set.timed_out);
    REQUIRE(set.exit_code == 0);
    REQUIRE_FALSE(get.timed_out);
    REQUIRE(get.exit_code == 0);
    REQUIRE(get.stdout_output.find("github") != std::string::npos);
    REQUIRE_FALSE(list.timed_out);
    REQUIRE(list.exit_code == 0);
    REQUIRE(list.stdout_output.find("pr.workflow = github") != std::string::npos);
    REQUIRE_FALSE(bad.timed_out);
    REQUIRE(bad.exit_code != 0);
    REQUIRE(bad.stderr_output.find("pr.workflow must be one of") != std::string::npos);
}

TEST_CASE("pulp status reports effective PR workflow",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-pr-workflow-status");
    fs::create_directories(tmp_home);
    {
        std::ofstream cfg(tmp_home / "config.toml");
        cfg << "[pr]\nworkflow = \"manual\"\n"
            << "[update]\nmode = \"off\"\n"
            << "[import_design]\ndefault_mode = \"baked\"\n";
    }

    ScopedEnvVar import_mode_env("PULP_IMPORT_DESIGN_DEFAULT_MODE");
    ScopedEnvVar import_emit_env("PULP_IMPORT_DESIGN_DEFAULT_EMIT");
    import_mode_env.set("");
    import_emit_env.set("");
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"status"});
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp_home);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("PR workflow: manual (config:pr.workflow)") != std::string::npos);
    REQUIRE(r.stdout_output.find("Shipyard tracking: disabled by pr.workflow=manual") != std::string::npos);
    REQUIRE(r.stdout_output.find("Import design defaults: --mode baked (config:import_design.default_mode), --emit ir-json (implied by config:import_design.default_mode)") != std::string::npos);
}

TEST_CASE("pulp status and clean reject unexpected arguments before side effects",
          "[cli][shellout][misc][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto status_extra = run_pulp({"status", "extra"});
    REQUIRE_FALSE(status_extra.timed_out);
    REQUIRE(status_extra.exit_code == 2);
    REQUIRE(status_extra.stderr_output.find("Unexpected status argument") !=
            std::string::npos);

    auto clean_extra = run_pulp({"clean", "extra"});
    REQUIRE_FALSE(clean_extra.timed_out);
    REQUIRE(clean_extra.exit_code == 2);
    REQUIRE(clean_extra.stderr_output.find("Unexpected clean argument") !=
            std::string::npos);
}

TEST_CASE("pulp status reports invalid and github PR workflow modes",
          "[cli][shellout][pr-workflow]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar home_env("PULP_HOME");
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    ScopedEnvVar workflow_env("PULP_PR_WORKFLOW");
    update_disabled.set("1");

    auto invalid_home = unique_temp_dir("pulp-pr-workflow-status-invalid");
    fs::create_directories(invalid_home);
    home_env.set(invalid_home.string());
    workflow_env.set("subversion");
    auto invalid = run_pulp({"status"});
    fs::remove_all(invalid_home);

    REQUIRE_FALSE(invalid.timed_out);
    REQUIRE(invalid.exit_code == 0);
    REQUIRE(invalid.stdout_output.find("PR workflow: invalid (env:PULP_PR_WORKFLOW)")
            != std::string::npos);
    REQUIRE(invalid.stdout_output.find("pr.workflow must be one of")
            != std::string::npos);

    auto github_home = unique_temp_dir("pulp-pr-workflow-status-github");
    fs::create_directories(github_home);
    home_env.set(github_home.string());
    workflow_env.set("github");
    auto github = run_pulp({"status"});
    fs::remove_all(github_home);

    REQUIRE_FALSE(github.timed_out);
    REQUIRE(github.exit_code == 0);
    REQUIRE(github.stdout_output.find("PR workflow: github (env:PULP_PR_WORKFLOW)")
            != std::string::npos);
    REQUIRE(github.stdout_output.find("GitHub CLI:") != std::string::npos);
    REQUIRE(github.stdout_output.find("Shipyard tracking: disabled by pr.workflow=github")
            != std::string::npos);
}


TEST_CASE("pulp config set/get/list round-trips isolated update settings",
          "[cli][shellout][config][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-config-roundtrip");
    fs::remove_all(home);
    fs::create_directories(home);
    write_text(home / "update-snooze", "dismissed\n");

    ScopedEnvVar pulp_home("PULP_HOME");
    pulp_home.set(home.string());
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto set_mode = run_pulp({"config", "set", "update.mode", "manual"}, 10000);
    REQUIRE_FALSE(set_mode.timed_out);
    REQUIRE(set_mode.exit_code == 0);
    REQUIRE(set_mode.stdout_output.find("Set update.mode = manual") != std::string::npos);
    REQUIRE_FALSE(fs::exists(home / "update-snooze"));

    auto get_mode = run_pulp({"config", "get", "update.mode"}, 10000);
    REQUIRE_FALSE(get_mode.timed_out);
    REQUIRE(get_mode.exit_code == 0);
    REQUIRE(get_mode.stdout_output == "manual\n");

    auto set_channel = run_pulp({"config", "set", "update.channel", "beta"}, 10000);
    REQUIRE_FALSE(set_channel.timed_out);
    REQUIRE(set_channel.exit_code == 0);
    REQUIRE(set_channel.stdout_output.find("Set update.channel = beta") != std::string::npos);

    auto set_import_mode = run_pulp({"config", "set", "import_design.default_mode", "baked"}, 10000);
    REQUIRE_FALSE(set_import_mode.timed_out);
    REQUIRE(set_import_mode.exit_code == 0);
    REQUIRE(set_import_mode.stdout_output.find("Set import_design.default_mode = baked") != std::string::npos);

    auto set_import_emit = run_pulp({"config", "set", "import_design.default_emit", "cpp"}, 10000);
    REQUIRE_FALSE(set_import_emit.timed_out);
    REQUIRE(set_import_emit.exit_code == 0);
    REQUIRE(set_import_emit.stdout_output.find("Set import_design.default_emit = cpp") != std::string::npos);

    auto list = run_pulp({"config", "list"}, 10000);
    const auto config_body = read_file(home / "config.toml");
    fs::remove_all(home);

    REQUIRE_FALSE(list.timed_out);
    REQUIRE(list.exit_code == 0);
    REQUIRE(list.stdout_output.find("update.mode = manual") != std::string::npos);
    REQUIRE(list.stdout_output.find("update.check_interval_hours = 24") != std::string::npos);
    REQUIRE(list.stdout_output.find("update.channel = beta") != std::string::npos);
    REQUIRE(list.stdout_output.find("update.bump_projects = prompt") != std::string::npos);
    REQUIRE(list.stdout_output.find("import_design.default_mode = baked") != std::string::npos);
    REQUIRE(list.stdout_output.find("import_design.default_emit = cpp") != std::string::npos);
    REQUIRE(config_body.find("[update]") != std::string::npos);
    REQUIRE(config_body.find("[import_design]") != std::string::npos);
    REQUIRE(config_body.find("mode = \"manual\"") != std::string::npos);
    REQUIRE(config_body.find("channel = \"beta\"") != std::string::npos);
    REQUIRE(config_body.find("default_mode = \"baked\"") != std::string::npos);
    REQUIRE(config_body.find("default_emit = \"cpp\"") != std::string::npos);
}

TEST_CASE("pulp config rejects malformed and invalid update keys",
          "[cli][shellout][config][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-config-invalid");
    fs::remove_all(home);
    fs::create_directories(home);

    ScopedEnvVar pulp_home("PULP_HOME");
    pulp_home.set(home.string());
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto malformed_get = run_pulp({"config", "get", "update"}, 10000);
    REQUIRE_FALSE(malformed_get.timed_out);
    REQUIRE(malformed_get.exit_code != 0);
    REQUIRE(malformed_get.stderr_output.find("key must be dotted") != std::string::npos);

    auto unknown_key = run_pulp({"config", "set", "update.not_a_key", "value"}, 10000);
    REQUIRE_FALSE(unknown_key.timed_out);
    REQUIRE(unknown_key.exit_code != 0);
    REQUIRE(unknown_key.stderr_output.find("unknown config key") != std::string::npos);

    auto bad_mode = run_pulp({"config", "set", "update.mode", "weekly"}, 10000);
    REQUIRE_FALSE(bad_mode.timed_out);
    REQUIRE(bad_mode.exit_code != 0);
    REQUIRE(bad_mode.stderr_output.find("update.mode must be one of") != std::string::npos);

    auto bad_interval =
        run_pulp({"config", "set", "update.check_interval_hours", "-1"}, 10000);
    const bool config_written = fs::exists(home / "config.toml");
    fs::remove_all(home);

    REQUIRE_FALSE(bad_interval.timed_out);
    REQUIRE(bad_interval.exit_code != 0);
    REQUIRE(bad_interval.stderr_output.find("non-negative integer") != std::string::npos);
    REQUIRE_FALSE(config_written);

    auto bad_import_mode =
        run_pulp({"config", "set", "import_design.default_mode", "frozen"}, 10000);
    REQUIRE_FALSE(bad_import_mode.timed_out);
    REQUIRE(bad_import_mode.exit_code != 0);
    REQUIRE(bad_import_mode.stderr_output.find("import_design.default_mode must be one of")
            != std::string::npos);

    auto bad_import_emit =
        run_pulp({"config", "set", "import_design.default_emit", "tokens"}, 10000);
    REQUIRE_FALSE(bad_import_emit.timed_out);
    REQUIRE(bad_import_emit.exit_code != 0);
    REQUIRE(bad_import_emit.stderr_output.find("import_design.default_emit must be one of")
            != std::string::npos);

    auto extra_get = run_pulp({"config", "get", "update.mode", "extra"}, 10000);
    REQUIRE_FALSE(extra_get.timed_out);
    REQUIRE(extra_get.exit_code == 2);
    REQUIRE(extra_get.stderr_output.find("unexpected `pulp config get` argument") !=
            std::string::npos);

    auto extra_set = run_pulp({"config", "set", "update.mode", "manual", "extra"}, 10000);
    REQUIRE_FALSE(extra_set.timed_out);
    REQUIRE(extra_set.exit_code == 2);
    REQUIRE(extra_set.stderr_output.find("unexpected `pulp config set` argument") !=
            std::string::npos);

    auto extra_list = run_pulp({"config", "list", "extra"}, 10000);
    REQUIRE_FALSE(extra_list.timed_out);
    REQUIRE(extra_list.exit_code == 2);
    REQUIRE(extra_list.stderr_output.find("unexpected `pulp config list` argument") !=
            std::string::npos);
}

TEST_CASE("pulp version subcommand runs and mentions the SDK",
          "[cli][shellout][version]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"version"});
    // `pulp version` prints SDK/plugin version info and exits 0. Guard
    // against a regression where the subcommand silently exits with
    // success but an empty body (the exact shape of bug #295 for the
    // ship command).
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Must have SOMETHING on stdout — stderr alone is not enough.
    REQUIRE_FALSE(r.stdout_output.empty());
}

TEST_CASE("pulp version check exits 0 on a clean tree and mentions SDK/plugin/marketplace",
          "[cli][shellout][version]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"version", "check"});
    REQUIRE_FALSE(r.timed_out);
    // The check may exit non-zero if version files have drifted. Both
    // exit-code branches must include the three surfaces so the user
    // can act on the diagnostic.
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE((combined.find("sdk") != std::string::npos ||
             combined.find("SDK") != std::string::npos ||
             combined.find("CMakeLists.txt") != std::string::npos));
}

TEST_CASE("pulp version outside a project reports SDK but check fails clearly",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    auto outside = unique_temp_dir("pulp-version-outside");
    fs::create_directories(outside);

    auto show = run_pulp_in_directory(outside, {"version"});
    REQUIRE_FALSE(show.timed_out);
    REQUIRE(show.exit_code == 0);
    REQUIRE(show.stdout_output.find("Pulp SDK version:") != std::string::npos);
    REQUIRE(show.stdout_output.find("Project version:") == std::string::npos);
    REQUIRE(show.stderr_output.empty());

    auto check = run_pulp_in_directory(outside, {"version", "check"});
    REQUIRE_FALSE(check.timed_out);
    REQUIRE(check.exit_code == 1);
    REQUIRE(check.stderr_output.find("not in a Pulp project directory") !=
            std::string::npos);

    fs::remove_all(outside);
}

TEST_CASE("pulp version check accepts a complete matching project fixture",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    const auto sdk_version = repo_project_version_for_shellout_tests();
    auto project = write_version_project_fixture("pulp-version-clean",
                                                sdk_version,
                                                "1.2.3",
                                                "1.2.3",
                                                "1.2.3",
                                                sdk_version);

    auto check = run_pulp_in_directory(project, {"version", "check"});
    INFO(check.stdout_output);
    INFO(check.stderr_output);
    REQUIRE_FALSE(check.timed_out);
    REQUIRE(check.exit_code == 0);
    REQUIRE(check.stderr_output.empty());
    REQUIRE(check.stdout_output.find("SDK version consistent: " + sdk_version) !=
            std::string::npos);
    REQUIRE(check.stdout_output.find("AU Info.plist uses computed version integer") !=
            std::string::npos);
    REQUIRE(check.stdout_output.find("CHANGELOG latest version matches (" +
                                     sdk_version + ")") != std::string::npos);
    REQUIRE(check.stdout_output.find("Claude plugin version: 1.2.3") !=
            std::string::npos);
    REQUIRE(check.stdout_output.find("marketplace.json version matches plugin.json") !=
            std::string::npos);
    REQUIRE(check.stdout_output.find("marketplace.json plugins[0].version matches plugin.json") !=
            std::string::npos);

    fs::remove_all(project);
}

TEST_CASE("pulp version check reports every version drift surface",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    auto project = write_version_project_fixture("pulp-version-drift",
                                                "0.0.1",
                                                "not-semver",
                                                "9.9.9",
                                                "8.8.8",
                                                "7.7.7",
                                                true);

    auto check = run_pulp_in_directory(project, {"version", "check"});
    const auto combined = check.stdout_output + check.stderr_output;
    REQUIRE_FALSE(check.timed_out);
    REQUIRE(check.exit_code == 1);
    REQUIRE(combined.find("SDK version mismatch: CMakeLists.txt=0.0.1") !=
            std::string::npos);
    REQUIRE(combined.find("AU Info.plist has hardcoded version integer") !=
            std::string::npos);
    REQUIRE(combined.find("CHANGELOG latest (7.7.7) differs from CMakeLists.txt (0.0.1)") !=
            std::string::npos);
    REQUIRE(combined.find("plugin.json version is not semver: not-semver") !=
            std::string::npos);
    REQUIRE(combined.find("marketplace.json version (9.9.9) differs from plugin.json (not-semver)") !=
            std::string::npos);
    REQUIRE(combined.find("marketplace.json plugins[0].version (8.8.8) differs from plugin.json (not-semver)") !=
            std::string::npos);

    fs::remove_all(project);
}

TEST_CASE("pulp version check rejects unknown options before running checks",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    const auto sdk_version = repo_project_version_for_shellout_tests();
    auto project = write_version_project_fixture("pulp-version-unknown-option",
                                                sdk_version,
                                                "1.2.3",
                                                "1.2.3",
                                                "1.2.3",
                                                sdk_version);

    auto check = run_pulp_in_directory(project, {"version", "check", "--surprise"});
    REQUIRE_FALSE(check.timed_out);
    REQUIRE(check.exit_code == 2);
    REQUIRE(check.stderr_output.find("unknown version check option: --surprise") !=
            std::string::npos);
    REQUIRE(check.stdout_output.find("SDK version consistent") == std::string::npos);

    fs::remove_all(project);
}

TEST_CASE("pulp version bump updates project version and changelog guidance",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    auto project = write_version_project_fixture("pulp-version-bump",
                                                "1.2.3",
                                                "4.5.6",
                                                "4.5.6",
                                                "4.5.6",
                                                "1.2.3");

    auto bump = run_pulp_in_directory(project, {"version", "bump", "patch"});
    REQUIRE_FALSE(bump.timed_out);
    REQUIRE(bump.exit_code == 0);
    REQUIRE(bump.stderr_output.empty());
    REQUIRE(bump.stdout_output.find("Version bumped: 1.2.3 -> 1.2.4") !=
            std::string::npos);
    REQUIRE(bump.stdout_output.find("Added CHANGELOG.md entry for 1.2.4") !=
            std::string::npos);
    REQUIRE(bump.stdout_output.find("Tag with: git tag v1.2.4") !=
            std::string::npos);

    auto cmake = read_file(project / "CMakeLists.txt");
    auto changelog = read_file(project / "CHANGELOG.md");
    REQUIRE(cmake.find("project(VersionFixture VERSION 1.2.4)") !=
            std::string::npos);
    REQUIRE(cmake.find("pulp_add_plugin(Fixture VERSION \"4.5.6\")") !=
            std::string::npos);
    REQUIRE(changelog.find("## [1.2.4]\n\n## [1.2.3]") != std::string::npos);

    fs::remove_all(project);
}

TEST_CASE("pulp version bump --plugin only updates pulp_add_plugin version",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    auto project = write_version_project_fixture("pulp-version-plugin-bump",
                                                "1.2.3",
                                                "4.5.6",
                                                "4.5.6",
                                                "4.5.6",
                                                "1.2.3");

    auto bump = run_pulp_in_directory(project,
                                      {"version", "bump", "major", "--plugin"});
    REQUIRE_FALSE(bump.timed_out);
    REQUIRE(bump.exit_code == 0);
    REQUIRE(bump.stderr_output.empty());
    REQUIRE(bump.stdout_output.find("Version bumped: 4.5.6 -> 5.0.0") !=
            std::string::npos);
    REQUIRE(bump.stdout_output.find("Added CHANGELOG.md entry") == std::string::npos);
    REQUIRE(bump.stdout_output.find("Tag with: git tag") == std::string::npos);

    auto cmake = read_file(project / "CMakeLists.txt");
    auto changelog = read_file(project / "CHANGELOG.md");
    REQUIRE(cmake.find("project(VersionFixture VERSION 1.2.3)") !=
            std::string::npos);
    REQUIRE(cmake.find("pulp_add_plugin(Fixture VERSION \"5.0.0\")") !=
            std::string::npos);
    REQUIRE(changelog.find("## [5.0.0]") == std::string::npos);

    fs::remove_all(project);
}

TEST_CASE("pulp version bump rejects missing and invalid components",
          "[cli][shellout][version][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar updates("PULP_UPDATE_CHECK_DISABLED");
    updates.set("1");

    auto project = write_version_project_fixture("pulp-version-bump-invalid",
                                                "1.2.3",
                                                "4.5.6",
                                                "4.5.6",
                                                "4.5.6",
                                                "1.2.3");

    auto missing = run_pulp_in_directory(project, {"version", "bump"});
    REQUIRE_FALSE(missing.timed_out);
    REQUIRE(missing.exit_code == 1);
    REQUIRE(missing.stderr_output.find("Usage: pulp version bump") !=
            std::string::npos);

    auto invalid = run_pulp_in_directory(project, {"version", "bump", "tiny"});
    REQUIRE_FALSE(invalid.timed_out);
    REQUIRE(invalid.exit_code == 1);
    REQUIRE(invalid.stderr_output.find("component must be major, minor, or patch") !=
            std::string::npos);

    auto cmake = read_file(project / "CMakeLists.txt");
    REQUIRE(cmake.find("project(VersionFixture VERSION 1.2.3)") !=
            std::string::npos);
    REQUIRE(cmake.find("pulp_add_plugin(Fixture VERSION \"4.5.6\")") !=
            std::string::npos);

    fs::remove_all(project);
}

// pulp #709 / #468 — `pulp import-design --from claude` ingests a
// manually-exported Claude Design HTML file AND scaffolds a
// bridge_handlers.cpp next to the generated JS. This is the CLI
// surface that pulp#468's manual-export framing depends on.
TEST_CASE("pulp import-design --from claude writes JS + bridge handler scaffold",
          "[cli][shellout][import-design][issue-709][issue-468]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-claude-smoke";
    fs::create_directories(tmp);
    auto html_path    = tmp / "claude-export.html";
    auto js_path      = tmp / "claude-ui.js";
    auto tokens_path  = tmp / "claude-tokens.json";
    auto bridge_path  = tmp / "claude-bridge.cpp";
    {
        std::ofstream f(html_path);
        f << "<!DOCTYPE html><html><body><div class=\"container\">"
             "<h1>Hello Claude</h1><button>Click</button></div></body></html>";
    }
    // Clean stale artifacts from prior runs.
    fs::remove(js_path);
    fs::remove(tokens_path);
    fs::remove(bridge_path);

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file",   html_path.string(),
                       "--output", js_path.string(),
                       "--tokens", tokens_path.string(),
                       "--bridge-output", bridge_path.string()},
                       30000);
    REQUIRE_FALSE(r.timed_out);
    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_path));
    REQUIRE(fs::exists(bridge_path));

    // Scaffold must reference the framework surface by full name so
    // future readers can trace the generated file back to its
    // framework host even after they've edited the handlers.
    std::ifstream b(bridge_path);
    std::string bridge_contents((std::istreambuf_iterator<char>(b)),
                                 std::istreambuf_iterator<char>());
    REQUIRE(bridge_contents.find("pulp::view::EditorBridge") != std::string::npos);
    REQUIRE(bridge_contents.find("add_handler") != std::string::npos);
    REQUIRE(bridge_contents.find("attach_webview") != std::string::npos);

    // Stdout reports both outputs (the JS view AND the scaffold).
    const auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("bridge handler scaffold") != std::string::npos);
}

// Paired with the above: `--no-bridge-scaffold` suppresses the
// scaffold emission while keeping the HTML → JS path intact.
TEST_CASE("pulp import-design --from claude --no-bridge-scaffold writes only the JS",
          "[cli][shellout][import-design][issue-709]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-claude-smoke-no-scaffold";
    fs::create_directories(tmp);
    auto html_path   = tmp / "claude-export.html";
    auto js_path     = tmp / "claude-ui.js";
    auto bridge_path = tmp / "bridge_handlers.cpp";
    {
        std::ofstream f(html_path);
        f << "<!DOCTYPE html><html><body><p>hi</p></body></html>";
    }
    fs::remove(js_path);
    fs::remove(bridge_path);

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_path.string(),
                       "--output", js_path.string(),
                       "--no-bridge-scaffold"},
                      30000);
    REQUIRE_FALSE(r.timed_out);
    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_path));
    REQUIRE_FALSE(fs::exists(bridge_path));
}

TEST_CASE("pulp help output lists the top-level subcommands",
          "[cli][shellout][help]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"help"});
    REQUIRE(r.exit_code == 0);

    // Every subcommand that's visible to users in the ci skill / docs
    // needs to survive help output — if someone silently drops one
    // from the dispatch table, this fails loudly.
    for (const char* cmd : {"build", "test", "run", "validate", "ship",
                            "version", "doctor", "create", "clean",
                            "docs", "status", "inspect"}) {
        INFO("help output missing subcommand: " << cmd);
        REQUIRE(r.stdout_output.find(cmd) != std::string::npos);
    }
}

TEST_CASE("pulp macos validates local operator arguments before gh calls",
          "[cli][shellout][macos][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto help = run_pulp({"macos", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("pulp macos") != std::string::npos);
    REQUIRE(help.stdout_output.find("retarget --pr") != std::string::npos);

    auto unknown = run_pulp({"macos", "wat"}, 10000);
    REQUIRE_FALSE(unknown.timed_out);
    REQUIRE(unknown.exit_code == 1);
    REQUIRE(unknown.stderr_output.find("unknown subcommand") != std::string::npos);

    auto missing = run_pulp({"macos", "retarget", "--pr", "123"}, 10000);
    REQUIRE_FALSE(missing.timed_out);
    REQUIRE(missing.exit_code == 1);
    REQUIRE(missing.stderr_output.find("Usage: pulp macos retarget")
            != std::string::npos);

    auto missing_pr_value = run_pulp({"macos", "retarget", "--pr"}, 10000);
    REQUIRE_FALSE(missing_pr_value.timed_out);
    REQUIRE(missing_pr_value.exit_code == 2);
    REQUIRE(missing_pr_value.stderr_output.find("--pr requires a value")
            != std::string::npos);

    auto missing_to_value = run_pulp({"macos", "retarget", "--pr", "123", "--to"},
                                     10000);
    REQUIRE_FALSE(missing_to_value.timed_out);
    REQUIRE(missing_to_value.exit_code == 2);
    REQUIRE(missing_to_value.stderr_output.find("--to requires a value")
            != std::string::npos);

    auto invalid_runner = run_pulp({"macos", "retarget", "--pr", "123", "--to", "mars"},
                                   10000);
    REQUIRE_FALSE(invalid_runner.timed_out);
    REQUIRE(invalid_runner.exit_code == 1);
    REQUIRE(invalid_runner.stderr_output.find("--to must be one of")
            != std::string::npos);

    auto status_unknown = run_pulp({"macos", "status", "--surprise"}, 10000);
    REQUIRE_FALSE(status_unknown.timed_out);
    REQUIRE(status_unknown.exit_code == 1);
    REQUIRE(status_unknown.stderr_output.find("unknown arg '--surprise'")
            != std::string::npos);

    auto status_missing_pr = run_pulp({"macos", "status", "--pr"}, 10000);
    REQUIRE_FALSE(status_missing_pr.timed_out);
    REQUIRE(status_missing_pr.exit_code == 2);
    REQUIRE(status_missing_pr.stderr_output.find("--pr requires a value")
            != std::string::npos);
}

TEST_CASE("pulp overflow validates non-mutating operator arguments",
          "[cli][shellout][overflow][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto help = run_pulp({"overflow", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("pulp overflow") != std::string::npos);
    REQUIRE(help.stdout_output.find("threshold [N]") != std::string::npos);

    auto unknown = run_pulp({"overflow", "wat"}, 10000);
    REQUIRE_FALSE(unknown.timed_out);
    REQUIRE(unknown.exit_code == 1);
    REQUIRE(unknown.stderr_output.find("unknown subcommand") != std::string::npos);

    auto threshold_extra = run_pulp({"overflow", "threshold", "1", "2"}, 10000);
    REQUIRE_FALSE(threshold_extra.timed_out);
    REQUIRE(threshold_extra.exit_code == 1);
    REQUIRE(threshold_extra.stderr_output.find("too many args") != std::string::npos);

    auto threshold_negative = run_pulp({"overflow", "threshold", "-1"}, 10000);
    REQUIRE_FALSE(threshold_negative.timed_out);
    REQUIRE(threshold_negative.exit_code == 1);
    REQUIRE(threshold_negative.stderr_output.find("must be >= 0") != std::string::npos);

    auto threshold_bad = run_pulp({"overflow", "threshold", "nope"}, 10000);
    REQUIRE_FALSE(threshold_bad.timed_out);
    REQUIRE(threshold_bad.exit_code == 1);
    REQUIRE(threshold_bad.stderr_output.find("is not a number") != std::string::npos);

    auto enable_missing_to = run_pulp({"overflow", "enable", "--to"}, 10000);
    REQUIRE_FALSE(enable_missing_to.timed_out);
    REQUIRE(enable_missing_to.exit_code == 2);
    REQUIRE(enable_missing_to.stderr_output.find("--to requires a value")
            != std::string::npos);

    auto enable_flag_value = run_pulp({"overflow", "enable", "--to", "--flag"},
                                      10000);
    REQUIRE_FALSE(enable_flag_value.timed_out);
    REQUIRE(enable_flag_value.exit_code == 2);
    REQUIRE(enable_flag_value.stderr_output.find("--to requires a value")
            != std::string::npos);
}

TEST_CASE("pulp inspect help and no-discovery paths are deterministic",
          "[cli][shellout][inspect][issue-643][issue-641]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto help = run_pulp({"inspect", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("Usage: pulp inspect [options]")
            != std::string::npos);
    REQUIRE(help.stdout_output.find("--port PORT") != std::string::npos);
    REQUIRE(help.stdout_output.find("--output FILE") != std::string::npos);

    auto base = unique_temp_dir("pulp-inspect-no-discovery");
    fs::create_directories(base);
#if defined(_WIN32)
    ScopedEnvVar temp_dir("TEMP");
#else
    ScopedEnvVar temp_dir("TMPDIR");
#endif
    temp_dir.set(base.string());

    auto missing = run_pulp({"inspect"}, 10000);
    fs::remove_all(base);

    REQUIRE_FALSE(missing.timed_out);
    REQUIRE(missing.exit_code == 1);
    REQUIRE(missing.stderr_output.find("no running Pulp inspector found")
            != std::string::npos);
    REQUIRE(missing.stderr_output.find("specify --port") != std::string::npos);
    REQUIRE(missing.stdout_output.find("Connecting to") == std::string::npos);
}

TEST_CASE("pulp inspect explicit port failure does not require a server",
          "[cli][shellout][inspect][issue-643][issue-641]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto base = unique_temp_dir("pulp-inspect-explicit-port");
    fs::create_directories(base);
    auto output = base / "inspect-response.json";

    auto r = run_pulp({"inspect",
                       "--host", "127.0.0.1",
                       "--port", "1",
                       "--command", "DOM.getDocument",
                       "--params", "{\"depth\":1}",
                       "--output", output.string()},
                      5000);
    const bool wrote_output = fs::exists(output);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stdout_output.find("Connecting to 127.0.0.1:1")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("could not connect to 127.0.0.1:1")
            != std::string::npos);
    REQUIRE_FALSE(wrote_output);
}

TEST_CASE("pulp inspect rejects invalid arguments before networking",
          "[cli][shellout][inspect][issue-643][issue-641]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto missing_port = run_pulp({"inspect", "--port"}, 10000);
    REQUIRE_FALSE(missing_port.timed_out);
    REQUIRE(missing_port.exit_code == 2);
    REQUIRE(missing_port.stderr_output.find("--port requires a value")
            != std::string::npos);
    REQUIRE(missing_port.stdout_output.find("Connecting to") == std::string::npos);

    auto invalid_port = run_pulp({"inspect", "--port", "not-a-port"}, 10000);
    REQUIRE_FALSE(invalid_port.timed_out);
    REQUIRE(invalid_port.exit_code == 2);
    REQUIRE(invalid_port.stderr_output.find("invalid --port value: not-a-port")
            != std::string::npos);
    REQUIRE(invalid_port.stdout_output.find("Connecting to") == std::string::npos);

    for (const char* port : {"0", "65536", "123abc"}) {
        INFO("port=" << port);
        auto rejected_port = run_pulp({"inspect", "--port", port}, 10000);
        REQUIRE_FALSE(rejected_port.timed_out);
        REQUIRE(rejected_port.exit_code == 2);
        REQUIRE(rejected_port.stderr_output.find(std::string("invalid --port value: ") + port)
                != std::string::npos);
        REQUIRE(rejected_port.stdout_output.find("Connecting to") == std::string::npos);
    }

    auto output_without_command = run_pulp({"inspect", "--output", "out.json"}, 10000);
    REQUIRE_FALSE(output_without_command.timed_out);
    REQUIRE(output_without_command.exit_code == 2);
    REQUIRE(output_without_command.stderr_output.find("--output requires --command")
            != std::string::npos);
    REQUIRE(output_without_command.stdout_output.find("Connecting to")
            == std::string::npos);

    auto params_without_command = run_pulp({"inspect", "--params", "{}"}, 10000);
    REQUIRE_FALSE(params_without_command.timed_out);
    REQUIRE(params_without_command.exit_code == 2);
    REQUIRE(params_without_command.stderr_output.find("--params requires --command")
            != std::string::npos);
    REQUIRE(params_without_command.stdout_output.find("Connecting to")
            == std::string::npos);

    auto unknown = run_pulp({"inspect", "--definitely-not-an-inspect-flag"}, 10000);
    REQUIRE_FALSE(unknown.timed_out);
    REQUIRE(unknown.exit_code == 2);
    REQUIRE(unknown.stderr_output.find("unknown inspect argument")
            != std::string::npos);
    REQUIRE(unknown.stdout_output.find("Connecting to") == std::string::npos);
}

TEST_CASE("pulp create scaffolds a no-build app project with Android files",
          "[cli][shellout][create][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-create-app-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto project = base / "out" / "neon-drum";
    fs::create_directories(home);
    fs::create_directories(project.parent_path());
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"create", "Neon Drum", "--type", "app",
                       "--targets", "android,standalone",
                       "--output", project.string(), "--no-build", "--ci"},
                      60000);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");

    const bool has_header = fs::exists(project / "neon_drum.hpp");
    const bool has_main = fs::exists(project / "main.cpp");
    const bool has_test = fs::exists(project / "test_neon_drum.cpp");
    const bool has_cmake = fs::exists(project / "CMakeLists.txt");
    const bool has_toml = fs::exists(project / "pulp.toml");
    const bool has_android_settings = fs::exists(project / "android" / "settings.gradle.kts");
    const bool has_android_activity =
        fs::exists(project / "android" / "app" / "src" / "main" / "java" /
                   "neon_drum" / "MainActivity.kt");
    const auto header = read_file(project / "neon_drum.hpp");
    const auto main_cpp = read_file(project / "main.cpp");
    const auto cmake = read_file(project / "CMakeLists.txt");
    const auto toml = read_file(project / "pulp.toml");
    const auto registry = read_file(home / "projects.json");

    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(has_header);
    REQUIRE(has_main);
    REQUIRE(has_test);
    REQUIRE(has_cmake);
    REQUIRE(has_toml);
    REQUIRE(has_android_settings);
    REQUIRE(has_android_activity);
    REQUIRE(header.find("class NeonDrum") != std::string::npos);
    REQUIRE(header.find("namespace neon_drum") != std::string::npos);
    REQUIRE(header.find("com.pulp.neon_drum") != std::string::npos);
    REQUIRE(main_cpp.find("neon_drum::create_neon_drum") != std::string::npos);
    REQUIRE(cmake.find("pulp_add_app(NeonDrum") != std::string::npos);
    REQUIRE(toml.find("sdk_checkout") != std::string::npos);
    REQUIRE(registry.find("Neon Drum") != std::string::npos);
    REQUIRE(registry.find("neon-drum") != std::string::npos);
}

TEST_CASE("pulp create rejects invalid type before scaffolding",
          "[cli][shellout][create][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-create-invalid-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto project = base / "out" / "bad-type";
    fs::create_directories(home);
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"create", "Bad Type", "--type", "potato",
                       "--output", project.string(), "--no-build", "--ci"},
                      10000);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");

    const bool project_exists = fs::exists(project);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE_FALSE(project_exists);
    REQUIRE(r.stderr_output.find("--type must be") != std::string::npos);
}

TEST_CASE("pulp create validates parser errors before scaffolding",
          "[cli][shellout][create][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-create-parser-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto out = base / "out";
    fs::create_directories(home);
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }

    struct Case {
        std::vector<std::string> args;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {{"create", "Missing Type", "--type"}, "--type requires a value"},
        {{"create", "Missing Template", "--template", "--no-build"}, "--template requires a value"},
        {{"create", "Missing Manufacturer", "--manufacturer"}, "--manufacturer requires a value"},
        {{"create", "Missing Output", "--output"}, "--output requires a value"},
        {{"create", "Missing Targets", "--targets"}, "--targets requires a value"},
        {{"create", "Unknown Flag", "--definitely-not-create"}, "unknown flag"},
    };

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    for (const auto& c : cases) {
        auto r = run_pulp(c.args, 10000);
        INFO("stderr: " << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.needle) != std::string::npos);
    }
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");

    REQUIRE_FALSE(fs::exists(out));
    fs::remove_all(base);
}

// #51 / #356: `pulp validate --strict` is supposed to upgrade
// skipped-because-missing-tool into a hard failure.
//
// cmd_validate now parses flags up-front and rejects unknown flags
// with exit code 2 — that gives us a testable distinction between
// "known flag, can't run because no project" and "unknown flag"
// without needing a real build tree. Codex P2 on PR #381 correctly
// flagged that the prior version of this test couldn't tell the two
// apart: unknown flags were silently ignored, so the assertion would
// pass even if --strict handling were removed.
TEST_CASE("pulp validate --strict is a recognized flag",
          "[cli][shellout][validate][issue-356]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Run from /tmp so resolve_active_project_root() bails with
    // "not in a Pulp project directory". Flag parsing runs first, so
    // --strict passes through cleanly and we hit exit 1, not 2.
    //
    // NB: resolve the absolute binary path up front. run_pulp's
    // pulp_binary() is cwd-relative ("<cwd>/../tools/cli/pulp"), so
    // if we swap cwd to /tmp first the lookup resolves to a path
    // that doesn't exist and we never actually run the CLI.
    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto strict = exec(bin.string(), {"validate", "--strict"}, 10000);
    auto bogus = exec(bin.string(), {"validate", "--this-flag-does-not-exist"}, 10000);
    auto missing_report = exec(bin.string(), {"validate", "--report"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(strict.timed_out);
    REQUIRE_FALSE(bogus.timed_out);
    REQUIRE_FALSE(missing_report.timed_out);

    // --strict is known: flag parser accepts, we fall through to the
    // project-root bail-out, exit 1.
    REQUIRE(strict.exit_code == 1);
    REQUIRE(strict.stderr_output.find("unknown flag") == std::string::npos);

    // Unknown flag: rejected with exit 2 before the project-root check
    // even runs. If cmd_validate ever silently swallows unknown flags
    // again, this fails loudly and --strict handling is protected.
    REQUIRE(bogus.exit_code == 2);
    REQUIRE(bogus.stderr_output.find("unknown flag") != std::string::npos);
    REQUIRE(bogus.stderr_output.find("--this-flag-does-not-exist")
            != std::string::npos);

    REQUIRE(missing_report.exit_code == 2);
    REQUIRE(missing_report.stderr_output.find("--report requires a path")
            != std::string::npos);
    REQUIRE(missing_report.stderr_output.find("not in a Pulp project directory")
            == std::string::npos);
}

TEST_CASE("pulp validate strict json report records missing VST3 validators",
          "[cli][shellout][validate][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = unique_temp_dir("pulp-validate-strict");
    auto project = base / "Project";
    auto build = project / "build";
    auto report = base / "validation-report.json";
    write_text(project / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.22)\n"
               "project(ValidateFixture VERSION 1.2.3)\n");
    write_text(project / "pulp.toml", "[pulp]\nsdk_version = \"1.2.3\"\n");
    write_text(build / "CMakeCache.txt", "# fixture cache\n");
    fs::create_directories(build / "VST3" / "Fixture.vst3");

    ScopedEnvVar path("PATH");
    path.set("");

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(project);
    auto r = exec(bin.string(),
                  {"validate", "--strict", "--json", "--report", report.string()},
                  10000);
    fs::current_path(cwd_saver);

    auto report_body = read_file(report);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stdout_output.find("Validation Summary: 1 total")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("\"status\": \"skip\"") != std::string::npos);
    REQUIRE(r.stderr_output.find("ERROR: 1 validator(s) not installed")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("pluginval") != std::string::npos);
    REQUIRE(r.stderr_output.find("Skipped-because-missing-tool count: 1")
            != std::string::npos);
    REQUIRE(report_body.find("\"plugin_format\": \"vst3\"")
            != std::string::npos);
    REQUIRE(report_body.find("\"status\": \"skip\"") != std::string::npos);
}

TEST_CASE("pulp validate covers empty builds, report failures, and screenshots",
          "[cli][shellout][validate][issue-643]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = unique_temp_dir("pulp-validate-empty");
    auto project = base / "Project";
    auto build = project / "build";
    write_text(project / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.22)\n"
               "project(ValidateEmpty VERSION 1.2.3)\n");
    write_text(project / "pulp.toml", "[pulp]\nsdk_version = \"1.2.3\"\n");
    write_text(build / "CMakeCache.txt", "# fixture cache\n");

    ScopedEnvVar path("PATH");
    path.set("");

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(project);
    auto r = exec(bin.string(),
                  {"validate", "--screenshot", "--report", build.string()},
                  10000);
    fs::current_path(cwd_saver);
    fs::remove_all(base);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Validation Summary: 0 total")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("No plugin screenshots captured")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("Failed to write report to")
            != std::string::npos);
}

TEST_CASE("pulp build fails fast when standalone SDK is ahead of the installed CLI",
          "[cli][shellout][build][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-build-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"build"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") != std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") != std::string::npos);
    REQUIRE(combined.find("pulp doctor --versions") != std::string::npos);
    REQUIRE(combined.find("--allow-unsupported-sdk") != std::string::npos);
}

TEST_CASE("pulp build allows explicit unsupported SDK bypass",
          "[cli][shellout][build][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-build-allow-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"build", "--allow-unsupported-sdk"}, 30000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") == std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") == std::string::npos);
}

TEST_CASE("pulp build validates js engine option before compatibility checks",
          "[cli][shellout][build][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-shellout-build-js-engine");
    fs::create_directories(tmp);
    write_text(tmp / "pulp.toml",
               "[pulp]\n"
               "sdk_version = \"99.0.0\"\n"
               "cli_min_version = \"99.0.0\"\n");

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto missing = exec(bin.string(), {"build", "--js-engine"}, 10000);
    auto invalid = exec(bin.string(), {"build", "--js-engine=spidermonkey"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(missing.timed_out);
    REQUIRE(missing.exit_code == 2);
    REQUIRE(missing.stderr_output.find("--js-engine requires a value")
            != std::string::npos);
    REQUIRE(missing.stderr_output.find("requires a newer Pulp CLI")
            == std::string::npos);

    REQUIRE_FALSE(invalid.timed_out);
    REQUIRE(invalid.exit_code == 1);
    REQUIRE(invalid.stderr_output.find("--js-engine must be auto, quickjs, jsc, or v8")
            != std::string::npos);
    REQUIRE(invalid.stderr_output.find("requires a newer Pulp CLI")
            == std::string::npos);
}

// #8 / #355 — `pulp doctor android` and `pulp doctor ios` are
// recognized subcommands; bogus subcommand fails with exit 2 + Usage.


// Issue #550 Slice 5: `update.mode = off` must produce zero network
// traffic and zero banner output. We can't directly observe the
// network inside ctest, but we CAN verify that (a) the command
// finishes well under the anonymous-GitHub round-trip latency (caching
// the cache is fine; actually dialing the API is not), and (b) no
// update banner appears on stderr. The banner hook's early-return on
// `Mode::Off` is what this test guards.
//
// We use PULP_HOME to point the CLI at a scratch config dir seeded
// with `update.mode = "off"` so we don't mutate the developer's real
// ~/.pulp. The test creates the config file directly rather than
// shelling out to `pulp config set` so we can reason about the
// sequence (`pulp config set` itself clears the snooze file — we want
// to isolate the dispatch-path behaviour).
TEST_CASE("pulp with update.mode=off never prints a banner",
          "[cli][shellout][update-mode][issue-550]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-mode-off-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream cfg(tmp / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }
    // Seed the cache with a spurious "newer version available" entry.
    // If the off-mode short-circuit regressed, this is what would
    // leak into the banner.
    {
        std::ofstream cache(tmp / "update-cache.json");
        cache << "{\n"
              << "  \"schema\": 1,\n"
              << "  \"last_check_epoch_sec\": 1713638400,\n"
              << "  \"latest_version\": \"99.99.99\",\n"
              << "  \"release_notes_url\": \"https://example.invalid/\",\n"
              << "  \"banner_shown_for_version\": \"\"\n"
              << "}\n";
    }

    const auto bin = fs::absolute(pulp_binary());
#ifdef _WIN32
    _putenv_s("PULP_HOME", tmp.string().c_str());
#else
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
#endif
    auto r = exec(bin.string(), {"help"}, 10000);
#ifdef _WIN32
    _putenv_s("PULP_HOME", "");
#else
    pulp_unsetenv("PULP_HOME");
#endif
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // The seeded cache has latest=99.99.99 which would trigger the
    // prompt/manual/auto banners. off-mode must suppress ALL of them.
    REQUIRE(r.stderr_output.find("99.99.99") == std::string::npos);
    REQUIRE(r.stderr_output.find("available") == std::string::npos);
    REQUIRE(r.stderr_output.find("downloaded") == std::string::npos);
}

// Issue #550 Slice 5: `update.mode = manual` prints the one-liner
// once per version. The banner shape is locked — it must differ from
// the prompt-mode banner so users (and shell scripts) can tell them
// apart. Same PULP_HOME scratch-dir trick as the off-mode test.
TEST_CASE("pulp with update.mode=manual prints the manual notice",
          "[cli][shellout][update-mode][issue-550]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-mode-manual-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream cfg(tmp / "config.toml");
        cfg << "[update]\nmode = \"manual\"\n";
    }
    {
        std::ofstream cache(tmp / "update-cache.json");
        cache << "{\n"
              << "  \"schema\": 1,\n"
              << "  \"last_check_epoch_sec\": 1713638400,\n"
              << "  \"latest_version\": \"99.99.99\",\n"
              << "  \"release_notes_url\": \"https://example.invalid/\",\n"
              << "  \"banner_shown_for_version\": \"\"\n"
              << "}\n";
    }

    const auto bin = fs::absolute(pulp_binary());
#ifdef _WIN32
    _putenv_s("PULP_HOME", tmp.string().c_str());
#else
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
#endif
    // Use `doctor --versions` — it's not on the banner_blocked list,
    // so the dispatch hook runs for it. `help` IS blocked.
    auto r = exec(bin.string(), {"doctor", "--versions"}, 30000);
#ifdef _WIN32
    _putenv_s("PULP_HOME", "");
#else
    pulp_unsetenv("PULP_HOME");
#endif
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    // Manual mode banner shape: "Run `pulp upgrade` when you're ready."
    REQUIRE(r.stderr_output.find("when you're ready") != std::string::npos);
    REQUIRE(r.stderr_output.find("99.99.99") != std::string::npos);
}

// Issue #564 Slice 7: `pulp project bump --help` must be wired at the
// dispatch level. Any future regression where the `project` command
// falls out of the dispatch table fails loudly here — same class of
// silent-failure bug that motivated the rest of this file.
TEST_CASE("pulp project is a recognized command with pin/unpin/undo subcommands",
          "[cli][shellout][issue-564][issue-2087]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto help = run_pulp({"project", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    // pulp #2087: `pin` is the primary command name; `bump` survives as
    // a deprecated alias for one minor release.
    REQUIRE(help.stdout_output.find("project pin") != std::string::npos);
    REQUIRE(help.stdout_output.find("project unpin") != std::string::npos);
    REQUIRE(help.stdout_output.find("project undo") != std::string::npos);
    REQUIRE(help.stdout_output.find("deprecated alias") != std::string::npos);

    // `pulp project pin --help` is the new primary help surface.
    auto pin_help = run_pulp({"project", "pin", "--help"}, 10000);
    REQUIRE_FALSE(pin_help.timed_out);
    REQUIRE(pin_help.exit_code == 0);
    REQUIRE(pin_help.stdout_output.find("--all") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--dry-run") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--force-dirty") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--allow-downgrade") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--allow-cli-skew") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--allow-redundant") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("--verify-builds") != std::string::npos);
    REQUIRE(pin_help.stdout_output.find("pulp.toml sdk_version") != std::string::npos);
    // The pin help should cross-reference unpin so the round-trip is
    // discoverable from either direction.
    REQUIRE(pin_help.stdout_output.find("pulp project unpin") != std::string::npos);

    // `pulp project bump --help` must still work (backward-compat alias).
    auto bump_help = run_pulp({"project", "bump", "--help"}, 10000);
    REQUIRE_FALSE(bump_help.timed_out);
    REQUIRE(bump_help.exit_code == 0);
    // The alias goes through the same code path as `pin`, so the help
    // is the same text — confirms users won't get a stale "bump"-named
    // dead-end if they keep typing the old command.
    REQUIRE(bump_help.stdout_output.find("--all") != std::string::npos);

    auto unpin_help = run_pulp({"project", "unpin", "--help"}, 10000);
    REQUIRE_FALSE(unpin_help.timed_out);
    REQUIRE(unpin_help.exit_code == 0);
    REQUIRE(unpin_help.stdout_output.find("floating") != std::string::npos);
    REQUIRE(unpin_help.stdout_output.find("pulp project pin") != std::string::npos);

    auto undo_help = run_pulp({"project", "undo", "--help"}, 10000);
    REQUIRE_FALSE(undo_help.timed_out);
    REQUIRE(undo_help.exit_code == 0);
    REQUIRE(undo_help.stdout_output.find("Revert") != std::string::npos);

    auto bogus = run_pulp({"project", "potato"}, 10000);
    REQUIRE_FALSE(bogus.timed_out);
    REQUIRE(bogus.exit_code != 0);
    REQUIRE(bogus.stderr_output.find("unknown subcommand") != std::string::npos);
}

// pulp #2087: `pulp project unpin` rewrites pulp.toml's sdk_version
// to "latest" so the project tracks the newest installed SDK on every
// rebuild. Inverse of `pin <version>`. We exercise the round trip
// against a synthetic standalone project fixture so the test doesn't
// depend on the registry or on remote network state.
TEST_CASE("pulp project unpin switches a pinned project to floating mode",
          "[cli][shellout][issue-2087]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-unpin-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto home = base / "home";
    auto project = base / "out" / "my-plugin";
    fs::create_directories(home);
    fs::create_directories(project);

    // Minimal pinned project. Don't run `pulp create` — it would fetch
    // the SDK from the network, which we don't want in a unit test.
    // The unpin command only needs pulp.toml + a not-the-Pulp-checkout
    // layout to operate, so we synthesize both inline.
    {
        std::ofstream f(project / "pulp.toml");
        f << "[pulp]\n";
        f << "sdk_version = \"0.91.0\"\n";
    }
    {
        std::ofstream f(project / "CMakeLists.txt");
        f << "cmake_minimum_required(VERSION 3.24)\n";
        f << "project(MyPlugin VERSION 1.0.0 LANGUAGES CXX)\n";
        f << "find_package(Pulp 0.91.0 REQUIRED)\n";
    }

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(project);

    // Dry-run first: must NOT mutate the file.
    auto dry = exec(bin.string(), {"project", "unpin", "--dry-run"}, 10000);
    const auto pre_dry = read_file(project / "pulp.toml");
    REQUIRE_FALSE(dry.timed_out);
    REQUIRE(dry.exit_code == 0);
    REQUIRE(dry.stdout_output.find("[dry-run]") != std::string::npos);
    REQUIRE(dry.stdout_output.find("0.91.0") != std::string::npos);
    REQUIRE(pre_dry.find("sdk_version = \"0.91.0\"") != std::string::npos);

    // Real run: pulp.toml's sdk_version must become "latest".
    auto run = exec(bin.string(), {"project", "unpin"}, 10000);
    const auto post = read_file(project / "pulp.toml");
    REQUIRE_FALSE(run.timed_out);
    REQUIRE(run.exit_code == 0);
    REQUIRE(run.stdout_output.find("unpinned") != std::string::npos);
    REQUIRE(post.find("sdk_version = \"latest\"") != std::string::npos);
    REQUIRE(post.find("sdk_version = \"0.91.0\"") == std::string::npos);

    // Idempotence: running unpin twice should be a no-op the second time.
    auto run2 = exec(bin.string(), {"project", "unpin"}, 10000);
    REQUIRE_FALSE(run2.timed_out);
    REQUIRE(run2.exit_code == 0);
    REQUIRE(run2.stdout_output.find("already floating") != std::string::npos);

    fs::current_path(cwd_saver);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(base);
}

// Issue #564 Slice 7: `pulp project bump --dry-run` rejects an
// invalid --to value with a diagnostic. No writes happen.
TEST_CASE("pulp project bump rejects non-semver --to",
          "[cli][shellout][issue-564]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    // Run from /tmp so there's no CMakeLists.txt — but we expect the
    // semver validation to fire BEFORE any filesystem touch anyway.
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto r = exec(bin.string(), {"project", "bump", "--to=not-a-version", "--dry-run"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("invalid target version") != std::string::npos);
}

TEST_CASE("pulp project bump rejects missing --to values before project lookup",
          "[cli][shellout][project][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());

    for (const auto& args : std::vector<std::vector<std::string>>{
             {"project", "bump", "--to"},
             {"project", "bump", "--to", "--dry-run"},
             {"project", "pin", "--to"},
             {"project", "pin", "--to", "--all"},
         }) {
        auto r = exec(bin.string(), args, 10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find("--to requires a version argument")
                != std::string::npos);
    }

    fs::current_path(cwd_saver);
}

TEST_CASE("pulp project validates stray parser arguments before project lookup",
          "[cli][shellout][project][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());

    for (const auto& c : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"project", "bump", "--bogus"}, "unknown argument '--bogus'"},
             {{"project", "pin", "1.2.3", "2.3.4"}, "unexpected extra version argument '2.3.4'"},
             {{"project", "unpin", "--bogus"}, "unknown argument '--bogus'"},
         }) {
        auto r = exec(bin.string(), c.first, 10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.second) != std::string::npos);
        REQUIRE(r.stderr_output.find("not inside") == std::string::npos);
    }

    fs::current_path(cwd_saver);
}


TEST_CASE("pulp project bump updates standalone SDK pins and undo reverts them",
          "[cli][shellout][project-bump][issue-244]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-project-bump-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto project = base / "Clock";
    auto home = base / "home";
    fs::create_directories(project);
    fs::create_directories(home);
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }
    {
        std::ofstream cmake(project / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.20)\n"
              << "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
              << "find_package(Pulp 0.1.0 REQUIRED)\n";
    }
    {
        std::ofstream toml(project / "pulp.toml");
        toml << "[pulp]\n"
             << "sdk_version = \"0.1.0\"\n"
             << "sdk_path = \"/custom/sdk\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    const auto saved_cwd = fs::current_path();
    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    fs::current_path(project);

    auto bump = exec(bin.string(), {"project", "bump", "--to=0.2.0"}, 10000);
    std::string cmake_after;
    std::string toml_after;
    ProcessResult undo;
    if (!bump.timed_out && bump.exit_code == 0) {
        cmake_after = read_file(project / "CMakeLists.txt");
        toml_after = read_file(project / "pulp.toml");
        undo = exec(bin.string(), {"project", "undo"}, 10000);
    }
    fs::current_path(saved_cwd);
    pulp_unsetenv("PULP_HOME");

    REQUIRE_FALSE(bump.timed_out);
    REQUIRE(bump.exit_code == 0);
    REQUIRE(bump.stdout_output.find("bumped") != std::string::npos);
    REQUIRE(bump.stdout_output.find("custom sdk_path left unchanged") != std::string::npos);

    REQUIRE(cmake_after.find("project(Clock VERSION 1.0.0") != std::string::npos);
    REQUIRE(cmake_after.find("find_package(Pulp 0.2.0 REQUIRED)") != std::string::npos);
    REQUIRE(toml_after.find("sdk_version = \"0.2.0\"") != std::string::npos);
    REQUIRE(toml_after.find("sdk_path = \"/custom/sdk\"") != std::string::npos);

    REQUIRE_FALSE(undo.timed_out);
    REQUIRE(undo.exit_code == 0);
    REQUIRE(undo.stdout_output.find("reverted") != std::string::npos);
    REQUIRE(read_file(project / "CMakeLists.txt").find("find_package(Pulp 0.1.0 REQUIRED)")
            != std::string::npos);
    REQUIRE(read_file(project / "pulp.toml").find("sdk_version = \"0.1.0\"")
            != std::string::npos);

    fs::remove_all(base);
}


// #591 Codex P2 / wave-4 sweep: `pulp docs build-site` must resolve
// `mkdocs.yml` from the project root, not the caller's CWD. This test
// invokes the CLI from inside a subdirectory (`tools/`) and asserts
// that the composed mkdocs command references the repo-root config —
// the previous bare `mkdocs build` would have failed with a
// "Config file 'mkdocs.yml' does not exist" error here.
//
// We don't require mkdocs to be installed: if it's missing, run exits
// non-zero with a "pip install -r requirements-docs.txt" hint on
// stderr, which is still a valid pass for this regression because the
// failure mode is "mkdocs not found", not "config file not found".
// What we must NOT see is a "Config file 'mkdocs.yml' does not exist"
// error, because that would mean `-f <root>/mkdocs.yml` wasn't passed.
TEST_CASE("pulp docs build-site resolves mkdocs.yml from project root",
          "[cli][shellout][docs][issue-591]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }

    // Walk up from the test CWD (<build>/test) to the repo root, then
    // drop into tools/ — a real subdirectory that exists in every
    // Pulp checkout. This mirrors the "user runs `pulp docs
    // build-site` from inside tools/" scenario Codex flagged.
    fs::path repo_root = fs::current_path() / ".." / "..";
    repo_root = fs::weakly_canonical(repo_root);
    if (!fs::exists(repo_root / "mkdocs.yml")) {
        SUCCEED("mkdocs.yml not at expected repo root — likely non-standard "
                "build layout; skipping");
        return;
    }
    fs::path subdir = repo_root / "tools";
    REQUIRE(fs::exists(subdir));

    // Pulp #597: pulp_binary() resolves from current_path() / "../tools/cli/pulp"
    // — that assumes the test is run from <build>/test. When we change
    // cwd below to <repo>/tools, the relative resolution points at
    // <repo>/tools/cli/pulp (source, not built), which doesn't exist,
    // and run_pulp silently returns exit_code=-1 without launching the
    // CLI — making the regression test vacuous. Resolve the absolute
    // binary path BEFORE the cwd change and pipe it through PULP_CLI_PATH,
    // which pulp_binary() honors as an explicit override.
    fs::path absolute_pulp_bin = fs::absolute(pulp_binary());
    REQUIRE(fs::exists(absolute_pulp_bin));
    // fs::path::c_str() returns const wchar_t* on Windows, so stringify
    // explicitly so the pulp_setenv(const char*, const char*, int) overload
    // resolves on every platform.
    const std::string absolute_pulp_bin_str = absolute_pulp_bin.string();
    pulp_setenv("PULP_CLI_PATH", absolute_pulp_bin_str.c_str(), 1);

    fs::path saved_cwd = fs::current_path();
    std::error_code ec;
    fs::current_path(subdir, ec);
    REQUIRE_FALSE(ec);

    // --strict is safe: if mkdocs runs at all, the build either
    // succeeds or surfaces a warnings-as-errors exit; neither produces
    // the "Config file 'mkdocs.yml' does not exist" string.
    auto r = run_pulp({"docs", "build-site", "--site-dir",
                      (repo_root / "build" / "site-shellout-test").string()},
                     60000);

    fs::current_path(saved_cwd, ec);  // always restore
    pulp_unsetenv("PULP_CLI_PATH");

    REQUIRE_FALSE(r.timed_out);
    // Permit mkdocs-missing ("command not found" path, rc != 0 with
    // install hint on stderr) but NOT "Config file 'mkdocs.yml' does
    // not exist" — that would mean the `-f <root>/mkdocs.yml` fix
    // regressed.
    const std::string combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("Config file 'mkdocs.yml' does not exist")
            == std::string::npos);
    REQUIRE(combined.find("does not exist; use --config-file")
            == std::string::npos);
}

TEST_CASE("pulp docs covers local reader index, search, open, and show paths",
          "[cli][shellout][docs][issue-643]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }

    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    SECTION("usage lists the local docs reader subcommands") {
        auto r = run_pulp({"docs"});
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.stdout_output.find("pulp docs") != std::string::npos);
        REQUIRE(r.stdout_output.find("show command") != std::string::npos);
        REQUIRE(r.stdout_output.find("build-site") != std::string::npos);
    }

    SECTION("index and open resolve docs-index slugs") {
        auto index = run_pulp({"docs", "index"});
        REQUIRE(index.exit_code == 0);
        REQUIRE_FALSE(index.timed_out);
        REQUIRE(index.stdout_output.find("Available Documentation") != std::string::npos);
        REQUIRE(index.stdout_output.find("getting-started") != std::string::npos);
        REQUIRE(index.stdout_output.find("docs/guides/getting-started.md") != std::string::npos);

        auto opened = run_pulp({"docs", "open", "getting-started"});
        REQUIRE(opened.exit_code == 0);
        REQUIRE_FALSE(opened.timed_out);
        REQUIRE(opened.stdout_output.find("Getting Started") != std::string::npos);
    }

    SECTION("search reports exact doc matches") {
        auto r = run_pulp({"docs", "search", "WebView"});
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE((r.stdout_output.find("match(es) found") != std::string::npos ||
                 r.stdout_output.find("docs/") != std::string::npos));

        auto multi_word = run_pulp({"docs", "search", "Getting", "Started"});
        REQUIRE(multi_word.exit_code == 0);
        REQUIRE_FALSE(multi_word.timed_out);
        REQUIRE(multi_word.stdout_output.find("getting-started") != std::string::npos);
    }

    SECTION("search reports fuzzy suggestions and empty results") {
        auto fuzzy = run_pulp({"docs", "search", "gttngstrtd"});
        REQUIRE(fuzzy.exit_code == 0);
        REQUIRE_FALSE(fuzzy.timed_out);
        REQUIRE(fuzzy.stdout_output.find("No exact matches") != std::string::npos);
        REQUIRE(fuzzy.stdout_output.find("Did you mean") != std::string::npos);
        REQUIRE(fuzzy.stdout_output.find("getting-started") != std::string::npos);

        auto empty = run_pulp({"docs", "search", "zzzz-no-doc-match"});
        REQUIRE(empty.exit_code == 0);
        REQUIRE_FALSE(empty.timed_out);
        REQUIRE(empty.stdout_output.find("No matches for") != std::string::npos);
    }

    SECTION("show reads support, command, cmake, and style manifests") {
        auto support = run_pulp({"docs", "show", "support", "vst3"});
        REQUIRE(support.exit_code == 0);
        REQUIRE_FALSE(support.timed_out);
        REQUIRE(support.stdout_output.find("Support info") != std::string::npos);
        REQUIRE(support.stdout_output.find("vst3") != std::string::npos);

        auto command = run_pulp({"docs", "show", "command", "ship"});
        REQUIRE(command.exit_code == 0);
        REQUIRE_FALSE(command.timed_out);
        REQUIRE(command.stdout_output.find("Command: ship") != std::string::npos);
        REQUIRE(command.stdout_output.find("Subcommands") != std::string::npos);
        REQUIRE(command.stdout_output.find("package") != std::string::npos);

        auto cmake = run_pulp({"docs", "show", "cmake", "pulp_add_plugin"});
        REQUIRE(cmake.exit_code == 0);
        REQUIRE_FALSE(cmake.timed_out);
        REQUIRE(cmake.stdout_output.find("CMake function: pulp_add_plugin") != std::string::npos);

        auto style = run_pulp({"docs", "show", "style"});
        REQUIRE(style.exit_code == 0);
        REQUIRE_FALSE(style.timed_out);
        REQUIRE(style.stdout_output.find("Style Rules") != std::string::npos);
        REQUIRE(style.stdout_output.find("public_headers_only") != std::string::npos);
    }

    SECTION("reader errors include actionable diagnostics") {
        auto search_usage = run_pulp({"docs", "search"});
        REQUIRE(search_usage.exit_code != 0);
        REQUIRE(search_usage.stderr_output.find("Usage: pulp docs search") != std::string::npos);

        auto open_usage = run_pulp({"docs", "open"});
        REQUIRE(open_usage.exit_code != 0);
        REQUIRE(open_usage.stderr_output.find("Usage: pulp docs open") != std::string::npos);

        auto missing_slug = run_pulp({"docs", "open", "not-a-real-doc"});
        REQUIRE(missing_slug.exit_code != 0);
        REQUIRE(missing_slug.stderr_output.find("no doc found") != std::string::npos);

        auto show_usage = run_pulp({"docs", "show"});
        REQUIRE(show_usage.exit_code != 0);
        REQUIRE(show_usage.stderr_output.find("Usage: pulp docs show") != std::string::npos);

        auto support_usage = run_pulp({"docs", "show", "support"});
        REQUIRE(support_usage.exit_code != 0);
        REQUIRE(support_usage.stderr_output.find("Usage: pulp docs show support") != std::string::npos);

        auto command_usage = run_pulp({"docs", "show", "command"});
        REQUIRE(command_usage.exit_code != 0);
        REQUIRE(command_usage.stderr_output.find("Usage: pulp docs show command") != std::string::npos);

        auto cmake_usage = run_pulp({"docs", "show", "cmake"});
        REQUIRE(cmake_usage.exit_code != 0);
        REQUIRE(cmake_usage.stderr_output.find("Usage: pulp docs show cmake") != std::string::npos);

        auto unknown_show = run_pulp({"docs", "show", "widget"});
        REQUIRE(unknown_show.exit_code != 0);
        REQUIRE(unknown_show.stderr_output.find("Unknown show topic") != std::string::npos);

        auto unknown_subcommand = run_pulp({"docs", "wat"});
        REQUIRE(unknown_subcommand.exit_code != 0);
        REQUIRE(unknown_subcommand.stderr_output.find("Unknown docs subcommand") != std::string::npos);
    }

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
}

// pulp #914 — `pulp run --help` must advertise the four new flags so
// users (and the docs generator) can discover them. This is the help
// surface contract; the parser is exercised in test_cli_run_options.cpp.
TEST_CASE("pulp run --help advertises the headless/screenshot/frames/watch flags",
          "[cli][shellout][run][issue-914]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"run", "--help"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("--headless")   != std::string::npos);
    REQUIRE(r.stdout_output.find("--screenshot") != std::string::npos);
    REQUIRE(r.stdout_output.find("--frames")     != std::string::npos);
    REQUIRE(r.stdout_output.find("--watch")      != std::string::npos);
    // Make sure the existing "active project build" line stays — the
    // root CMakeLists.txt regex test depends on it.
    REQUIRE(r.stdout_output.find("active project build") != std::string::npos);
}

// pulp #914 — bad `--frames` is caught before the run path tries to
// resolve a project, so we get a clean exit-2 with a diagnostic.
TEST_CASE("pulp run --frames rejects non-positive / non-integer values",
          "[cli][shellout][run][issue-914]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto bad_int = run_pulp({"run", "--frames", "notanumber"});
    REQUIRE_FALSE(bad_int.timed_out);
    REQUIRE(bad_int.exit_code == 2);
    REQUIRE(bad_int.stderr_output.find("--frames") != std::string::npos);

    auto zero = run_pulp({"run", "--frames", "0"});
    REQUIRE_FALSE(zero.timed_out);
    REQUIRE(zero.exit_code == 2);
    REQUIRE(zero.stderr_output.find("--frames") != std::string::npos);

    auto missing_path = run_pulp({"run", "--screenshot"});
    REQUIRE_FALSE(missing_path.timed_out);
    REQUIRE(missing_path.exit_code == 2);
    REQUIRE(missing_path.stderr_output.find("--screenshot") != std::string::npos);
}

// pulp #914 — end-to-end CI-validation contract: `pulp run --headless
// --screenshot <path> --frames 1 <target>` against a fake project that
// contains the test fixture binary under build/examples/<dir>/<exe>
// must (a) discover the binary, (b) launch it with the flags forwarded
// AND env vars set, and (c) leave a non-empty PNG file at <path>.
TEST_CASE("pulp run --headless --screenshot --frames writes a PNG",
          "[cli][shellout][run][issue-914]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // The fixture binary lives next to the test binary in the build
    // tree (test/pulp-cli-run-fixture). The test runner's cwd is
    // <build>/test, so a relative resolve to the fixture works.
    fs::path fixture_src = fs::current_path() /
#if defined(_WIN32)
        "pulp-cli-run-fixture.exe";
#else
        "pulp-cli-run-fixture";
#endif
    if (!fs::exists(fixture_src)) {
        SUCCEED("fixture binary not built at " + fixture_src.string()
                + "; skipping");
        return;
    }

    // Build a fake project tree that cmd_run can navigate.
    auto base = fs::temp_directory_path() /
                ("pulp-shellout-run-headless-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto build_dir = base / "build";
    auto bin_dir = build_dir / "bin";
    fs::create_directories(bin_dir);
    // pulp.toml at the root marks this as a standalone project (no
    // `core/` sibling needed) — find_standalone_root() picks it up.
    {
        std::ofstream toml(base / "pulp.toml");
        toml << "[pulp]\nsdk_version = \"99.0.0\"\n";
    }
    // Stub CMakeCache.txt — cmd_run only checks for existence.
    { std::ofstream c(build_dir / "CMakeCache.txt"); c << "# stub\n"; }

    // Copy the fixture in as the discovered binary. In standalone mode
    // cmd_run scans build/bin for a single executable file with no
    // extension. Use a name without dots and without "test" so the
    // selector picks it up.
    auto target = bin_dir /
#if defined(_WIN32)
        "pulpcliruntarget.exe";
#else
        "pulpcliruntarget";
#endif
    std::error_code copy_ec;
    fs::copy_file(fixture_src, target,
                   fs::copy_options::overwrite_existing, copy_ec);
    REQUIRE_FALSE(copy_ec);
#if !defined(_WIN32)
    fs::permissions(target,
                    fs::perms::owner_all | fs::perms::group_read |
                    fs::perms::group_exec | fs::perms::others_read |
                    fs::perms::others_exec,
                    fs::perm_options::add);
#endif

    auto screenshot = base / "shot.png";
    fs::remove(screenshot);

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(base);
    // Pass the explicit target name so cmd_run's standalone-mode lookup
    // path is taken (it matches by name without the "no dot in fname"
    // filter the unnamed search applies — that filter blocks the
    // Windows .exe extension).
    auto r = exec(bin.string(),
                  {"run", "pulpcliruntarget",
                   "--headless", "--screenshot", screenshot.string(),
                   "--frames", "1"},
                  20000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(r.timed_out);
    INFO("stdout: " << r.stdout_output);
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.exit_code == 0);

    // Fixture echoes its resolved options — verify the CLI forwarded
    // both the args (preferred) AND the env vars (fallback).
    REQUIRE(r.stdout_output.find("fixture: headless=1") != std::string::npos);
    REQUIRE(r.stdout_output.find(screenshot.string())   != std::string::npos);

    // The PNG itself must exist and start with the standard signature.
    REQUIRE(fs::exists(screenshot));
    auto size = fs::file_size(screenshot);
    REQUIRE(size > 0);
    std::ifstream png(screenshot, std::ios::binary);
    unsigned char hdr[8] = {0};
    png.read(reinterpret_cast<char*>(hdr), 8);
    REQUIRE(hdr[0] == 0x89);
    REQUIRE(hdr[1] == 'P');
    REQUIRE(hdr[2] == 'N');
    REQUIRE(hdr[3] == 'G');

    fs::remove_all(base);
}

// #682 — PULP_DEBUG=1 must emit timestamped phase markers to stderr so
// future "pulp hung at 0% CPU" reports pin themselves. Unset by default
// it must stay silent so scripts that parse stderr aren't affected.
TEST_CASE("PULP_DEBUG=1 surfaces phase markers to stderr (#682)",
          "[cli][shellout][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Disable the update-check network path so the test stays offline
    // and deterministic — we're testing the instrumentation hook, not
    // the network dependency.
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    SECTION("unset: stderr stays free of phase markers") {
        pulp_unsetenv("PULP_DEBUG");
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") == std::string::npos);
    }

    SECTION("PULP_DEBUG=1: emits markers on stderr, not stdout") {
        pulp_setenv("PULP_DEBUG", "1", 1);
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") != std::string::npos);
        REQUIRE(r.stderr_output.find("update-banner") != std::string::npos);
        REQUIRE(r.stdout_output.find("[pulp-debug") == std::string::npos);
        pulp_unsetenv("PULP_DEBUG");
    }

    SECTION("PULP_DEBUG=0: treated as off, no markers") {
        pulp_setenv("PULP_DEBUG", "0", 1);
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") == std::string::npos);
        pulp_unsetenv("PULP_DEBUG");
    }

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
}
