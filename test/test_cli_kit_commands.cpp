// SPDX-License-Identifier: MIT
// Focused tests for `pulp kit` metadata-only validation.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/cli_fs_util.hpp"
#include "../tools/cli/kit_commands.hpp"

#include <pulp/runtime/crypto.hpp>

#include "../external/miniz/miniz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::cli::kit;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<int> seq{0};
        path = fs::temp_directory_path() /
               ("pulp-cli-kit-test-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                std::to_string(seq.fetch_add(1)));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << body;
}

bool has_issue(const KitValidationResult& result, const std::string& code) {
    for (const auto& issue : result.issues)
        if (issue.code == code) return true;
    return false;
}

fs::path repo_root() {
#ifdef PULP_SOURCE_DIR
    return fs::path(PULP_SOURCE_DIR);
#else
    return fs::current_path();
#endif
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string replace_all(std::string value,
                        const std::string& from,
                        const std::string& to) {
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::string read_zip_entry(const fs::path& zip_path, const std::string& entry) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &size, 0);
    std::string out;
    if (data) {
        out.assign(static_cast<const char*>(data), size);
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return out;
}

fs::path write_archive_with_optional_hash_manifest(const fs::path& archive,
                                                   const fs::path& source_root,
                                                   bool include_hash_manifest,
                                                   bool add_unlisted_payload) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0));
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> hashes;
    for (fs::recursive_directory_iterator it(source_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rel = fs::relative(it->path(), source_root, ec).generic_string();
        if (rel == "files.sha256.json") continue;
        std::ifstream file(it->path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(mz_zip_writer_add_mem(&zip, rel.c_str(), body.data(), body.size(),
                                      MZ_DEFAULT_COMPRESSION));
        hashes.push_back({rel, "sha256-" + pulp::runtime::sha256_hex(body)});
    }
    if (include_hash_manifest) {
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
    }
    if (add_unlisted_payload) {
        const std::string extra = "not declared in files.sha256.json";
        REQUIRE(mz_zip_writer_add_mem(&zip, "extras/unlisted.txt",
                                      extra.data(), extra.size(),
                                      MZ_DEFAULT_COMPRESSION));
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return archive;
}

std::string capture_stdout_for(const std::function<int()>& fn, int& exit_code) {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    exit_code = fn();
    std::cout.rdbuf(old);
    return captured.str();
}

std::string hex_encode(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    for (const auto byte : bytes)
        out << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(byte);
    return out.str();
}

fs::path write_registry_manifest(const fs::path& kit_root) {
    const auto manifest_text = read_file(kit_root / "pulp.package.json");
    const auto canonical_sha = "sha256-" + pulp::runtime::sha256_hex(manifest_text);
    std::array<std::uint8_t, pulp::runtime::ed25519_seed_size> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(i + 1);
    auto keypair = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(keypair.has_value());
    const auto message = std::string("pulp-registry-manifest-v1\n")
        + "dev.pulp.fixtures.basic-ui-kit\n"
        + "0.1.0\n"
        + canonical_sha + "\n";
    auto signature = pulp::runtime::ed25519_sign(
        keypair->private_key.data(), keypair->private_key.size(),
        reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    REQUIRE(signature.has_value());

    const auto path = kit_root / "registry" / "pulp-registry-manifest.json";
    write_file(path, std::string("{\n")
        + "  \"schema\": \"pulp-registry-manifest-v1\",\n"
        + "  \"id\": \"dev.pulp.fixtures.basic-ui-kit\",\n"
        + "  \"version\": \"0.1.0\",\n"
        + "  \"canonicalManifestSha256\": \"" + canonical_sha + "\",\n"
        + "  \"signerPublicKey\": \"" + hex_encode(keypair->public_key) + "\",\n"
        + "  \"signature\": \"" + hex_encode(*signature) + "\"\n"
        + "}\n");
    return path;
}

std::string quote_for_shell(const fs::path& path) {
    std::string s = path.string();
#ifdef _WIN32
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

fs::path screenshot_tool_path_for_test(const fs::path& project_root) {
    // Mirror default_screenshot_tool_for_project(): `.exe` on Windows (checked
    // first there), bare name on POSIX.
#ifdef _WIN32
    return project_root / "build" / "tools" / "screenshot" / "pulp-screenshot.exe";
#else
    return project_root / "build" / "tools" / "screenshot" / "pulp-screenshot";
#endif
}

void set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Install the compiled fake `pulp-screenshot` into the project's expected tool
// location and record the byte payload it should emit. The payload is passed
// through the `PULP_FAKE_SCREENSHOT_PAYLOAD` environment variable (the kit
// verifier spawns the tool via std::system, which inherits our environment); a
// `pulp-screenshot.bytes` sidecar is written as a fallback. Using a real
// executable (built as the `pulp-fake-screenshot-tool` target) keeps the
// kit-verify screenshot-execution path identical across platforms; generated
// .cmd/.sh shims were Windows-flaky.
void write_fake_screenshot_tool(const fs::path& project_root, const std::string& bytes) {
    const auto screenshot_tool = screenshot_tool_path_for_test(project_root);
    std::error_code ec;
    fs::create_directories(screenshot_tool.parent_path(), ec);
    fs::copy_file(fs::path(PULP_FAKE_SCREENSHOT_TOOL), screenshot_tool,
                  fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    fs::permissions(screenshot_tool,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add, ec);
    write_file(screenshot_tool.parent_path() / "pulp-screenshot.bytes", bytes);
    set_env_var("PULP_FAKE_SCREENSHOT_PAYLOAD", bytes);
}

// Collect every render log written under the project's kit-validation tree, so
// a screenshot-execution failure surfaces the tool's own diagnostics in the
// Catch2 failure message (the render logs are not uploaded as CI artifacts on
// every platform).
std::string collect_render_logs(const fs::path& project_root) {
    const auto root = project_root / ".pulp" / "kit-validation";
    std::error_code ec;
    if (!fs::exists(root, ec)) return "(no kit-validation directory)";
    std::string out;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".log") {
            std::ifstream in(it->path(), std::ios::binary);
            out += "--- " + it->path().string() + " ---\n";
            out += std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
            out += "\n";
        }
    }
    return out.empty() ? "(no render logs found)" : out;
}

std::string fake_screenshot_bytes(std::string bytes) {
    return bytes;
}

int run_success_command(const std::string& cmd) {
    return std::system(cmd.c_str());
}

}  // namespace

TEST_CASE("pulp kit validates metadata-only kit fixture manifests",
          "[cli][kit]") {
    const auto root = repo_root();
    for (const auto& rel : {
             "fixtures/packages/gain-dsp-kit",
             "fixtures/packages/basic-ui-kit",
             "fixtures/packages/simple-plugin-template",
             "fixtures/packages/level-graph-node-kit",
             "fixtures/packages/signed-node-pack-kit",
             "fixtures/packages/native-component-kit",
         }) {
        INFO(rel);
        auto result = validate_manifest_path(root / rel);
        REQUIRE(result.ok());
        REQUIRE(result.summary.schema == "pulp-package-v1");
        REQUIRE_FALSE(result.summary.id.empty());
        REQUIRE_FALSE(result.summary.kinds.empty());
    }
}

TEST_CASE("pulp kit rejects incompatible requires.pulp constraints",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.future-sdk-kit",
  "name": "Future SDK Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">999.0.0",
    "cpp": 20,
    "platforms": ["macOS"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(std::any_of(result.issues.begin(), result.issues.end(), [](const auto& issue) {
        return issue.code == "sdk-incompatible";
    }));
}

TEST_CASE("pulp kit rejects incompatible requires.cpp constraints",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.future-cpp-kit",
  "name": "Future Cpp Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 26,
    "platforms": ["macOS"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "cpp-incompatible"));
}

TEST_CASE("pulp kit rejects malformed requires.cpp constraints",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-cpp-kit",
  "name": "Bad Cpp Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": "20",
    "platforms": ["macOS"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-cpp-requirement"));
}

TEST_CASE("pulp kit rejects fractional requires.cpp constraints",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.fractional-cpp-kit",
  "name": "Fractional Cpp Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20.5,
    "platforms": ["macOS"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-cpp-requirement"));
}

TEST_CASE("pulp kit rejects unknown Pulp module dependencies",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.unknown-pulp-module-kit",
  "name": "Unknown Pulp Module Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::definitely-not-a-module"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "unknown-pulp-module-dependency"));
}

TEST_CASE("pulp kit rejects manifest array entries that violate the schema",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-array-schema-kit",
  "name": "Bad Array Schema Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT"
  },
  "kind": ["source", "space-station", 42],
  "audience": ["developer", "robot", false],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Plan9", 99]
  },
  "capabilities": ["audio.effect.test", {}],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": ["rubberband", 17]
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-kind"));
    REQUIRE(has_issue(result, "invalid-audience"));
    REQUIRE(has_issue(result, "invalid-platform"));
    REQUIRE(has_issue(result, "invalid-capability"));
    REQUIRE(has_issue(result, "invalid-dependency-package"));
}

