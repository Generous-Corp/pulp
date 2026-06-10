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
#include <vector>

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

TEST_CASE("model downloader: http error status is reported and not written into .part",
          "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-404";
    fs::remove_all(root);
    fs::create_directories(root / "out");

    // A server that answers everything with a distinctive 404 error body.
    httplib::Server svr;
    const std::string error_body = "NOT-FOUND-ERROR-PAYLOAD";
    svr.Get(".*", [&](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(error_body, "text/plain");
    });
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread th([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    const fs::path dest = root / "out" / "fixture.bin";
    DownloadRequest req{.url = "http://127.0.0.1:" + std::to_string(port) + "/missing.bin",
                        .dest = dest};
    auto res = download_file(req);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("http status 404") != std::string::npos);
    REQUIRE_FALSE(fs::exists(dest));  // not published

    // The error body must never end up in the resumable .part.
    fs::path part = dest;
    part += ".part";
    if (fs::exists(part)) {
        std::ifstream in(part, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(in)), {});
        REQUIRE(contents.find(error_body) == std::string::npos);
    }

    svr.stop();
    if (th.joinable()) th.join();
    fs::remove_all(root);
}

TEST_CASE("model downloader: a stale oversized partial fails closed instead of resuming blindly",
          "[runtime][model][download]") {
    // A resumed download must verify the server is actually continuing from the
    // requested offset rather than trusting the 206. A conforming server (httplib)
    // can't be coerced into emitting a *lying* Content-Range — it returns the
    // correct range or a 416 when the requested offset is unsatisfiable. This test
    // exercises the latter, real, fail-closed path: a partial larger than the file
    // on the server must NOT be silently accepted as a finished download. (The
    // content-range-start check in download_file guards the complementary case of a
    // non-conforming server that returns 206 from the wrong offset.)
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-badrange";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    fs::create_directories(root / "out");
    make_fixture(root / "serve" / "fixture.bin", 10);  // tiny real file

    // Seed a 100k partial — much larger than the 10-byte file on the server.
    make_fixture(root / "out" / "fixture.bin.part", 100'000);

    LocalServer server(root / "serve");
    const fs::path dest = root / "out" / "fixture.bin";
    DownloadRequest req{.url = server.url("/fixture.bin"), .dest = dest, .resume = true};
    auto res = download_file(req);
    INFO("error: " << res.error);
    REQUIRE_FALSE(res.ok);
    REQUIRE_FALSE(fs::exists(dest));  // not published as a completed download

    fs::remove_all(root);
}

TEST_CASE("model downloader: cross-origin redirect drops the Authorization header",
          "[runtime][model][download]") {
    // HF gated downloads 302 from huggingface.co to a pre-signed S3 URL on another
    // host. The auth token must NOT be replayed to the redirect target. Two local
    // servers on different ports model the two origins; the second records whether
    // it saw an Authorization header.
    const fs::path root = fs::temp_directory_path() / "pulp-mm-pr2-redirect";
    fs::remove_all(root);
    fs::create_directories(root / "out");
    const std::string body = make_fixture(root / "out" / "payload-src.bin", 4096);
    const std::string sha = sha256_hex(body);

    // Origin B: serves the real bytes, records any Authorization header it received.
    std::atomic<bool> saw_auth_on_b{false};
    httplib::Server b;
    b.Get("/payload.bin", [&](const httplib::Request& reqB, httplib::Response& res) {
        if (reqB.has_header("Authorization")) saw_auth_on_b = true;
        res.set_content(body, "application/octet-stream");
    });
    int port_b = b.bind_to_any_port("127.0.0.1");
    std::thread tb([&] { b.listen_after_bind(); });
    b.wait_until_ready();

    // Origin A: 302-redirects to origin B (different port == different origin).
    httplib::Server a;
    a.Get("/gated.bin", [&](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", "http://127.0.0.1:" + std::to_string(port_b) + "/payload.bin");
    });
    int port_a = a.bind_to_any_port("127.0.0.1");
    std::thread ta([&] { a.listen_after_bind(); });
    a.wait_until_ready();

    const fs::path dest = root / "out" / "fixture.bin";
    DownloadRequest req{.url = "http://127.0.0.1:" + std::to_string(port_a) + "/gated.bin",
                        .dest = dest, .expected_sha256 = sha};
    req.headers.push_back(HttpHeader{"Authorization", "Bearer hf_secret_token"});
    auto res = download_file(req);
    INFO("error: " << res.error);
    REQUIRE(res.ok);              // redirect followed, bytes fetched
    REQUIRE(res.sha256 == sha);
    REQUIRE_FALSE(saw_auth_on_b); // token did NOT leak to the cross-origin target

    a.stop(); if (ta.joinable()) ta.join();
    b.stop(); if (tb.joinable()) tb.join();
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

TEST_CASE("model store: install_model reports no-length progress as indeterminate, then complete",
          "[runtime][model][download]") {
    const fs::path root = fs::temp_directory_path() / "pulp-mm-no-length-progress";
    fs::remove_all(root);
    fs::create_directories(root / "home");
    const std::string body(64'000, 'm');

    httplib::Server svr;
    svr.Get("/weights.bin", [body](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "application/octet-stream",
            [body](size_t offset, httplib::DataSink& sink) {
                if (offset >= body.size()) {
                    sink.done();
                    return true;
                }
                const size_t remaining = body.size() - offset;
                const size_t chunk = remaining < 4096 ? remaining : 4096;
                return sink.write(body.data() + offset, chunk);
            });
    });
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread th([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    ModelEntry model{.model_id = "m-no-length", .display_name = "No Length", .backend = "mlx"};
    model.download_url = "http://127.0.0.1:" + std::to_string(port) + "/weights.bin";

    std::vector<DownloadProgress> progress;
    auto inst = install_model(
        model, "magenta",
        [&](const DownloadProgress& p) {
            progress.push_back(p);
            return true;
        },
        /*cancel=*/nullptr, /*headers=*/{}, root / "home");

    INFO("install error: " << inst.error);
    REQUIRE(inst.ok);
    REQUIRE_FALSE(progress.empty());

    bool saw_indeterminate_bytes = false;
    for (const auto& p : progress) {
        if (p.total == 0 && p.downloaded > 0) saw_indeterminate_bytes = true;
    }
    REQUIRE(saw_indeterminate_bytes);
    REQUIRE(progress.back().total == 1'000'000);
    REQUIRE(progress.back().downloaded == 1'000'000);

    svr.stop();
    if (th.joinable()) th.join();
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
        ModelAsset{.role = "weights", .checkpoint_ref = server.url("/mrt2.mlxfn"),
                   .sha256 = sha256_hex(weights)},
        ModelAsset{.role = "state", .checkpoint_ref = server.url("/mrt2_state.safetensors"),
                   .sha256 = sha256_hex(state)},
    };

    // Correct per-asset hashes must pass verification (the install must honor them, not
    // silently skip — see the mismatch test below).
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

    meta_in.close();  // release the handle before remove_all (Windows can't unlink an open file)
    fs::remove_all(root);
}

TEST_CASE("model store: install_model verifies each bundle asset's sha256",
          "[runtime][model][download]") {
    // A pinned-but-wrong hash on a secondary asset must fail the install — the bundle
    // path must honor ModelAsset::sha256, not silently skip verification.
    const fs::path root = fs::temp_directory_path() / "pulp-mm-multiasset-badsha";
    fs::remove_all(root);
    fs::create_directories(root / "serve");
    const std::string weights = make_fixture(root / "serve" / "m.mlxfn", 40'000);
    make_fixture(root / "serve" / "m_state.safetensors", 20'000);

    LocalServer server(root / "serve");
    const fs::path home = root / "home";

    ModelEntry model{.model_id = "m", .display_name = "M", .backend = "mlx"};
    model.assets = {
        ModelAsset{.role = "weights", .checkpoint_ref = server.url("/m.mlxfn"),
                   .sha256 = sha256_hex(weights)},
        ModelAsset{.role = "state", .checkpoint_ref = server.url("/m_state.safetensors"),
                   .sha256 = std::string(64, 'a')},  // wrong on purpose
    };

    auto inst = install_model(model, "magenta", /*on_progress=*/{}, /*cancel=*/nullptr,
                              /*headers=*/{}, home);
    REQUIRE_FALSE(inst.ok);
    REQUIRE(inst.error.find("mismatch") != std::string::npos);

    fs::remove_all(root);
}

TEST_CASE("model store: install_model rejects a bundle whose assets collide on filename",
          "[runtime][model][download]") {
    // Two assets from different paths but the same leaf name would overwrite each other on
    // disk (destinations are URL basenames so the engine can find sibling files); the install
    // must fail loudly rather than report success with one file silently clobbered.
    const fs::path root = fs::temp_directory_path() / "pulp-mm-multiasset-collide";
    fs::remove_all(root);
    fs::create_directories(root / "serve" / "a");
    fs::create_directories(root / "serve" / "b");
    make_fixture(root / "serve" / "a" / "model.bin", 1'000);
    make_fixture(root / "serve" / "b" / "model.bin", 1'000);

    LocalServer server(root / "serve");
    const fs::path home = root / "home";

    ModelEntry model{.model_id = "dup", .display_name = "Dup", .backend = "mlx"};
    model.assets = {
        ModelAsset{.role = "weights", .checkpoint_ref = server.url("/a/model.bin")},
        ModelAsset{.role = "state", .checkpoint_ref = server.url("/b/model.bin")},
    };

    auto inst = install_model(model, "magenta", /*on_progress=*/{}, /*cancel=*/nullptr,
                              /*headers=*/{}, home);
    REQUIRE_FALSE(inst.ok);
    REQUIRE(inst.error.find("same file") != std::string::npos);

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}
