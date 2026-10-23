#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/http.hpp>

#include <httplib.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::runtime;

namespace {

class LoopbackServer {
public:
    httplib::Server server;

    void start() {
        port_ = server.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~LoopbackServer() {
        server.stop();
        if (thread_.joinable())
            thread_.join();
    }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    // Addresses the plain-http listener with an https scheme so the client
    // attempts a TLS handshake against a peer that speaks no TLS.
    std::string https_url(std::string_view path) const {
        return "https://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

private:
    int port_ = 0;
    std::thread thread_;
};

void serve_sse(httplib::Response& response) {
    static const std::vector<std::string> chunks = {
        "event: token\ndata: alpha\n\n",
        "event: token\ndata: beta\n\n",
        "event: done\ndata: [DONE]\n\n",
    };
    response.set_chunked_content_provider(
        "text/event-stream",
        [](size_t offset, httplib::DataSink& sink) {
            size_t consumed = 0;
            for (const auto& chunk : chunks) {
                if (consumed == offset) {
                    std::this_thread::sleep_for(10ms);
                    return sink.write(chunk.data(), chunk.size());
                }
                consumed += chunk.size();
            }
            sink.done();
            return true;
        });
}

}  // namespace

TEST_CASE("HTTP request sends custom headers without reflecting their values",
          "[runtime][http]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    std::string received_authorization;
    loopback.server.Get("/headers",
                        [&](const httplib::Request& request, httplib::Response& response) {
                            {
                                std::lock_guard lock(received_mutex);
                                received_authorization =
                                    request.get_header_value("Authorization");
                            }
                            response.set_content("accepted", "text/plain");
                        });
    loopback.start();

    const std::string sensitive_marker = "Bearer loopback-sensitive-marker";
    HttpRequest request;
    request.url = loopback.url("/headers");
    request.headers["Authorization"] = sensitive_marker;

    const auto response = http_request(request);

    REQUIRE(response.ok());
    REQUIRE(response.body == "accepted");
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(received_authorization == sensitive_marker);
    }
    REQUIRE(response.error.find(sensitive_marker) == std::string::npos);
}

TEST_CASE("HTTP request delivers chunked SSE incrementally", "[runtime][http]") {
    LoopbackServer loopback;
    loopback.server.Get("/events",
                        [](const httplib::Request&, httplib::Response& response) {
                            serve_sse(response);
                        });
    loopback.start();

    std::string delivered;
    size_t callback_count = 0;
    HttpRequest request;
    request.url = loopback.url("/events");
    request.on_chunk = [&](std::string_view chunk) {
        ++callback_count;
        delivered.append(chunk);
        return true;
    };

    const auto response = http_request(request);

    REQUIRE(response.ok());
    REQUIRE(response.body.empty());
    REQUIRE(callback_count >= 2);
    REQUIRE(delivered ==
            "event: token\ndata: alpha\n\n"
            "event: token\ndata: beta\n\n"
            "event: done\ndata: [DONE]\n\n");
}

TEST_CASE("HTTP request callback can abort a chunked response", "[runtime][http]") {
    LoopbackServer loopback;
    loopback.server.Get("/events",
                        [](const httplib::Request&, httplib::Response& response) {
                            serve_sse(response);
                        });
    loopback.start();

    size_t callback_count = 0;
    std::string delivered;
    HttpRequest request;
    request.url = loopback.url("/events");
    request.on_chunk = [&](std::string_view chunk) {
        ++callback_count;
        delivered.append(chunk);
        return false;
    };

    const auto response = http_request(request);

    REQUIRE(response.status_code == 200);
    REQUIRE_FALSE(response.ok());
    REQUIRE(response.error == "Response callback aborted request");
    REQUIRE(callback_count == 1);
    REQUIRE_FALSE(delivered.empty());
    REQUIRE(delivered.find("[DONE]") == std::string::npos);
}

TEST_CASE("HTTP request enforces its timeout after a healthy control request",
          "[runtime][http]") {
    LoopbackServer loopback;
    loopback.server.Get("/ready",
                        [](const httplib::Request&, httplib::Response& response) {
                            response.set_content("ready", "text/plain");
                        });
    loopback.server.Get("/slow",
                        [](const httplib::Request&, httplib::Response& response) {
                            std::this_thread::sleep_for(1500ms);
                            response.set_content("late", "text/plain");
                        });
    loopback.start();

    REQUIRE(http_get(loopback.url("/ready"), 1).ok());

    HttpRequest request;
    request.url = loopback.url("/slow");
    request.timeout_seconds = 1;
    request.headers["Authorization"] = "Bearer timeout-sensitive-marker";
    const auto started = std::chrono::steady_clock::now();
    const auto response = http_request(request);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(response.ok());
    REQUIRE(response.status_code == 0);
    REQUIRE_FALSE(response.error.empty());
    REQUIRE(response.error.find("timeout-sensitive-marker") == std::string::npos);
    REQUIRE(elapsed < 2500ms);
}

TEST_CASE("HTTPS request to a closed port reports an error without throwing",
          "[runtime][http][tls]") {
    HttpResponse response;
    REQUIRE_NOTHROW(response = http_get("https://127.0.0.1:1/", 2));

    REQUIRE_FALSE(response.ok());
    REQUIRE(response.status_code == 0);
    REQUIRE_FALSE(response.error.empty());
}

TEST_CASE("HTTPS request to a plain-http loopback server fails the TLS handshake",
          "[runtime][http][tls]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    size_t handled_requests = 0;
    loopback.server.Get("/tls", [&](const httplib::Request&, httplib::Response& response) {
        {
            std::lock_guard lock(received_mutex);
            ++handled_requests;
        }
        response.set_content("plaintext", "text/plain");
    });
    loopback.start();

    // Control: the same listener serves the same path over plain http.
    REQUIRE(http_get(loopback.url("/tls"), 2).ok());

    HttpResponse response;
    REQUIRE_NOTHROW(response = http_get(loopback.https_url("/tls"), 2));

    REQUIRE_FALSE(response.ok());
    REQUIRE(response.status_code == 0);
    REQUIRE_FALSE(response.error.empty());
    REQUIRE(response.body.empty());
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(handled_requests == 1);
    }
}