TEST_CASE("pulp kit rejects malformed Pulp module dependencies",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-pulp-module-kit",
  "name": "Bad Pulp Module Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": "pulp::signal",
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-pulp-module-dependency"));
}

TEST_CASE("pulp kit search separates local kit and content lanes",
          "[cli][kit]") {
    const auto root = repo_root() / "fixtures/packages";

    int exit_code = 0;
    const auto kits = capture_stdout_for([&] {
        return cmd_kit({"search", "basic", "--root", root.string(), "--lane", "kit", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(kits.find(R"("lane":"kit")") != std::string::npos);
    REQUIRE(kits.find("dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    REQUIRE(kits.find(R"("lane":"content")") == std::string::npos);

    const auto content = capture_stdout_for([&] {
        return cmd_kit({"search", "basic", "--root", root.string(), "--lane", "content", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(content.find(R"("lane":"content")") != std::string::npos);
    REQUIRE(content.find("dev.pulp.fixtures.basic-content-pack") != std::string::npos);
    REQUIRE(content.find(R"("lane":"kit")") == std::string::npos);
}

TEST_CASE("pulp kit search discovers verified local kit and content archives",
          "[cli][kit][archive]") {
    const auto root = repo_root() / "fixtures/packages";
    TempDir search_root;
    const auto kit_archive = search_root.path / "basic-ui-kit.pulpkit";
    const auto content_archive = search_root.path / "basic-content-pack.pulpcontent";

    REQUIRE(cmd_kit({"pack", (root / "basic-ui-kit").string(),
                     "--output", kit_archive.string(), "--json"}) == 0);
    REQUIRE(cmd_kit({"pack", (root / "basic-content-pack").string(),
                     "--output", content_archive.string(), "--json"}) == 0);

    int exit_code = 0;
    const auto kits = capture_stdout_for([&] {
        return cmd_kit({"search", "basic", "--root", search_root.path.string(),
                        "--lane", "kit", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(kits.find("dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    REQUIRE(kits.find(kit_archive.filename().string()) != std::string::npos);
    REQUIRE(kits.find("dev.pulp.fixtures.basic-content-pack") == std::string::npos);

    const auto content = capture_stdout_for([&] {
        return cmd_kit({"search", "basic", "--root", search_root.path.string(),
                        "--lane", "content", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(content.find("dev.pulp.fixtures.basic-content-pack") != std::string::npos);
    REQUIRE(content.find(content_archive.filename().string()) != std::string::npos);
    REQUIRE(content.find("dev.pulp.fixtures.basic-ui-kit") == std::string::npos);
}

TEST_CASE("pulp kit metadata can inspect content archives but cannot plan, apply, or publish them",
          "[cli][kit][content][trust]") {
    const auto root = repo_root() / "fixtures/packages";
    TempDir tmp;
    TempDir project;
    write_file(project.path / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(ContentWrongLane)\n");
    const auto fixture = root / "basic-content-pack";
    const auto archive = tmp.path / "basic-content-pack.pulpcontent";

    REQUIRE(cmd_kit({"pack", fixture.string(), "--output", archive.string(), "--json"}) == 0);

    int exit_code = -1;
    const auto inspect = capture_stdout_for([&] {
        return cmd_kit({"inspect", archive.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(inspect.find("dev.pulp.fixtures.basic-content-pack") != std::string::npos);
    REQUIRE(inspect.find(R"("kind":["content-pack"])") != std::string::npos);

    const auto plan = capture_stdout_for([&] {
        return cmd_kit({"plan", fixture.string(), "--project", project.path.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(plan.find(R"("code":"content-pack-wrong-lane")") != std::string::npos);
    REQUIRE(plan.find(R"("actions":[])") != std::string::npos);

    REQUIRE(cmd_kit({"apply", archive.string(), "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));

    const auto publish = capture_stdout_for([&] {
        return cmd_kit({"publish", archive.string(), "--dry-run", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(publish.find(R"("code":"content-pack-wrong-lane")") != std::string::npos);
    REQUIRE(publish.find(R"("publishing_enabled":false)") != std::string::npos);
}

TEST_CASE("pulp kit search filters local manifests by package kind",
          "[cli][kit]") {
    const auto root = repo_root() / "fixtures/packages";

    int exit_code = 0;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"search", "*", "--root", root.string(), "--kind", "content-pack", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(output.find("dev.pulp.fixtures.basic-content-pack") != std::string::npos);
    REQUIRE(output.find("dev.pulp.fixtures.basic-ui-kit") == std::string::npos);
}

TEST_CASE("pulp kit search rejects invalid lane filters before scanning",
          "[cli][kit]") {
    const auto root = repo_root() / "fixtures/packages";
    REQUIRE(cmd_kit({"search", "basic", "--root", root.string(), "--lane", "dependency", "--json"}) == 2);
}

TEST_CASE("pulp kit validation accepts template generated-project golden diffs",
          "[cli][kit]") {
    const auto root = repo_root();
    const auto fixture = root / "fixtures/packages/simple-plugin-template";

    auto result = validate_manifest_path(fixture);
    REQUIRE(result.ok());
    REQUIRE_FALSE(has_issue(result, "missing-path"));
    REQUIRE(fs::exists(fixture / "validation" / "generated-project.diff"));
    REQUIRE(read_file(fixture / "validation" / "generated-project.diff")
                .find("diff --git a/kit_gain.hpp b/kit_gain.hpp") != std::string::npos);
}

TEST_CASE("pulp kit validation rejects template kits without generated-project diffs",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "templates" / "basic-plugin" / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(MissingTemplateDiff)\n");
    write_file(kit.path / "validation" / "template-smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.template-without-diff",
  "name": "Template Without Diff",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT"
  },
  "kind": ["template"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS"]
  },
  "capabilities": ["template.plugin.basic"],
  "exports": {
    "templates": ["templates/basic-plugin"]
  },
  "dependencies": {
    "pulp": ["pulp::format"],
    "packages": []
  },
  "validation": {
    "profiles": ["validation/template-smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "missing-template-generated-project-diff"));
}

TEST_CASE("pulp package JSON Schema carries kind-specific review evidence rules",
          "[cli][kit][schema]") {
    const auto schema = read_file(repo_root() / "tools/kits/pulp-package.schema.json");

    REQUIRE(schema.find(R"("allOf")") != std::string::npos);
    REQUIRE(schema.find(R"("contains": { "const": "template" })") != std::string::npos);
    REQUIRE(schema.find(R"("required": ["generatedProjectDiffs"])") != std::string::npos);
    const auto generated_diffs = schema.find(R"("generatedProjectDiffs")");
    REQUIRE(generated_diffs != std::string::npos);
    REQUIRE(schema.find(R"("minItems": 1)", generated_diffs) != std::string::npos);
    REQUIRE(schema.find(R"("contains": { "const": "ui-kit" })") != std::string::npos);
    REQUIRE(schema.find(R"("anyOf")") != std::string::npos);
    REQUIRE(schema.find(R"("required": ["screenshots"])") != std::string::npos);
    REQUIRE(schema.find(R"("required": ["reports"])") != std::string::npos);
}

TEST_CASE("pulp kit validation accepts UI screenshot evidence paths",
          "[cli][kit]") {
    const auto root = repo_root();
    const auto fixture = root / "fixtures/packages/basic-ui-kit";

    auto result = validate_manifest_path(fixture);
    REQUIRE(result.ok());
    REQUIRE_FALSE(has_issue(result, "missing-path"));
    REQUIRE(fs::exists(fixture / "validation" / "screenshots" / "basic-ui-kit.json"));
    REQUIRE(fs::exists(fixture / "validation" / "reports" / "basic-ui-kit-screenshot.json"));

    const auto profile = read_file(fixture / "validation" / "screenshots" / "basic-ui-kit.json");
    const auto report = read_file(fixture / "validation" / "reports" / "basic-ui-kit-screenshot.json");
    REQUIRE(profile.find(R"("kind": "pulp-screenshot-profile")") != std::string::npos);
    REQUIRE(profile.find(R"("executeDuringInspect": false)") != std::string::npos);
    REQUIRE(report.find(R"("renderer": "pulp")") != std::string::npos);
}

TEST_CASE("pulp kit init scaffolds structured authoring provenance",
          "[cli][kit]") {
    TempDir kit;

    REQUIRE(cmd_kit({"init", "--kind", "source",
                     "--id", "dev.pulp.tests.initialized-kit",
                     "--dir", kit.path.string()}) == 0);

    const auto manifest = read_file(kit.path / "pulp.package.json");
    REQUIRE(manifest.find(R"("createdBy": {)") != std::string::npos);
    REQUIRE(manifest.find(R"("type": "human")") != std::string::npos);
    REQUIRE(manifest.find(R"("humanReview": {)") != std::string::npos);
    REQUIRE(manifest.find(R"("reviewed": true)") != std::string::npos);
    REQUIRE(fs::exists(kit.path / "AGENTS.md"));

    auto result = validate_manifest_path(kit.path, true);
    REQUIRE(result.ok());
}

TEST_CASE("pulp kit validation verifies hashed evidence objects",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "validation" / "reports" / "smoke.json",
               "{\"status\":\"pass\"}\n");
    const auto report_sha =
        "sha256-" + pulp::runtime::sha256_hex(
            read_file(kit.path / "validation" / "reports" / "smoke.json"));
    write_file(kit.path / "pulp.package.json", std::string(R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.hashed-evidence-kit",
  "name": "Hashed Evidence Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"],
    "reports": ["validation/reports/smoke.json"]
  },
  "evidence": {
    "validationReports": [
      {
        "id": "smoke",
        "path": "validation/reports/smoke.json",
        "tool": "pulp_validate",
        "status": "pass",
        "sha256": ")JSON") + report_sha + R"JSON("
      }
    ]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE(result.ok());
    REQUIRE_FALSE(has_issue(result, "evidence-digest-mismatch"));
}

TEST_CASE("pulp kit validation requires a per-asset license inventory",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.missing-license-inventory-kit",
  "name": "Missing License Inventory Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "missing-license-inventory"));
}

TEST_CASE("pulp kit validation rejects mismatched evidence digests",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "src" / "gain.cpp", "// gain\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "validation" / "reports" / "smoke.json",
               "{\"status\":\"pass\"}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-evidence-kit",
  "name": "Bad Evidence Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/gain.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"],
    "reports": ["validation/reports/smoke.json"]
  },
  "evidence": {
    "reports": [
      {
        "path": "validation/reports/smoke.json",
        "sha256": "sha256-0000000000000000000000000000000000000000000000000000000000000000"
      }
    ]
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "evidence-digest-mismatch"));
}

TEST_CASE("pulp kit validation rejects missing UI screenshot evidence paths",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "ui" / "index.js", "export const fixtureName = 'Missing Evidence';\n");
    write_file(kit.path / "ui" / "tokens.json", "{}\n");
    write_file(kit.path / "AGENTS.md", "# Missing evidence kit\n");
    write_file(kit.path / "validation" / "ui-smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.missing-ui-evidence",
  "name": "Missing UI Evidence",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "description": "UI evidence fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.basic"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"],
    "designTokens": ["ui/tokens.json"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/ui-smoke.json"],
    "reports": ["validation/reports/missing-screenshot.json"]
  },
  "evidence": {
    "screenshots": ["validation/screenshots/missing-screenshot.json"]
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "missing-path"));
}

TEST_CASE("pulp kit validation rejects UI kits with no screenshot evidence",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "ui" / "index.js", "export const fixtureName = 'No Evidence';\n");
    write_file(kit.path / "ui" / "tokens.json", "{}\n");
    write_file(kit.path / "AGENTS.md", "# No evidence kit\n");
    write_file(kit.path / "validation" / "ui-smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.no-ui-evidence",
  "name": "No UI Evidence",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "description": "UI evidence absence fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.basic"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"],
    "designTokens": ["ui/tokens.json"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/ui-smoke.json"]
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "missing-ui-evidence"));
}

TEST_CASE("pulp kit verify evaluates Pulp screenshot profile reports after plan review",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerify)\n");

    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", fixture.string(), "--project", project.path.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("kind":"ui-kit-cmake-include")") != std::string::npos);
    REQUIRE(output.find("\"path\":\"include(cmake/pulp-kits.cmake OPTIONAL)\"") != std::string::npos);
    REQUIRE(output.find(R"("kind":"ui-kit-cmake-target")") != std::string::npos);
    REQUIRE(output.find(R"("path":"pulp_kit_dev_pulp_fixtures_basic_ui_kit")") != std::string::npos);
    REQUIRE(output.find(R"("kind":"ui-kit-helper-call")") != std::string::npos);
    REQUIRE(output.find("pulp_use_kit_ui(<plugin-target> pulp_kit_dev_pulp_fixtures_basic_ui_kit SCRIPT pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/index.js TOKENS pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/tokens.json)") != std::string::npos);
    REQUIRE(output.find(R"("kind":"ui-kit-asset-root")") != std::string::npos);
    REQUIRE(output.find(R"("path":"pulp-kits/dev.pulp.fixtures.basic-ui-kit/assets/")") != std::string::npos);
}

TEST_CASE("pulp kit verify can explicitly execute screenshot profiles after review",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerifyRender)\n");
    write_fake_screenshot_tool(project.path, "fake png bytes");

    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", fixture.string(),
                        "--project", project.path.string(),
                        "--execute-screenshots",
                        "--screenshot-backend", "skia",
                        "--json"});
    }, exit_code);
    INFO(output);
    INFO("render-logs:\n" + collect_render_logs(project.path));
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("kind":"rendered-screenshot")") != std::string::npos);
    REQUIRE(output.find(R"("kind":"render-log")") != std::string::npos);
    REQUIRE(fs::exists(project.path / ".pulp" / "kit-validation" /
                       "validation-screenshots-basic-ui-kit-json" /
                       "basic-ui-kit-default.png"));
}

TEST_CASE("pulp kit verify writes visual diff reports for explicit screenshot baselines",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerifyVisualDiff)\n");
    write_fake_screenshot_tool(project.path, "fake png bytes");

    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Visual diff kit\n");
    write_file(kit.path / "ui" / "index.js", "createLabel('status', 'visual', '');\n");
    write_file(kit.path / "validation" / "screenshots" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-profile",
  "id": "visual",
  "entrypoint": "ui/index.js",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  },
  "expectedReport": "validation/reports/visual.json",
  "expectedImage": "validation/goldens/visual.png",
  "policy": {
    "renderer": "pulp",
    "executeDuringInspect": false
  }
})JSON");
    write_file(kit.path / "validation" / "reports" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-report",
  "profile": "validation/screenshots/visual.json",
  "renderer": "pulp",
  "status": "recorded",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  }
})JSON");
    write_file(kit.path / "validation" / "goldens" / "visual.png",
               fake_screenshot_bytes("fake png bytes"));
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.visual-diff-kit",
  "name": "Visual Diff Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT",
    "assets": "CC0-1.0"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.visual-diff"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  },
  "evidence": {
    "screenshots": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  }
})JSON");

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", kit.path.string(),
                        "--project", project.path.string(),
                        "--execute-screenshots",
                        "--json"});
    }, exit_code);
    INFO(output);
    INFO("render-logs:\n" + collect_render_logs(project.path));
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("kind":"visual-diff-report")") != std::string::npos);
    const auto report = project.path / ".pulp" / "kit-validation" /
                        "validation-screenshots-visual-json" /
                        "visual.visual-diff.json";
    REQUIRE(fs::exists(report));
    REQUIRE(read_file(report).find(R"("status": "pass")") != std::string::npos);
}

TEST_CASE("pulp kit verify fails explicit screenshot baselines on visual mismatch",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerifyVisualMismatch)\n");
    write_fake_screenshot_tool(project.path, "actual bytes");

    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Visual mismatch kit\n");
    write_file(kit.path / "ui" / "index.js", "createLabel('status', 'visual', '');\n");
    write_file(kit.path / "validation" / "screenshots" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-profile",
  "id": "visual",
  "entrypoint": "ui/index.js",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  },
  "expectedReport": "validation/reports/visual.json",
  "expectedImage": "validation/goldens/visual.png",
  "policy": {
    "renderer": "pulp",
    "executeDuringInspect": false
  }
})JSON");
    write_file(kit.path / "validation" / "reports" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-report",
  "profile": "validation/screenshots/visual.json",
  "renderer": "pulp",
  "status": "recorded",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  }
})JSON");
    write_file(kit.path / "validation" / "goldens" / "visual.png", "expected bytes");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.visual-mismatch-kit",
  "name": "Visual Mismatch Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT",
    "assets": "CC0-1.0"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.visual-diff"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  },
  "evidence": {
    "screenshots": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  }
})JSON");

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", kit.path.string(),
                        "--project", project.path.string(),
                        "--execute-screenshots",
                        "--json"});
    }, exit_code);
    INFO(output);
    INFO("render-logs:\n" + collect_render_logs(project.path));
    REQUIRE(exit_code == 1);
    REQUIRE(output.find("screenshot-visual-diff-mismatch") != std::string::npos);
    const auto report = project.path / ".pulp" / "kit-validation" /
                        "validation-screenshots-visual-json" /
                        "visual.visual-diff.json";
    REQUIRE(fs::exists(report));
    REQUIRE(read_file(report).find(R"("status": "fail")") != std::string::npos);
}

TEST_CASE("pulp kit verify allows visual diffs within declared byte tolerance",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerifyVisualTolerance)\n");
    write_fake_screenshot_tool(project.path, "actual bytes");

    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Visual tolerance kit\n");
    write_file(kit.path / "ui" / "index.js", "createLabel('status', 'visual', '');\n");
    write_file(kit.path / "validation" / "screenshots" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-profile",
  "id": "visual",
  "entrypoint": "ui/index.js",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  },
  "expectedReport": "validation/reports/visual.json",
  "expectedImage": "validation/goldens/visual.png",
  "visualToleranceBytes": 16,
  "policy": {
    "renderer": "pulp",
    "executeDuringInspect": false
  }
})JSON");
    write_file(kit.path / "validation" / "reports" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-report",
  "profile": "validation/screenshots/visual.json",
  "renderer": "pulp",
  "status": "recorded",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  }
})JSON");
    write_file(kit.path / "validation" / "goldens" / "visual.png", "expected bytes");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.visual-tolerance-kit",
  "name": "Visual Tolerance Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT",
    "assets": "CC0-1.0"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.visual-diff"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  },
  "evidence": {
    "screenshots": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  }
})JSON");

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", kit.path.string(),
                        "--project", project.path.string(),
                        "--execute-screenshots",
                        "--json"});
    }, exit_code);
    INFO(output);
    INFO("render-logs:\n" + collect_render_logs(project.path));
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("kind":"visual-diff-report")") != std::string::npos);
    const auto report = project.path / ".pulp" / "kit-validation" /
                        "validation-screenshots-visual-json" /
                        "visual.visual-diff.json";
    REQUIRE(fs::exists(report));
    const auto report_text = read_file(report);
    REQUIRE(report_text.find(R"("status": "pass")") != std::string::npos);
    REQUIRE(report_text.find(R"("mode": "byte-tolerance")") != std::string::npos);
    REQUIRE(report_text.find(R"("tolerance_bytes": 16)") != std::string::npos);
}

