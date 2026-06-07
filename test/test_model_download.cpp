// Streaming model downloader (MM-PR2) — exercised against a local HTTP server (no
// TLS, so no network + no flakiness): full download + sha256 + atomic rename, Range
// resume, sha256-mismatch rejection, and cancel-keeps-partial. The HTTPS/mbedTLS path
// is the same code; only the transport differs.
#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/model_download.hpp>
#include <pulp/runtime/model_registry.hpp>
#include <pulp/runtime/model_store.hpp>

#include <httplib.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace pulp::runtime;
namespace fs = std::filesystem;

namespace {

struct LocalServer {
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    explicit LocalServer(const fs::path& serve_dir) {
        svr.set_mount_point("/", serve_dir.string());  // static files w/ Range support
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~LocalServer() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

std::string make_fixture(const fs::path& path, size_t n) {
    std::string body;
    body.reserve(n);
    for (size_t i = 0; i < n; ++i) body += static_cast<char>('A' + (i * 7 + 3) % 26);
    std::ofstream(path, std::ios::binary).write(body.data(), static_cast<std::streamsize>(body.size()));
    return body;
}

}  // namespace

TEST_CASE("model downloader: full download + sha256 + atomic rename", "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    const std::string body = make_fixture(root / "serve" / "fixture.bin", 250'000);
    const std::string sha = sha256_hex(body);

    LocalServer server(root / "serve");

    const fs::path dest = root / "out" / "fixture.bin";
    DownloadRequest req{.url = server.url("/fixture.bin"), .dest = dest, .expected_sha256 = sha};

    auto res = download_file(req);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    REQUIRE(res.sha256 == sha);
    REQUIRE(res.bytes == body.size());
    REQUIRE(fs::exists(dest));
    REQUIRE_FALSE(fs::exists(fs::path(dest) += ".part"));  // .part consumed by atomic rename

    fs::remove_all(root);
}

TEST_CASE("model downloader: Range resume continues a partial", "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-resume";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    fs::create_directories(root / "out");
    const std::string body = make_fixture(root / "serve" / "fixture.bin", 250'000);
    const std::string sha = sha256_hex(body);

    LocalServer server(root / "serve");

    // Seed a partial .part with the first 100k bytes (as a prior interrupted download would).
    const fs::path dest = root / "out" / "fixture.bin";
    fs::path part = dest;
    part += ".part";
    std::ofstream(part, std::ios::binary).write(body.data(), 100'000);

    DownloadRequest req{.url = server.url("/fixture.bin"), .dest = dest, .expected_sha256 = sha,
                        .resume = true};
    auto res = download_file(req);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    REQUIRE(res.resumed);
    REQUIRE(res.sha256 == sha);  // re-hashed the partial + streamed the rest correctly
    REQUIRE(fs::exists(dest));

    fs::remove_all(root);
}

TEST_CASE("model downloader: sha256 mismatch is rejected", "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-bad";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    make_fixture(root / "serve" / "fixture.bin", 50'000);

    LocalServer server(root / "serve");

    const fs::path dest = root / "out" / "fixture.bin";
    DownloadRequest req{.url = server.url("/fixture.bin"), .dest = dest,
                        .expected_sha256 = std::string(64, 'a')};  // wrong
    auto res = download_file(req);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("mismatch") != std::string::npos);
    REQUIRE_FALSE(fs::exists(dest));                    // not published
    REQUIRE_FALSE(fs::exists(fs::path(dest) += ".part"));  // corrupt partial removed

    fs::remove_all(root);
}

TEST_CASE("model downloader: cancel keeps the partial for resume", "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-cancel";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    make_fixture(root / "serve" / "fixture.bin", 500'000);

    LocalServer server(root / "serve");

    const fs::path dest = root / "out" / "fixture.bin";
    DownloadRequest req{.url = server.url("/fixture.bin"), .dest = dest};

    std::atomic<int> chunks{0};
    auto res = download_file(req, [&](const DownloadProgress&) {
        return ++chunks < 1;  // cancel immediately after the first chunk
    });
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.cancelled);
    REQUIRE_FALSE(fs::exists(dest));                 // not published
    REQUIRE(fs::exists(fs::path(dest) += ".part"));  // partial kept for resume

    fs::remove_all(root);
}

TEST_CASE("model store: install_model downloads + records, then remove_model deletes",
          "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-install";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    const std::string body = make_fixture(root / "serve" / "weights.bin", 120'000);
    const std::string sha = sha256_hex(body);

    LocalServer server(root / "serve");
    const fs::path home = root / "home";  // acts as PULP_HOME

    ModelEntry model{.model_id = "m1", .display_name = "Model One", .backend = "mlx"};
    model.download_url = server.url("/weights.bin");
    model.sha256 = sha;

    auto inst = install_model(model, "magenta", /*on_progress=*/{}, /*cancel=*/nullptr,
                              /*headers=*/{}, home);
    INFO("install error: " << inst.error);
    REQUIRE(inst.ok);
    REQUIRE(inst.sha256 == sha);
    REQUIRE(fs::exists(inst.checkpoint_path));
    REQUIRE(fs::exists(inst.metadata_path));

    // The store now sees it installed + activatable.
    auto rec = read_installed_model("magenta", "m1", home);
    REQUIRE(rec.metadata_found);
    REQUIRE(rec.checkpoint_exists);
    std::vector<ModelEntry> registry = {model};
    REQUIRE(activate_model(registry, "magenta", "m1", home).ok);
    REQUIRE(read_active_model_id("magenta", home) == "m1");

    // Remove deletes files + metadata and clears the active selection.
    std::string err;
    REQUIRE(remove_model("magenta", "m1", err, home));
    REQUIRE(err.empty());
    REQUIRE_FALSE(fs::exists(inst.checkpoint_path));
    REQUIRE_FALSE(fs::exists(inst.metadata_path));
    REQUIRE(read_active_model_id("magenta", home).empty());

    fs::remove_all(root);
}

TEST_CASE("model store: install_model fetches every asset of a multi-asset bundle",
          "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-multiasset";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    const std::string weights = make_fixture(root / "serve" / "mrt2.mlxfn", 80'000);
    const std::string state = make_fixture(root / "serve" / "mrt2_state.safetensors", 50'000);

    LocalServer server(root / "serve");
    const fs::path home = root / "home";

    ModelEntry model{.model_id = "mrt2", .display_name = "MRT2", .backend = "mlx"};
    model.assets = {
        ModelAsset{.role = "weights", .checkpoint_ref = server.url("/mrt2.mlxfn")},
        ModelAsset{.role = "state", .checkpoint_ref = server.url("/mrt2_state.safetensors")},
    };

    auto inst = install_model(model, "magenta", /*on_progress=*/{}, /*cancel=*/nullptr,
                              /*headers=*/{}, home);
    INFO("install error: " << inst.error);
    REQUIRE(inst.ok);

    // BOTH assets land on disk in the model directory — not just the primary.
    const fs::path dir = home / "magenta" / "models" / "mrt2";
    REQUIRE(fs::exists(dir / "mrt2.mlxfn"));
    REQUIRE(fs::exists(dir / "mrt2_state.safetensors"));
    REQUIRE(fs::file_size(dir / "mrt2.mlxfn") == weights.size());
    REQUIRE(fs::file_size(dir / "mrt2_state.safetensors") == state.size());

    // The engine-facing checkpoint path is the first (weights) asset, and the metadata
    // records every asset by role.
    REQUIRE(inst.checkpoint_path == dir / "mrt2.mlxfn");
    std::ifstream meta_in(inst.metadata_path);
    const std::string meta((std::istreambuf_iterator<char>(meta_in)), std::istreambuf_iterator<char>());
    REQUIRE(meta.find("\"weights\"") != std::string::npos);
    REQUIRE(meta.find("\"state\"") != std::string::npos);

    fs::remove_all(root);
}
