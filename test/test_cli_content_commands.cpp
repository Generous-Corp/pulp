// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/content_commands.hpp"
#include "../tools/cli/kit_commands.hpp"

#include "../external/miniz/miniz.h"

#include <pulp/runtime/crypto.hpp>
#include <pulp/state/content_registry.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pulp::cli::content::cmd_content;
using pulp::state::ContentCapabilityManifest;
using pulp::state::ContentRegistry;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<int> seq{0};
        path = fs::temp_directory_path() /
               ("pulp-cli-content-test-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                std::to_string(seq.fetch_add(1)));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path repo_root() {
#ifdef PULP_SOURCE_DIR
    return fs::path(PULP_SOURCE_DIR);
#else
    return fs::current_path();
#endif
}

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string capture_stdout_for(const std::function<int()>& fn, int& exit_code) {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    exit_code = fn();
    std::cout.rdbuf(old);
    return captured.str();
}

fs::path write_archive_without_hashes(const fs::path& archive,
                                      const fs::path& source_root) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0));
    std::error_code ec;
    for (fs::recursive_directory_iterator it(source_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rel = fs::relative(it->path(), source_root, ec).generic_string();
        std::ifstream file(it->path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(mz_zip_writer_add_mem(&zip, rel.c_str(), body.data(), body.size(),
                                      MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return archive;
}

fs::path write_archive_with_unlisted_payload(const fs::path& archive,
                                             const fs::path& source_root) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0));
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> hashes;
    for (fs::recursive_directory_iterator it(source_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rel = fs::relative(it->path(), source_root, ec).generic_string();
        std::ifstream file(it->path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(mz_zip_writer_add_mem(&zip, rel.c_str(), body.data(), body.size(),
                                      MZ_DEFAULT_COMPRESSION));
        hashes.push_back({rel, "sha256-" + pulp::runtime::sha256_hex(body)});
    }
    std::sort(hashes.begin(), hashes.end());
    std::string sha_manifest = "{\n  \"schema\": \"pulp-files-sha256-v1\",\n  \"files\": {";
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        sha_manifest += i == 0 ? "\n" : ",\n";
        sha_manifest += "    \"" + hashes[i].first + "\": \"" + hashes[i].second + "\"";
    }
    sha_manifest += "\n  }\n}\n";
    REQUIRE(mz_zip_writer_add_mem(&zip, "files.sha256.json",
                                  sha_manifest.data(), sha_manifest.size(),
                                  MZ_DEFAULT_COMPRESSION));
    const std::string extra = "not declared in files.sha256.json";
    REQUIRE(mz_zip_writer_add_mem(&zip, "extras/unlisted.txt",
                                  extra.data(), extra.size(),
                                  MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return archive;
}

}  // namespace

TEST_CASE("pulp content validates the content-pack fixture",
          "[cli][content]") {
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    REQUIRE(cmd_content({"validate", fixture.string(), "--json"}) == 0);
}

TEST_CASE("pulp content installs, lists, reveals, and removes data-only packs",
          "[cli][content]") {
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);

    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    REQUIRE(fs::exists(installed / "pulp.package.json"));
    REQUIRE(fs::exists(installed / "presets" / "init.json"));
    REQUIRE(fs::exists(installed / "themes" / "dark.json"));
    REQUIRE(fs::exists(installed / "samples" / "kick.wav"));
    REQUIRE(fs::exists(installed / "wavetables" / "sine.wavetable.json"));
    REQUIRE(fs::exists(data.path / "Content" / "index.json"));
    const auto manifest_sha =
        "sha256-" + pulp::runtime::sha256_hex(read_file(fixture / "pulp.package.json"));
    const auto index_after_install = read_file(data.path / "Content" / "index.json");
    REQUIRE(index_after_install.find(pack_id) != std::string::npos);
    REQUIRE(index_after_install.find(R"("plugin_id":")" + plugin + R"(")")
            != std::string::npos);
    REQUIRE(index_after_install.find(R"("manifest_sha256":")" + manifest_sha + R"(")")
            != std::string::npos);

    REQUIRE(cmd_content({"list", "--plugin", plugin, "--root", data.path.string(), "--json"}) == 0);
    REQUIRE(cmd_content({"reveal", pack_id, "--plugin", plugin,
                         "--version", "0.1.0", "--root", data.path.string()}) == 0);

    write_file(data.path / "Content" / plugin / "UserPresets" / "mine.json", "{}\n");
    REQUIRE(cmd_content({"remove", pack_id, "--plugin", plugin,
                         "--version", "0.1.0", "--root", data.path.string(), "--yes"}) == 0);
    REQUIRE_FALSE(fs::exists(installed));
    REQUIRE(fs::exists(data.path / "Content" / plugin / "UserPresets" / "mine.json"));
}

TEST_CASE("pulp content install writes the index atomically with no temp sidecar",
          "[cli][content][reliability]") {
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);

    const auto index = data.path / "Content" / "index.json";
    REQUIRE(fs::exists(index));
    // The atomic temp-then-rename must leave the index in place and never leak
    // its sibling staging file.
    REQUIRE_FALSE(fs::exists(data.path / "Content" / "index.json.tmp"));
    // A fully-written (not truncated) index parses as valid JSON.
    REQUIRE_NOTHROW(choc::json::parse(read_file(index)));
}

TEST_CASE("pulp content install refuses to overwrite an installed pack version",
          "[cli][content]") {
    TempDir data;
    TempDir changed_pack;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    const auto original = read_file(installed / "presets" / "init.json");

    fs::copy(fixture, changed_pack.path,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    write_file(changed_pack.path / "presets" / "init.json", "{\"name\":\"Should Not Install\"}\n");

    REQUIRE(cmd_content({"install", changed_pack.path.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE(read_file(installed / "presets" / "init.json") == original);
}

TEST_CASE("pulp content install is visible through ContentRegistry",
          "[cli][content]") {
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);

    ContentCapabilityManifest manifest;
    manifest.plugin_id = plugin;
    manifest.capabilities = {
        "content.presets.v1",
        "content.samples.v1",
        "content.wavetables.v1"
    };
    manifest.content_kinds = {"presets", "samples", "wavetables"};

    ContentRegistry registry(data.path);
    auto packs = registry.packs_for_plugin(manifest);
    REQUIRE(packs.size() == 1);
    REQUIRE(packs.front().id == "dev.pulp.fixtures.basic-content-pack");
    REQUIRE(packs.front().presets.size() == 1);
    REQUIRE(packs.front().samples.size() == 1);
    REQUIRE(packs.front().wavetables.size() == 1);

    auto presets = registry.presets_for_plugin(manifest);
    REQUIRE(presets.size() == 1);
    REQUIRE(presets.front().is_factory);
    REQUIRE(presets.front().folder == "Basic Content Pack");
}

TEST_CASE("pulp content commands reject unsafe path components before mutation",
          "[cli][content][security]") {
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", "../outside",
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(data.path / "Content"));
    REQUIRE_FALSE(fs::exists(data.path / "outside"));

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", "bad:plugin",
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(data.path / "Content"));

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    REQUIRE(fs::exists(installed / "pulp.package.json"));

    REQUIRE(cmd_content({"update", fixture.string(), "--plugin", "bad/plugin",
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE(fs::exists(installed / "pulp.package.json"));

    REQUIRE(cmd_content({"remove", "../outside", "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE(fs::exists(installed / "pulp.package.json"));

    REQUIRE(cmd_content({"remove", pack_id, "--plugin", plugin,
                         "--version", "../0.1.0", "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE(fs::exists(installed / "pulp.package.json"));

    REQUIRE(cmd_content({"reveal", pack_id, "--plugin", "../outside",
                         "--version", "0.1.0", "--root", data.path.string()}) == 1);
    REQUIRE(cmd_content({"reveal", pack_id, "--plugin", plugin,
                         "--version", "0.1.0", "--root", data.path.string()}) == 0);
}

TEST_CASE("pulp content install rejects symlinked directory entries before copy",
          "[cli][content][security]") {
    TempDir data;
    TempDir pack;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    fs::copy(fixture, pack.path,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    std::error_code ec;
    fs::remove(pack.path / "presets" / "init.json", ec);
    fs::create_symlink(fixture / "presets" / "init.json",
                       pack.path / "presets" / "init.json",
                       ec);
    if (ec) {
        SUCCEED("filesystem does not allow symlink creation in this test environment");
        return;
    }

    REQUIRE(cmd_content({"install", pack.path.string(),
                         "--plugin", "dev.pulp.fixtures.content-target",
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(data.path / "Content"));
}

TEST_CASE("pulp content install and update reject bare manifest files before mutation",
          "[cli][content][security]") {
    TempDir data;
    TempDir pack;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    fs::copy(fixture, pack.path,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    write_file(pack.path / "unrelated-secret.txt", "not content\n");

    REQUIRE(cmd_content({"install", (pack.path / "pulp.package.json").string(),
                         "--plugin", plugin,
                         "--root", data.path.string(),
                         "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(data.path / "Content"));

    REQUIRE(cmd_content({"install", pack.path.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    const auto original = read_file(installed / "presets" / "init.json");

    write_file(pack.path / "presets" / "init.json", "{\"name\":\"Should Not Update\"}\n");
    REQUIRE(cmd_content({"update", (pack.path / "pulp.package.json").string(),
                         "--plugin", plugin,
                         "--root", data.path.string(),
                         "--yes"}) == 1);
    REQUIRE(read_file(installed / "presets" / "init.json") == original);
    REQUIRE_FALSE(fs::exists(installed / "unrelated-secret.txt"));
}

TEST_CASE("pulp content preview reports runtime compatibility and reload policy",
          "[cli][content]") {
    TempDir tmp;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const auto runtime = tmp.path / "pulp.plugin-runtime.json";
    write_file(runtime, R"json({
  "schema": "pulp.plugin-runtime.v1",
  "pluginId": "dev.pulp.fixtures.content-target",
  "content": {
    "capabilities": ["content.presets.v1", "content.themes.v1"],
    "kinds": ["presets", "themes"],
    "reload": {
      "hotReloadKinds": ["themes"],
      "manualRescanKinds": ["presets"]
    }
  }
})json");

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_content({"preview", fixture.string(),
                            "--plugin-runtime", runtime.string(),
                            "--plugin", "dev.pulp.fixtures.content-target",
                            "--json"});
    }, exit_code);

    REQUIRE(exit_code == 0);
    REQUIRE(output.find("\"ok\":true") != std::string::npos);
    REQUIRE(output.find("\"plugin_id\":\"dev.pulp.fixtures.content-target\"") != std::string::npos);
    REQUIRE(output.find("\"kind\":\"presets\",\"policy\":\"manual-rescan\"") != std::string::npos);
    REQUIRE(output.find("\"kind\":\"themes\",\"policy\":\"hot-reload\"") != std::string::npos);
    REQUIRE(output.find("\"requires_restart\":false") != std::string::npos);

    const auto mismatch = capture_stdout_for([&] {
        return cmd_content({"preview", fixture.string(),
                            "--plugin-runtime", runtime.string(),
                            "--plugin", "dev.pulp.other",
                            "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(mismatch.find("plugin-mismatch") != std::string::npos);
}

TEST_CASE("pulp content update replaces a local pack version without touching user files",
          "[cli][content]") {
    TempDir data;
    TempDir updated_pack;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    write_file(data.path / "Content" / plugin / "UserPresets" / "mine.json", "{}\n");

    fs::copy(fixture, updated_pack.path,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    write_file(updated_pack.path / "presets" / "init.json", "{\"name\":\"Updated\"}\n");

    REQUIRE(cmd_content({"update", updated_pack.path.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    REQUIRE(read_file(installed / "presets" / "init.json").find("Updated") != std::string::npos);
    REQUIRE(fs::exists(data.path / "Content" / plugin / "UserPresets" / "mine.json"));
    REQUIRE_FALSE(fs::exists(installed.parent_path() /
                             ".pulp-content-update-backup-dev.pulp.fixtures.basic-content-pack-0.1.0"));
    const auto index = read_file(data.path / "Content" / "index.json");
    REQUIRE(index.find(pack_id) != std::string::npos);
    REQUIRE(index.find("sha256-" + pulp::runtime::sha256_hex(read_file(updated_pack.path / "pulp.package.json")))
            != std::string::npos);
    REQUIRE(index.find(".pulp-content-update-backup") == std::string::npos);
}

TEST_CASE("pulp content update rejects invalid packs before replacing installed content",
          "[cli][content]") {
    TempDir data;
    TempDir invalid_pack;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    const auto original = read_file(installed / "presets" / "init.json");

    write_file(invalid_pack.path / "pulp.package.json", R"json({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.fixtures.basic-content-pack",
  "name": "Invalid Content Pack",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {"presets": "MIT"},
  "kind": ["source"],
  "capabilities": ["content.presets.v1"],
  "exports": {"presets": ["presets"]},
  "dependencies": {"pulp": [], "packages": []},
  "validation": {},
  "authoring": {
    "createdBy": {"type": "human", "name": "Pulp Tests"},
    "humanReview": {"reviewed": true, "reviewer": "Pulp Tests"}
  }
})json");

    REQUIRE(cmd_content({"update", invalid_pack.path.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE(fs::exists(installed / "pulp.package.json"));
    REQUIRE(read_file(installed / "presets" / "init.json") == original);
}

TEST_CASE("pulp content rescan rebuilds the content index without touching packs",
          "[cli][content]") {
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const std::string plugin = "dev.pulp.fixtures.content-target";
    const std::string pack_id = "dev.pulp.fixtures.basic-content-pack";

    REQUIRE(cmd_content({"install", fixture.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);

    const auto installed = data.path / "Content" / plugin / pack_id / "0.1.0";
    const auto index = data.path / "Content" / "index.json";
    REQUIRE(fs::exists(installed / "pulp.package.json"));
    fs::remove(index);
    REQUIRE_FALSE(fs::exists(index));

    REQUIRE(cmd_content({"rescan", "--root", data.path.string(), "--json"}) == 0);
    REQUIRE(fs::exists(index));
    const auto index_text = read_file(index);
    REQUIRE(index_text.find(pack_id) != std::string::npos);
    REQUIRE(index_text.find("sha256-" + pulp::runtime::sha256_hex(read_file(installed / "pulp.package.json")))
            != std::string::npos);
    REQUIRE(index_text.find("Content/" + plugin + "/" + pack_id + "/0.1.0") != std::string::npos);
    REQUIRE(fs::exists(installed / "presets" / "init.json"));
    REQUIRE(fs::exists(installed / "samples" / "kick.wav"));
    REQUIRE(fs::exists(installed / "wavetables" / "sine.wavetable.json"));
}

TEST_CASE("pulp content validates and installs a packed .pulpcontent archive",
          "[cli][content]") {
    TempDir tmp;
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const auto archive = tmp.path / "basic-content-pack.pulpcontent";
    const std::string plugin = "dev.pulp.fixtures.content-target";

    REQUIRE(pulp::cli::kit::cmd_kit({"pack", fixture.string(), "--output", archive.string(), "--json"}) == 0);
    REQUIRE(fs::exists(archive));
    REQUIRE(cmd_content({"validate", archive.string(), "--json"}) == 0);
    REQUIRE(cmd_content({"install", archive.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 0);
    REQUIRE(fs::exists(data.path / "Content" / plugin /
                       "dev.pulp.fixtures.basic-content-pack" / "0.1.0" /
                       "files.sha256.json"));
    REQUIRE(fs::exists(data.path / "Content" / plugin /
                       "dev.pulp.fixtures.basic-content-pack" / "0.1.0" /
                       "samples" / "kick.wav"));
    REQUIRE(fs::exists(data.path / "Content" / plugin /
                       "dev.pulp.fixtures.basic-content-pack" / "0.1.0" /
                       "wavetables" / "sine.wavetable.json"));
}

TEST_CASE("pulp content rejects archives without files.sha256 manifests",
          "[cli][content][security]") {
    TempDir tmp;
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const auto archive = write_archive_without_hashes(tmp.path / "no-hashes.pulpcontent",
                                                      fixture);
    const std::string plugin = "dev.pulp.fixtures.content-target";

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_content({"validate", archive.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find("sha256: missing files.sha256.json") != std::string::npos);

    REQUIRE(cmd_content({"install", archive.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(data.path / "Content"));
}

TEST_CASE("pulp content rejects archives with unlisted payload files",
          "[cli][content][security]") {
    TempDir tmp;
    TempDir data;
    const auto fixture = repo_root() / "fixtures/packages/basic-content-pack";
    const auto archive = write_archive_with_unlisted_payload(
        tmp.path / "unlisted-payload.pulpcontent", fixture);
    const std::string plugin = "dev.pulp.fixtures.content-target";

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_content({"validate", archive.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find("sha256: unlisted archived file `extras/unlisted.txt`") != std::string::npos);

    REQUIRE(cmd_content({"install", archive.string(), "--plugin", plugin,
                         "--root", data.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(data.path / "Content"));
}

TEST_CASE("pulp content validation catches missing sample and wavetable exports",
          "[cli][content]") {
    TempDir pack;
    write_file(pack.path / "pulp.package.json", R"json({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.missing-rich-content",
  "name": "Missing Rich Content",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "samples": "MIT",
    "wavetables": "MIT"
  },
  "kind": ["content-pack"],
  "capabilities": ["content.samples.v1", "content.wavetables.v1"],
  "exports": {
    "samples": ["samples"],
    "wavetables": ["wavetables"]
  },
  "dependencies": {"pulp": [], "packages": []},
  "validation": {},
  "authoring": {
    "createdBy": {"type": "human", "name": "Pulp Tests"},
    "humanReview": {"reviewed": true, "reviewer": "Pulp Tests"}
  }
})json");

    REQUIRE(cmd_content({"validate", pack.path.string(), "--json"}) == 1);
}