TEST_CASE("pulp kit verify rejects negative visual diff tolerance without screenshot execution",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerifyBadVisualTolerance)\n");

    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Bad visual tolerance kit\n");
    write_file(kit.path / "ui" / "index.js", "createLabel('status', 'visual', '');\n");
    write_file(kit.path / "validation" / "screenshots" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-profile",
  "id": "visual",
  "entrypoint": "ui/index.js",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  },
  "expectedReport": "validation/reports/visual.json",
  "expectedImage": "validation/goldens/visual.png",
  "visualToleranceBytes": -1,
  "policy": {
    "renderer": "pulp",
    "executeDuringInspect": false
  }
})JSON");
    write_file(kit.path / "validation" / "reports" / "visual.json", R"JSON({
  "kind": "pulp-screenshot-report",
  "profile": "validation/screenshots/visual.json",
  "renderer": "pulp",
  "status": "recorded",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  }
})JSON");
    write_file(kit.path / "validation" / "goldens" / "visual.png", "expected bytes");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-visual-tolerance-kit",
  "name": "Bad Visual Tolerance Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "validation": "MIT",
    "assets": "CC0-1.0"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.visual-diff"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  },
  "evidence": {
    "screenshots": ["validation/screenshots/visual.json"],
    "reports": ["validation/reports/visual.json"]
  }
})JSON");

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", kit.path.string(),
                        "--project", project.path.string(),
                        "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find("invalid-screenshot-visual-tolerance") != std::string::npos);
}