TEST_CASE("legacy HTTP GET and POST overloads retain buffered behavior",
          "[runtime][http]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    std::string received_content_type;
    loopback.server.Get("/legacy-get",
                        [](const httplib::Request&, httplib::Response& response) {
                            response.set_header("X-Compatibility", "get");
                            response.set_content("legacy-get", "text/plain");
                        });
    loopback.server.Post(
        "/legacy-post",
        [&](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard lock(received_mutex);
                received_content_type = request.get_header_value("Content-Type");
            }
            response.set_content(request.body, "text/plain");
        });
    loopback.start();

    const auto get_response = http_get(loopback.url("/legacy-get"), 2);
    const auto post_response =
        http_post(loopback.url("/legacy-post"), "legacy-post", "text/x-pulp", 2);

    REQUIRE(get_response.ok());
    REQUIRE(get_response.body == "legacy-get");
    REQUIRE(get_response.headers.at("X-Compatibility") == "get");
    REQUIRE(post_response.ok());
    REQUIRE(post_response.body == "legacy-post");
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(received_content_type == "text/x-pulp");
    }
}

TEST_CASE("HTTP POST content type handling remains compatible and case insensitive",
          "[runtime][http]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    size_t custom_type_count = 0;
    std::string custom_type;
    size_t empty_legacy_type_count = 0;
    std::string empty_legacy_type;
    loopback.server.Post(
        "/custom-type",
        [&](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard lock(received_mutex);
                custom_type_count = request.get_header_value_count("Content-Type");
                custom_type = request.get_header_value("Content-Type");
            }
            response.set_content("custom", "text/plain");
        });
    loopback.server.Post(
        "/empty-legacy-type",
        [&](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard lock(received_mutex);
                empty_legacy_type_count =
                    request.get_header_value_count("Content-Type");
                empty_legacy_type = request.get_header_value("Content-Type");
            }
            response.set_content("legacy", "text/plain");
        });
    loopback.start();

    HttpRequest custom_request;
    custom_request.url = loopback.url("/custom-type");
    custom_request.method = "POST";
    custom_request.body = "payload";
    custom_request.headers["content-type"] = "application/x-custom";
    const auto custom_response = http_request(custom_request);
    const auto legacy_response =
        http_post(loopback.url("/empty-legacy-type"), "payload", "", 2);

    REQUIRE(custom_response.ok());
    REQUIRE(legacy_response.ok());
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(custom_type_count == 1);
        REQUIRE(custom_type == "application/x-custom");
        REQUIRE(empty_legacy_type_count == 1);
        REQUIRE(empty_legacy_type == "text/plain");
    }
}