TEST_CASE("pulp kit verify rejects mismatched screenshot profile reports",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitVerifyBad)\n");

    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Bad screenshot report kit\n");
    write_file(kit.path / "ui" / "index.js", "export const fixtureName = 'Bad Screenshot';\n");
    write_file(kit.path / "validation" / "screenshots" / "bad.json", R"JSON({
  "kind": "pulp-screenshot-profile",
  "id": "bad",
  "entrypoint": "ui/index.js",
  "dimensions": {
    "width": 320,
    "height": 160,
    "scale": 1
  },
  "expectedReport": "validation/reports/bad.json",
  "policy": {
    "renderer": "pulp",
    "executeDuringInspect": false
  }
})JSON");
    write_file(kit.path / "validation" / "reports" / "bad.json", R"JSON({
  "kind": "pulp-screenshot-report",
  "profile": "validation/screenshots/bad.json",
  "renderer": "pulp",
  "status": "recorded",
  "dimensions": {
    "width": 999,
    "height": 160,
    "scale": 1
  },
  "artifacts": []
})JSON");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-screenshot-report",
  "name": "Bad Screenshot Report",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["ui-kit"],
  "audience": ["developer", "agent"],
  "description": "Screenshot report mismatch fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["ui.controls.bad-screenshot"],
  "exports": {
    "pulpUiScripts": ["ui/index.js"]
  },
  "dependencies": {
    "pulp": ["pulp::view"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/screenshots/bad.json"],
    "reports": ["validation/reports/bad.json"]
  },
  "evidence": {
    "screenshots": ["validation/screenshots/bad.json"],
    "reports": ["validation/reports/bad.json"]
  }
})JSON");

    REQUIRE(cmd_kit({"verify", kit.path.string(), "--project", project.path.string(), "--json"}) == 1);
}

TEST_CASE("pulp kit validates graph, node-pack, and native-component fixtures",
          "[cli][kit]") {
    const auto root = repo_root();
    for (const auto& rel : {
             "fixtures/packages/level-graph-node-kit",
             "fixtures/packages/signed-node-pack-kit",
             "fixtures/packages/native-component-kit",
         }) {
        INFO(rel);
        auto result = validate_manifest_path(root / rel);
        REQUIRE(result.ok());
        REQUIRE_FALSE(has_issue(result, "missing-path"));
        REQUIRE(result.summary.schema == "pulp-package-v1");
        REQUIRE_FALSE(result.summary.id.empty());
        REQUIRE_FALSE(result.summary.capabilities.empty());
    }
}

TEST_CASE("pulp kit verify checks graph, node-pack, and native profiles",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitPhase4Verify)\n");
    const auto root = repo_root();

    REQUIRE(cmd_kit({"verify",
                     (root / "fixtures/packages/level-graph-node-kit").string(),
                     "--project", project.path.string(), "--json"}) == 0);
    REQUIRE(cmd_kit({"verify",
                     (root / "fixtures/packages/signed-node-pack-kit").string(),
                     "--project", project.path.string(), "--json"}) == 0);
    REQUIRE(cmd_kit({"verify",
                     (root / "fixtures/packages/native-component-kit").string(),
                     "--project", project.path.string(), "--json"}) == 0);
}

TEST_CASE("pulp kit verify rejects executable node-pack inspect profiles",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitBadNodeVerify)\n");

    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Bad node pack\n");
    write_file(kit.path / "node-pack" / "manifest.json", R"JSON({
  "pack_id": "dev.pulp.tests.bad-node-pack",
  "abi_major": 1,
  "binary": "bad.dylib",
  "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "signer_public_key": "0000000000000000000000000000000000000000000000000000000000000000",
  "signature": "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
  "nodes": [
    {"type_id": "dev.pulp.tests.bad-node-pack.node", "capabilities": 1}
  ]
})JSON");
    write_file(kit.path / "validation" / "node-pack-smoke.json", R"JSON({
  "kind": "node-pack-validation-profile",
  "checks": [
    "manifest-shape",
    "signature-before-load",
    "hash-before-load"
  ],
  "manifest": "node-pack/manifest.json",
  "executeDuringInspect": true
})JSON");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.bad-node-pack",
  "name": "Bad Node Pack",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT",
    "binary": "MIT"
  },
  "kind": ["node-pack"],
  "audience": ["developer", "agent"],
  "description": "Bad node-pack verify fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux", "Android"]
  },
  "capabilities": ["signal.graph.node.native"],
  "exports": {
    "nodePackManifests": ["node-pack/manifest.json"]
  },
  "dependencies": {
    "pulp": ["pulp::host"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/node-pack-smoke.json"]
  }
})JSON");

    int exit_code = 0;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"verify", kit.path.string(),
                        "--project", project.path.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find("node-pack-executes-during-inspect") != std::string::npos);
}

TEST_CASE("pulp kit validation rejects dynamic-native kits on iOS and AUv3",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Unsupported native kit\n");
    write_file(kit.path / "validation" / "node-pack-smoke.json", "{}\n");
    write_file(kit.path / "node-pack" / "manifest.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.unsupported-node-pack-kit",
  "name": "Unsupported Node Pack Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["node-pack"],
  "audience": ["developer", "agent"],
  "description": "Unsupported dynamic native platform fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "iOS", "AUv3"]
  },
  "capabilities": ["signal.graph.node.native"],
  "exports": {
    "nodePackManifests": ["node-pack/manifest.json"]
  },
  "dependencies": {
    "pulp": ["pulp::host"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/node-pack-smoke.json"]
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "dynamic-native-unsupported"));
}

TEST_CASE("pulp kit validation requires realtime contracts for graph and native kits",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Missing realtime contract\n");
    write_file(kit.path / "include" / "gain_core.h", "#pragma once\n");
    write_file(kit.path / "src" / "gain_core.cpp", "float gain(float x) { return x; }\n");
    write_file(kit.path / "validation" / "native-component-smoke.json", R"JSON({
  "kind": "native-component-validation-profile",
  "checks": [
    "public-native-core-abi",
    "source-built-only-when-selected",
    "process-no-alloc-no-lock"
  ]
})JSON");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.missing-native-rt-contract",
  "name": "Missing Native RT Contract",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["native-component"],
  "audience": ["developer", "agent"],
  "description": "Native component without realtime claims.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux", "Android"]
  },
  "capabilities": ["native.processor.core"],
  "exports": {
    "nativeComponentHeaders": ["include/gain_core.h"],
    "nativeComponentSources": ["src/gain_core.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::native-components"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/native-component-smoke.json"]
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "missing-rt-contract"));
}

TEST_CASE("pulp kit validation rejects incomplete realtime contracts for graph kits",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Incomplete graph realtime contract\n");
    write_file(kit.path / "src" / "node.cpp", "int graph_node() { return 0; }\n");
    write_file(kit.path / "fixtures" / "graph.json", "{}\n");
    write_file(kit.path / "fixtures" / "state.json", "{}\n");
    write_file(kit.path / "validation" / "graph-state.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.incomplete-graph-rt-contract",
  "name": "Incomplete Graph RT Contract",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "description": "Graph kit with incomplete realtime claims.",
  "requires": {
    "pulp": ">=0.395.0",
    "cpp": 20,
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["signal.graph.node.test"],
  "exports": {
    "sourceFiles": ["src/node.cpp"],
    "graphFixtures": ["fixtures/graph.json"],
    "stateFixtures": ["fixtures/state.json"]
  },
  "dependencies": {
    "pulp": ["pulp::host", "pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/graph-state.json"]
  }
})JSON");

    auto result = validate_manifest_path(kit.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-rt-contract"));
}

TEST_CASE("pulp kit validation reports actionable manifest errors",
          "[cli][kit]") {
    TempDir tmp;
    write_file(tmp.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "bad id with spaces",
  "name": "Bad Kit",
  "version": "x.y.z",
  "license": "GPL-3.0",
  "kind": ["node-pack"],
  "capabilities": [],
  "exports": {
    "pulpUiScripts": ["ui/missing.js"]
  },
  "dependencies": {
    "packages": []
  },
  "requires": {
    "platforms": ["iOS", "AUv3"]
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": true,
    "locksInProcess": true
  },
  "agent": {
    "autoApply": true
  },
  "validation": {
    "profiles": []
  }
})JSON");

    auto result = validate_manifest_path(tmp.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-id"));
    REQUIRE(has_issue(result, "invalid-version"));
    REQUIRE(has_issue(result, "license-rejected"));
    REQUIRE(has_issue(result, "dynamic-native-unsupported"));
    REQUIRE(has_issue(result, "rt-claim-conflict"));
    REQUIRE(has_issue(result, "missing-path"));
    REQUIRE(has_issue(result, "agent-auto-apply"));
}

TEST_CASE("pulp kit validation rejects ids unsafe as project path components",
          "[cli][kit]") {
    TempDir tmp;
    write_file(tmp.path / "ui" / "main.js", "export function setup() {}\n");
    write_file(tmp.path / "licenses" / "LICENSE.txt", "MIT\n");
    write_file(tmp.path / "validation" / "smoke.json", R"JSON({
  "schema": "pulp-validation-profile-v1",
  "kind": "ui-kit-smoke",
  "status": "pass",
  "issues": []
})JSON");
    write_file(tmp.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests:bad-path-id",
  "name": "Bad Path Id Kit",
  "version": "0.1.0",
  "license": "MIT",
  "kind": ["ui-kit"],
  "capabilities": ["ui.controls.test"],
  "exports": {
    "pulpUiScripts": ["ui/main.js"],
    "licenses": ["licenses/LICENSE.txt"]
  },
  "dependencies": {
    "pulp": [],
    "packages": []
  },
  "requires": {
    "pulp": ">=0.399.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  },
  "authoring": {
    "humanReviewed": true
  }
})JSON");

    auto result = validate_manifest_path(tmp.path);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_issue(result, "invalid-id"));
}

TEST_CASE("pulp kit inspect JSON summarizes without executing package files",
          "[cli][kit]") {
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    auto result = validate_manifest_path(fixture);
    auto json = validation_result_json(result);
    REQUIRE(result.ok());
    REQUIRE(json.find(R"("ok":true)") != std::string::npos);
    REQUIRE(json.find("dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    REQUIRE(json.find("ui.controls.basic") != std::string::npos);
}

TEST_CASE("pulp kit plan previews project mutations without writing files",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitPlan)\n");

    TempDir kit;
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.plan-kit",
  "name": "Plan Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "description": "Plan fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "cmakeTargets": ["plan_fixture::target"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  }
})JSON");
    write_file(kit.path / "AGENTS.md", "# Plan kit\n");

    const auto before_lock_exists = fs::exists(project.path / ".pulp" / "kits.lock.json");
    const auto before_cmake_exists = fs::exists(project.path / "cmake" / "pulp-kits.cmake");
    REQUIRE(cmd_kit({"plan", kit.path.string(), "--project", project.path.string(), "--json"}) == 0);
    REQUIRE(fs::exists(project.path / ".pulp" / "kits.lock.json") == before_lock_exists);
    REQUIRE(fs::exists(project.path / "cmake" / "pulp-kits.cmake") == before_cmake_exists);
}

TEST_CASE("pulp kit rejects preview alias to preserve plan/apply trust wording",
          "[cli][kit][trust]") {
    REQUIRE(cmd_kit({"preview", (repo_root() / "fixtures/packages/basic-ui-kit").string()}) == 2);
}

TEST_CASE("pulp kit plan resolves dependency packages only through curated registry",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitPlanDeps)\n");
    write_file(project.path / "tools" / "packages" / "registry.json", R"JSON({
  "registry_version": 1,
  "packages": {}
})JSON");

    TempDir kit;
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "AGENTS.md", "# Plan kit\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.plan-kit-deps",
  "name": "Plan Kit Deps",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "description": "Plan dependency fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "cmakeTargets": ["plan_fixture::target"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": ["missing-curated-package"]
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  }
})JSON");

    REQUIRE(cmd_kit({"plan", kit.path.string(), "--project", project.path.string(), "--json"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));
}

TEST_CASE("pulp kit rejects unsafe CMake target metadata before generated files",
          "[cli][kit][security]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitUnsafeTarget)\n");

    TempDir kit;
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "AGENTS.md", "# Unsafe target kit\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.unsafe-target-kit",
  "name": "Unsafe Target Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "description": "Unsafe target fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "cmakeTargets": ["safe::target\nmessage(FATAL_ERROR injected)"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "realtime": {
    "processSafe": true,
    "allocatesInProcess": false,
    "locksInProcess": false
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "human",
    "humanReviewed": true
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  }
})JSON");

    REQUIRE(cmd_kit({"plan", kit.path.string(), "--project", project.path.string(), "--json"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));
    REQUIRE(cmd_kit({"apply", kit.path.string(), "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));
}

TEST_CASE("pulp kit apply writes owned lock, CMake include, and declared UI files",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitApply)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string()}) == 2);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));
    REQUIRE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "ui" / "index.js"));
    REQUIRE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "ui" / "tokens.json"));
    REQUIRE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "assets" / ".gitkeep"));

    const auto lock = read_file(project.path / ".pulp" / "kits.lock.json");
    const auto manifest_sha =
        "sha256-" + pulp::runtime::sha256_hex(read_file(fixture / "pulp.package.json"));
    REQUIRE(lock.find("dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    REQUIRE(lock.find(R"("manifest_sha256": ")" + manifest_sha + R"(")") != std::string::npos);
    REQUIRE(lock.find("cmake/pulp-kits.cmake") != std::string::npos);
    REQUIRE(lock.find("pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/index.js") != std::string::npos);
    REQUIRE(lock.find(R"("ui_scripts": ["pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/index.js"])")
            != std::string::npos);
    REQUIRE(lock.find(R"("design_tokens": ["pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/tokens.json"])")
            != std::string::npos);
    REQUIRE(lock.find("pulp-kits/dev.pulp.fixtures.basic-ui-kit/assets") != std::string::npos);

    const auto cmake = read_file(project.path / "cmake" / "pulp-kits.cmake");
    REQUIRE(cmake.find("BEGIN_PULP_KIT dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    REQUIRE(cmake.find("add_library(pulp_kit_dev_pulp_fixtures_basic_ui_kit INTERFACE)") != std::string::npos);
    REQUIRE(cmake.find("PULP_UI_SCRIPTS \"pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/index.js\"")
            != std::string::npos);
    REQUIRE(cmake.find("PULP_DESIGN_TOKENS \"pulp-kits/dev.pulp.fixtures.basic-ui-kit/ui/tokens.json\"")
            != std::string::npos);
    REQUIRE(cmake.find("PULP_ASSETS \"pulp-kits/dev.pulp.fixtures.basic-ui-kit/assets")
            != std::string::npos);
    REQUIRE(read_file(project.path / "CMakeLists.txt")
                .find("include(cmake/pulp-kits.cmake OPTIONAL)") != std::string::npos);

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string(), "--yes"}) == 0);
    const auto cmakelists = read_file(project.path / "CMakeLists.txt");
    const auto include_pos = cmakelists.find("include(cmake/pulp-kits.cmake OPTIONAL)");
    REQUIRE(include_pos != std::string::npos);
    REQUIRE(cmakelists.find("include(cmake/pulp-kits.cmake OPTIONAL)", include_pos + 1)
            == std::string::npos);
}