// ── TLS verification against a real TLS listener ─────────────────────────────
//
// Refusing a peer that speaks no TLS is a property a build with TLS compiled
// out also "passes", by rejecting the scheme. This fixture is a listener that
// really speaks TLS, presenting a certificate generated at test time (never
// checked in, never written to disk), so the assertion is about certificate
// verification rather than about the scheme.

namespace {

struct SelfSignedCertificate {
    std::string cert_pem;
    std::string key_pem;
};

// A P-256 key and a self-signed certificate for CN=localhost, valid now.
SelfSignedCertificate make_self_signed_certificate() {
    SelfSignedCertificate out;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    const char* personalization = "pulp-test-http-tls";
    REQUIRE(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char*>(personalization),
                                  std::strlen(personalization)) == 0);
    REQUIRE(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
    REQUIRE(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                mbedtls_ctr_drbg_random, &drbg) == 0);

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    REQUIRE(mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost") == 0);
    REQUIRE(mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost") == 0);
    unsigned char serial[] = {0x01};
    REQUIRE(mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) == 0);
    REQUIRE(mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20991231235959") == 0);
    REQUIRE(mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0);

    std::vector<unsigned char> buffer(8192, 0);
    REQUIRE(mbedtls_x509write_crt_pem(&crt, buffer.data(), buffer.size(), mbedtls_ctr_drbg_random,
                                      &drbg) == 0);
    out.cert_pem = reinterpret_cast<const char*>(buffer.data());
    std::fill(buffer.begin(), buffer.end(), 0);
    REQUIRE(mbedtls_pk_write_key_pem(&key, buffer.data(), buffer.size()) == 0);
    out.key_pem = reinterpret_cast<const char*>(buffer.data());

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return out;
}

class TlsLoopbackServer {
  public:
    explicit TlsLoopbackServer(const SelfSignedCertificate& certificate)
        : server_(httplib::SSLServer::PemMemory{
              certificate.cert_pem.c_str(), certificate.cert_pem.size(),
              certificate.key_pem.c_str(), certificate.key_pem.size(), nullptr, 0, nullptr}) {}

    httplib::SSLServer& server() {
        return server_;
    }

    void start() {
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~TlsLoopbackServer() {
        server_.stop();
        if (thread_.joinable())
            thread_.join();
    }

    int port() const {
        return port_;
    }

    // Addressed by name, not by IP literal: the client's TLS backend verifies
    // the chain for a hostname, whereas an IP-literal host takes a separate
    // path that this case does not cover.
    std::string url(std::string_view path) const {
        return "https://localhost:" + std::to_string(port_) + std::string(path);
    }

  private:
    httplib::SSLServer server_;
    int port_ = 0;
    std::thread thread_;
};

} // namespace

TEST_CASE("HTTPS request to a loopback TLS server with a self-signed certificate fails "
          "verification",
          "[runtime][http][tls]") {
    const auto certificate = make_self_signed_certificate();
    TlsLoopbackServer loopback(certificate);
    REQUIRE(loopback.server().is_valid());

    std::mutex received_mutex;
    size_t handled_requests = 0;
    loopback.server().Get("/tls", [&](const httplib::Request&, httplib::Response& response) {
        {
            std::lock_guard lock(received_mutex);
            ++handled_requests;
        }
        response.set_content("secret", "text/plain");
    });
    loopback.start();

    // Control: a client that trusts exactly this certificate completes the
    // handshake and reads the body, so the listener speaks TLS and the
    // certificate is valid for localhost. Only the trust decision differs below.
    {
        httplib::SSLClient trusting("localhost", loopback.port());
        trusting.load_ca_cert_store(certificate.cert_pem.c_str(), certificate.cert_pem.size());
        trusting.set_connection_timeout(2);
        trusting.set_read_timeout(2);
        auto result = trusting.Get("/tls");
        REQUIRE(result);
        REQUIRE(result->status == 200);
        REQUIRE(result->body == "secret");
    }

    HttpResponse response;
    REQUIRE_NOTHROW(response = http_get(loopback.url("/tls"), 2));
    CAPTURE(response.status_code, response.error, response.body);

    // The refusal comes from the TLS layer ("SSL server verification failed"
    // with a system trust store, "SSL connection failed" on a host without
    // one), never from the scheme or the socket, and the handshake aborts
    // before a request reaches the handler.
    REQUIRE_FALSE(response.ok());
    REQUIRE(response.status_code == 0);
    REQUIRE(response.body.empty());
    REQUIRE(response.error.find("SSL") != std::string::npos);
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(handled_requests == 1);
    }
}