TEST_CASE("pulp kit apply replaces same-id kit roots without leaving stale owned files",
          "[cli][kit]") {
    TempDir project;
    TempDir first_pack;
    TempDir second_pack;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitReplace)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    std::error_code ec;
    fs::copy(fixture, first_pack.path / "basic-ui-kit",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    REQUIRE_FALSE(ec);
    fs::copy(fixture, second_pack.path / "basic-ui-kit",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    REQUIRE_FALSE(ec);
    write_file(first_pack.path / "basic-ui-kit" / "legacy" / "old.txt", "old export\n");
    auto manifest = read_file(first_pack.path / "basic-ui-kit" / "pulp.package.json");
    const auto assets = std::string(R"("assets": ["assets/"])");
    const auto pos = manifest.find(assets);
    REQUIRE(pos != std::string::npos);
    manifest.replace(pos, assets.size(), R"("assets": ["assets/", "legacy/"])");
    write_file(first_pack.path / "basic-ui-kit" / "pulp.package.json", manifest);

    REQUIRE(cmd_kit({"apply", (first_pack.path / "basic-ui-kit").string(),
                     "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "legacy" / "old.txt"));

    REQUIRE(cmd_kit({"apply", (second_pack.path / "basic-ui-kit").string(),
                     "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "legacy" / "old.txt"));
    const auto lock = read_file(project.path / ".pulp" / "kits.lock.json");
    REQUIRE(lock.find("legacy/old.txt") == std::string::npos);
}

TEST_CASE("pulp kit apply rolls back copied files when ownership lock cannot be written",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitRollback)\n");
    write_file(project.path / ".pulp", "not a directory\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE(fs::exists(project.path / ".pulp"));
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "ui" / "index.js"));
    REQUIRE(read_file(project.path / "CMakeLists.txt")
                .find("include(cmake/pulp-kits.cmake OPTIONAL)") == std::string::npos);
}

TEST_CASE("pulp kit apply rejects symlinks inside exported directories before copying",
          "[cli][kit]") {
    TempDir project;
    TempDir kit_copy;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitSymlinkReject)\n");

    std::error_code ec;
    fs::copy(repo_root() / "fixtures/packages/basic-ui-kit",
             kit_copy.path / "basic-ui-kit",
             fs::copy_options::recursive,
             ec);
    REQUIRE_FALSE(ec);

    const auto outside = kit_copy.path / "outside.txt";
    write_file(outside, "outside secret\n");
    fs::create_symlink(outside, kit_copy.path / "basic-ui-kit" / "assets" / "outside-link.txt", ec);
    if (ec) {
        SUCCEED("symlink creation unavailable on this platform");
        return;
    }

    REQUIRE(cmd_kit({"apply", (kit_copy.path / "basic-ui-kit").string(),
                     "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "assets" / "outside-link.txt"));
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "ui" / "index.js"));
}

TEST_CASE("pulp kit remove deletes only lock-recorded owned files",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitRemove)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string(), "--yes"}) == 0);
    write_file(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "USER.txt",
               "not owned by lock\n");

    REQUIRE(cmd_kit({"remove", "dev.pulp.fixtures.basic-ui-kit",
                     "--project", project.path.string()}) == 2);
    REQUIRE(fs::exists(project.path / ".pulp" / "kits.lock.json"));

    int remove_rc = 0;
    const auto remove_out = capture_stdout_for([&] {
        return cmd_kit({"remove", "dev.pulp.fixtures.basic-ui-kit",
                        "--project", project.path.string(), "--yes"});
    }, remove_rc);
    REQUIRE(remove_rc == 0);
    // The OK line names what was removed (parity with `tool uninstall`).
    REQUIRE(remove_out.find("Removed kit dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    REQUIRE(remove_out.find("(removed ") != std::string::npos);
    REQUIRE(remove_out.find("file") != std::string::npos);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "cmake" / "pulp-kits.cmake"));
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "ui" / "index.js"));
    REQUIRE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "USER.txt"));
    REQUIRE(read_file(project.path / "CMakeLists.txt")
                .find("include(cmake/pulp-kits.cmake OPTIONAL)") == std::string::npos);
}

TEST_CASE("pulp kit apply builds, tests, and removes cleanly from a CMake project",
          "[cli][kit][acceptance]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\n"
               "project(KitApplyBuild LANGUAGES CXX)\n"
               "enable_testing()\n"
               "add_test(NAME kit-smoke COMMAND ${CMAKE_COMMAND} -E echo kit-smoke)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(run_success_command("cmake -S " + quote_for_shell(project.path)
                                + " -B " + quote_for_shell(project.path / "build")
                                + " -DCMAKE_BUILD_TYPE=Release") == 0);
    REQUIRE(run_success_command("cmake --build " + quote_for_shell(project.path / "build")
                                + " --config Release") == 0);
    REQUIRE(run_success_command("ctest --test-dir " + quote_for_shell(project.path / "build")
                                + " -C Release --output-on-failure") == 0);

    REQUIRE(cmd_kit({"remove", "dev.pulp.fixtures.basic-ui-kit",
                     "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(run_success_command("cmake -S " + quote_for_shell(project.path)
                                + " -B " + quote_for_shell(project.path / "build-after-remove")
                                + " -DCMAKE_BUILD_TYPE=Release") == 0);
    REQUIRE(run_success_command("cmake --build "
                                + quote_for_shell(project.path / "build-after-remove")
                                + " --config Release") == 0);
    REQUIRE(run_success_command("ctest --test-dir "
                                + quote_for_shell(project.path / "build-after-remove")
                                + " -C Release --output-on-failure") == 0);
}

TEST_CASE("pulp kit remove rejects tampered lock paths outside the kit-owned tree",
          "[cli][kit][security]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitRemoveTamper)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", project.path.string(), "--yes"}) == 0);
    const auto lock_path = project.path / ".pulp" / "kits.lock.json";
    auto lock = read_file(lock_path);
    const auto marker = R"("owned_paths": [)";
    const auto pos = lock.find(marker);
    REQUIRE(pos != std::string::npos);
    lock.insert(pos + std::string(marker).size(), R"("CMakeLists.txt", )");
    write_file(lock_path, lock);

    REQUIRE(cmd_kit({"remove", "dev.pulp.fixtures.basic-ui-kit",
                     "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE(fs::exists(project.path / "CMakeLists.txt"));
    REQUIRE(fs::exists(project.path / "pulp-kits" / "dev.pulp.fixtures.basic-ui-kit" / "ui" / "index.js"));
    REQUIRE(fs::exists(lock_path));
}

TEST_CASE("pulp kit apply copies graph, node-pack, and native exports",
          "[cli][kit]") {
    TempDir project;
    write_file(project.path / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\nproject(KitPhase4)\n");
    const auto root = repo_root();

    REQUIRE(cmd_kit({"apply",
                     (root / "fixtures/packages/level-graph-node-kit").string(),
                     "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(cmd_kit({"apply",
                     (root / "fixtures/packages/signed-node-pack-kit").string(),
                     "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(cmd_kit({"apply",
                     (root / "fixtures/packages/native-component-kit").string(),
                     "--project", project.path.string(), "--yes"}) == 0);

    const auto graph_root =
        project.path / "pulp-kits" / "dev.pulp.fixtures.level-graph-node-kit";
    REQUIRE(fs::exists(graph_root / "src" / "level_node.cpp"));
    REQUIRE(fs::exists(graph_root / "fixtures" / "level-graph.json"));
    REQUIRE(fs::exists(graph_root / "fixtures" / "level-state.json"));

    const auto node_root =
        project.path / "pulp-kits" / "dev.pulp.fixtures.signed-node-pack-kit";
    REQUIRE(fs::exists(node_root / "node-pack" / "manifest.json"));

    const auto native_root =
        project.path / "pulp-kits" / "dev.pulp.fixtures.native-component-kit";
    REQUIRE(fs::exists(native_root / "include" / "gain_core.h"));
    REQUIRE(fs::exists(native_root / "src" / "gain_core.cpp"));

    const auto lock = read_file(project.path / ".pulp" / "kits.lock.json");
    REQUIRE(lock.find(R"("source_files": ["pulp-kits/dev.pulp.fixtures.level-graph-node-kit/src/level_node.cpp"])")
            != std::string::npos);
    REQUIRE(lock.find(R"("node_pack_manifests": ["pulp-kits/dev.pulp.fixtures.signed-node-pack-kit/node-pack/manifest.json"])")
            != std::string::npos);
    REQUIRE(lock.find(R"("native_component_headers": ["pulp-kits/dev.pulp.fixtures.native-component-kit/include/gain_core.h"])")
            != std::string::npos);
    REQUIRE(lock.find(R"("native_component_sources": ["pulp-kits/dev.pulp.fixtures.native-component-kit/src/gain_core.cpp"])")
            != std::string::npos);
    REQUIRE(lock.find(R"("graph_fixtures": ["pulp-kits/dev.pulp.fixtures.level-graph-node-kit/fixtures/level-graph.json"])")
            != std::string::npos);
    REQUIRE(lock.find(R"("state_fixtures": ["pulp-kits/dev.pulp.fixtures.level-graph-node-kit/fixtures/level-state.json"])")
            != std::string::npos);

    const auto cmake = read_file(project.path / "cmake" / "pulp-kits.cmake");
    REQUIRE(cmake.find("PROPERTY PULP_SOURCE_FILES "
                       "\"pulp-kits/dev.pulp.fixtures.level-graph-node-kit/src/level_node.cpp\"")
            != std::string::npos);
    REQUIRE(cmake.find("PROPERTY PULP_GRAPH_FIXTURES "
                       "\"pulp-kits/dev.pulp.fixtures.level-graph-node-kit/fixtures/level-graph.json\"")
            != std::string::npos);
    REQUIRE(cmake.find("PROPERTY PULP_STATE_FIXTURES "
                       "\"pulp-kits/dev.pulp.fixtures.level-graph-node-kit/fixtures/level-state.json\"")
            != std::string::npos);
    REQUIRE(cmake.find("PROPERTY PULP_NODE_PACK_MANIFESTS "
                       "\"pulp-kits/dev.pulp.fixtures.signed-node-pack-kit/node-pack/manifest.json\"")
            != std::string::npos);
    REQUIRE(cmake.find("PROPERTY PULP_NATIVE_COMPONENT_HEADERS "
                       "\"pulp-kits/dev.pulp.fixtures.native-component-kit/include/gain_core.h\"")
            != std::string::npos);
    REQUIRE(cmake.find("PROPERTY PULP_NATIVE_COMPONENT_SOURCES "
                       "\"pulp-kits/dev.pulp.fixtures.native-component-kit/src/gain_core.cpp\"")
            != std::string::npos);
}

TEST_CASE("pulp kit pack writes archive with SHA-256 manifest",
          "[cli][kit]") {
    TempDir tmp;
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    const auto out = tmp.path / "basic-ui-kit.pulpkit";

    REQUIRE(cmd_kit({"pack", fixture.string(), "--output", out.string(), "--json"}) == 0);
    REQUIRE(fs::exists(out));

    const auto manifest = read_zip_entry(out, "pulp.package.json");
    REQUIRE(manifest.find("dev.pulp.fixtures.basic-ui-kit") != std::string::npos);
    const auto sha_manifest = read_zip_entry(out, "files.sha256.json");
    REQUIRE(sha_manifest.find("pulp.package.json") != std::string::npos);
    REQUIRE(sha_manifest.find("ui/index.js") != std::string::npos);
    REQUIRE(sha_manifest.find("sha256-") != std::string::npos);
}

TEST_CASE("pulp kit pack rejects symlinks before writing archive payloads",
          "[cli][kit][security]") {
    TempDir tmp;
    TempDir kit_copy;
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    const auto out = tmp.path / "symlinked-ui-kit.pulpkit";

    std::error_code ec;
    fs::copy(fixture, kit_copy.path / "basic-ui-kit",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    REQUIRE_FALSE(ec);
    const auto outside = kit_copy.path / "outside-secret.txt";
    write_file(outside, "outside secret\n");
    fs::create_symlink(outside, kit_copy.path / "basic-ui-kit" / "assets" / "outside-link.txt", ec);
    if (ec) {
        SUCCEED("symlink creation unavailable on this platform");
        return;
    }

    REQUIRE(cmd_kit({"pack", (kit_copy.path / "basic-ui-kit").string(),
                     "--output", out.string(), "--json"}) == 1);
    REQUIRE_FALSE(fs::exists(out));
}

TEST_CASE("pulp kit accepts packed pulpkit archives for validate, plan, and apply",
          "[cli][kit][archive]") {
    TempDir tmp;
    TempDir project;
    write_file(project.path / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(KitArchiveApply)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    const auto archive = tmp.path / "basic-ui-kit.pulpkit";

    REQUIRE(cmd_kit({"pack", fixture.string(), "--output", archive.string(), "--json"}) == 0);

    int exit_code = -1;
    auto output = capture_stdout_for([&] {
        return cmd_kit({"validate", archive.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("ok":true)") != std::string::npos);
    REQUIRE(output.find("dev.pulp.fixtures.basic-ui-kit") != std::string::npos);

    output = capture_stdout_for([&] {
        return cmd_kit({"plan", archive.string(), "--project", project.path.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("copy-ui-script")") != std::string::npos);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));

    output = capture_stdout_for([&] {
        return cmd_kit({"publish", archive.string(), "--dry-run", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("publishing_enabled":false)") != std::string::npos);

    REQUIRE(cmd_kit({"apply", archive.string(), "--project", project.path.string(), "--yes"}) == 0);
    REQUIRE(fs::exists(project.path / "pulp-kits" /
                       "dev.pulp.fixtures.basic-ui-kit" / "ui" / "index.js"));
    const auto lock = read_file(project.path / ".pulp" / "kits.lock.json");
    REQUIRE(lock.find(archive.generic_string()) != std::string::npos);
}

TEST_CASE("pulp kit rejects pulpkit archives without hash manifests",
          "[cli][kit][archive][security]") {
    TempDir tmp;
    TempDir project;
    write_file(project.path / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(KitArchiveReject)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    const auto archive = write_archive_with_optional_hash_manifest(
        tmp.path / "missing-hashes.pulpkit", fixture, false, false);

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"validate", archive.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find(R"("code":"sha256")") != std::string::npos);
    REQUIRE(output.find("missing files.sha256.json") != std::string::npos);

    REQUIRE(cmd_kit({"apply", archive.string(), "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits"));
}

TEST_CASE("pulp kit rejects pulpkit archives with unlisted payload files",
          "[cli][kit][archive][security]") {
    TempDir tmp;
    TempDir project;
    write_file(project.path / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(KitArchiveUnlisted)\n");
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";
    const auto archive = write_archive_with_optional_hash_manifest(
        tmp.path / "unlisted-payload.pulpkit", fixture, true, true);

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"validate", archive.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find(R"("code":"sha256")") != std::string::npos);
    REQUIRE(output.find("unlisted archived file `extras/unlisted.txt`") != std::string::npos);

    REQUIRE(cmd_kit({"apply", archive.string(), "--project", project.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(project.path / ".pulp" / "kits.lock.json"));
    REQUIRE_FALSE(fs::exists(project.path / "pulp-kits"));
}

TEST_CASE("pulp kit publish dry-run enforces publish policy without remote mutation",
          "[cli][kit]") {
    TempDir tmp;
    const auto fixture = tmp.path / "basic-ui-kit";
    fs::copy(repo_root() / "fixtures/packages/basic-ui-kit", fixture,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    const auto registry_manifest = write_registry_manifest(fixture);

    REQUIRE(cmd_kit({"publish", fixture.string()}) == 2);

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"publish", fixture.string(), "--dry-run",
                        "--registry-manifest", registry_manifest.string(), "--json"});
    }, exit_code);
    REQUIRE(exit_code == 0);
    REQUIRE(output.find(R"("notice-compatibility")") != std::string::npos);
    REQUIRE(output.find(R"("publish-ready")") != std::string::npos);
}

TEST_CASE("pulp kit publish dry-run rejects mismatched signed registry manifests",
          "[cli][kit]") {
    TempDir tmp;
    const auto fixture = tmp.path / "basic-ui-kit";
    fs::copy(repo_root() / "fixtures/packages/basic-ui-kit", fixture,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    const auto registry_manifest = write_registry_manifest(fixture);
    write_file(fixture / "pulp.package.json", read_file(fixture / "pulp.package.json") + "\n");

    REQUIRE(cmd_kit({"publish", fixture.string(), "--dry-run",
                     "--registry-manifest", registry_manifest.string(), "--json"}) == 1);
}

TEST_CASE("pulp kit publish dry-run requires NOTICE-compatible license files",
          "[cli][kit]") {
    TempDir tmp;
    const auto fixture = tmp.path / "basic-ui-kit";
    fs::copy(repo_root() / "fixtures/packages/basic-ui-kit", fixture,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    auto manifest = read_file(fixture / "pulp.package.json");
    manifest = replace_all(manifest, "\r\n", "\n");
    const std::string licenses_line = "    \"licenses\": [\"licenses/LICENSE.txt\"]\n";
    const auto pos = manifest.find(licenses_line);
    REQUIRE(pos != std::string::npos);
    manifest.erase(pos, licenses_line.size());
    manifest = replace_all(manifest, "    \"assets\": [\"assets/\"],\n  }",
                           "    \"assets\": [\"assets/\"]\n  }");
    write_file(fixture / "pulp.package.json", manifest);

    int exit_code = -1;
    const auto output = capture_stdout_for([&] {
        return cmd_kit({"publish", fixture.string(), "--dry-run", "--json"});
    }, exit_code);
    REQUIRE(exit_code == 1);
    REQUIRE(output.find(R"("code":"missing-notice-compatibility")") != std::string::npos);
    REQUIRE(output.find(R"("publish-ready")") == std::string::npos);
}

TEST_CASE("pulp kit publish dry-run rejects agent-authored packages without human review",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Agent kit\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "src" / "agent.cpp", "int agent_fixture() { return 1; }\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.agent-publish-kit",
  "name": "Agent Publish Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "description": "Agent publish policy fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/agent.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": "agent",
    "humanReviewed": false
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  }
})JSON");

    REQUIRE(cmd_kit({"publish", kit.path.string(), "--dry-run", "--json"}) == 1);
}

TEST_CASE("pulp kit publish dry-run rejects structured agent provenance without human review",
          "[cli][kit]") {
    TempDir kit;
    write_file(kit.path / "AGENTS.md", "# Structured agent kit\n");
    write_file(kit.path / "validation" / "smoke.json", "{}\n");
    write_file(kit.path / "src" / "agent.cpp", "int structured_agent_fixture() { return 1; }\n");
    write_file(kit.path / "pulp.package.json", R"JSON({
  "schema": "pulp-package-v1",
  "id": "dev.pulp.tests.structured-agent-publish-kit",
  "name": "Structured Agent Publish Kit",
  "version": "0.1.0",
  "license": "MIT",
  "licenses": {
    "code": "MIT"
  },
  "kind": ["source"],
  "audience": ["developer", "agent"],
  "description": "Structured agent publish policy fixture.",
  "requires": {
    "pulp": ">=0.395.0",
    "platforms": ["macOS", "Windows", "Linux"]
  },
  "capabilities": ["audio.effect.test"],
  "exports": {
    "sourceFiles": ["src/agent.cpp"]
  },
  "dependencies": {
    "pulp": ["pulp::signal"],
    "packages": []
  },
  "agent": {
    "guidance": "AGENTS.md"
  },
  "authoring": {
    "createdBy": {
      "type": "agent",
      "tool": "pulp_mcp",
      "toolVersion": "0.395.0"
    },
    "humanReview": {
      "requiredBeforePublish": true,
      "reviewed": false
    }
  },
  "validation": {
    "profiles": ["validation/smoke.json"]
  }
})JSON");

    REQUIRE(cmd_kit({"publish", kit.path.string(), "--dry-run", "--json"}) == 1);
}

TEST_CASE("pulp kit apply rejects non-project roots before writing files",
          "[cli][kit]") {
    TempDir not_project;
    const auto fixture = repo_root() / "fixtures/packages/basic-ui-kit";

    REQUIRE(cmd_kit({"apply", fixture.string(), "--project", not_project.path.string(), "--yes"}) == 1);
    REQUIRE_FALSE(fs::exists(not_project.path / ".pulp"));
    REQUIRE_FALSE(fs::exists(not_project.path / "cmake"));
    REQUIRE_FALSE(fs::exists(not_project.path / "pulp-kits"));
}

// ── cli_fs_util contract ────────────────────────────────────────────────────
//
// These pin the archive-safety helpers the kit commands guard extraction with.
// The containment check is lexical and frame-of-reference sensitive, which is
// exactly the part a caller can get wrong silently, so it is pinned here
// rather than left implied by the call sites.

TEST_CASE("path_is_within accepts paths at or below the root", "[cli][kit][fs-safety]") {
    using pulp::cli::fsutil::path_is_within;

    REQUIRE(path_is_within("/tmp/root/a/b.txt", "/tmp/root"));
    REQUIRE(path_is_within("/tmp/root", "/tmp/root"));
    // Interior traversal that stays inside resolves to a contained path.
    REQUIRE(path_is_within("/tmp/root/a/../b.txt", "/tmp/root"));
    // An empty root is a prefix of everything.
    REQUIRE(path_is_within("/tmp/root/a", ""));
}

TEST_CASE("path_is_within rejects escapes and sibling prefixes", "[cli][kit][fs-safety]") {
    using pulp::cli::fsutil::path_is_within;

    REQUIRE_FALSE(path_is_within("/tmp/root/../escaped.txt", "/tmp/root"));
    REQUIRE_FALSE(path_is_within("/tmp/other/a.txt", "/tmp/root"));
    REQUIRE_FALSE(path_is_within("/tmp", "/tmp/root"));
    // A component-wise compare, not a string-prefix compare: `/tmp/rooted` must
    // not read as inside `/tmp/root`.
    REQUIRE_FALSE(path_is_within("/tmp/rooted/a.txt", "/tmp/root"));
}

TEST_CASE("path_is_within compares lexically within one frame of reference",
          "[cli][kit][fs-safety]") {
    using pulp::cli::fsutil::path_is_within;

    // Neither argument is made absolute, so a relative/absolute pair never
    // shares a prefix regardless of the process CWD. Callers are responsible
    // for normalizing both sides into the same frame before calling.
    REQUIRE_FALSE(path_is_within("root/a.txt", "/tmp/root"));
    REQUIRE_FALSE(path_is_within("/tmp/root/a.txt", "root"));
    // Both relative against the same base compares as expected.
    REQUIRE(path_is_within("root/a.txt", "root"));
    REQUIRE_FALSE(path_is_within("root/../a.txt", "root"));
}

TEST_CASE("safe_archive_rel rejects unsafe archive entry names", "[cli][kit][fs-safety]") {
    using pulp::cli::fsutil::safe_archive_rel;

    REQUIRE(safe_archive_rel(fs::path("a/b/c.txt")));
    REQUIRE(safe_archive_rel(fs::path("file.txt")));

    REQUIRE_FALSE(safe_archive_rel(fs::path("")));
    REQUIRE_FALSE(safe_archive_rel(fs::path("/abs/file.txt")));
    REQUIRE_FALSE(safe_archive_rel(fs::path("../escape.txt")));
    REQUIRE_FALSE(safe_archive_rel(fs::path("a/../../escape.txt")));
    REQUIRE_FALSE(safe_archive_rel(fs::path("./a.txt")));
    REQUIRE_FALSE(safe_archive_rel(fs::path("a/./b.txt")));
}

TEST_CASE("is_package_archive_path recognizes package containers", "[cli][kit][fs-safety]") {
    using pulp::cli::fsutil::is_package_archive_path;

    REQUIRE(is_package_archive_path(fs::path("bundle.pulpkit")));
    REQUIRE(is_package_archive_path(fs::path("/tmp/pack.pulpcontent")));
    REQUIRE_FALSE(is_package_archive_path(fs::path("bundle.zip")));
    REQUIRE_FALSE(is_package_archive_path(fs::path("pulp.package.json")));
    REQUIRE_FALSE(is_package_archive_path(fs::path("bundle")));
}

TEST_CASE("temporary_archive_root yields a fresh path per call", "[cli][kit][fs-safety]") {
    using pulp::cli::fsutil::temporary_archive_root;

    const auto a = temporary_archive_root();
    const auto b = temporary_archive_root();

    REQUIRE(a != b);
    REQUIRE(a.is_absolute());
    REQUIRE_FALSE(fs::exists(a));
}
