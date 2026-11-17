// =============================================================================
//  h2 over TLS + ALPN（S4）
// =============================================================================
//  对端用 **netmech 自己的 TLS 客户端**，而不是在测试里直接引 OpenSSL：
//    · 这样验证的是"同一条链"——netmech 的 alpn_protocol() 与 h2mech 的
//      分发逻辑用的是同一套语义；
//    · h2mech 的测试不需要 OpenSSL 头/库，NETMECH_TLS=OFF 的构建照样能编
//      （本文件整体被 #if 排除）。
//
//  证书是**内嵌的 PEM**（自签、CN=localhost、有效期 100 年），避免运行时
//  生成需要 OpenSSL C API。私钥只用于本仓测试，与任何生产部署无关。
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/h2/server.hpp>

#if defined(NETMECH_HAS_TLS) && defined(H2MECH_HAS_NGHTTP2)

#include <netmech/conn.hpp>
#include <netmech/iohandler.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>
#include <netmech/tls.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace {

/// 测试证书（自签 / CN=localhost / 2026-11-17 起 100 年）。
constexpr const char* kCertPem = R"(-----BEGIN CERTIFICATE-----
MIIBgDCCASWgAwIBAgIUJpdKYLQFVsRMFMoqYf3nackfc+gwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTExNzAzNDczOVoYDzIxMjYxMDI0
MDM0NzM5WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAARdXMEXBTBSsoO1sOB1Z/ROVgQqGeXPbwLQBAI76SMqYC8Ir+hQs0w2
5qbmDfDoOhhRWjmktuuDEkAWGhDOGNsjo1MwUTAdBgNVHQ4EFgQUBtZ8G5wSLNye
BCc0m9+0wJ/JHQAwHwYDVR0jBBgwFoAUBtZ8G5wSLNyeBCc0m9+0wJ/JHQAwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNJADBGAiEAh4bfdFbHOvGKrrX+anOD
GwoZpOp3lQoGks35270qxp4CIQC41xXw5gz1xjqBvV+lUYZUJUOxGBsmMi5SFKNk
ppr7yw==
-----END CERTIFICATE-----
)";

constexpr const char* kKeyPem = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEINkks9rnJc8ICeysH/XQJTn+q1tZbOKU9gIBUcO/mbsloAoGCCqGSM49
AwEHoUQDQgAEXVzBFwUwUrKDtbDgdWf0TlYEKhnlz28C0AQCO+kjKmAvCK/oULNM
Nuam5g3w6DoYUVo5pLbrgxJAFhoQzhjbIw==
-----END EC PRIVATE KEY-----
)";

/// 上面那张证书的 SHA-256 指纹（allow ':' 分隔，netmech 会归一化）。
constexpr const char* kFingerprint =
    "4C:30:37:EB:7F:01:28:06:84:84:73:03:7C:56:B1:C3:"
    "25:A5:A8:0C:CB:6E:CD:74:B9:07:4C:24:8E:F6:DA:59";

std::string write_temp(const char* suffix, const char* content)
{
    static std::atomic<unsigned> seq{0};
    const auto dir  = std::filesystem::temp_directory_path();
    const auto name = std::string("h2mech_tls_") + std::to_string(seq.fetch_add(1)) + suffix;
    const auto path = (dir / name).string();

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

/// TLS 上的 h2 客户端：netmech 负责 TLS，nghttp2 负责 h2。
class TlsH2Client final : public netmech::Linkable,
                          public netmech::ConnHandler,
                          public std::enable_shared_from_this<TlsH2Client> {
public:
    explicit TlsH2Client(std::string path) : path_(std::move(path)) {}

    ~TlsH2Client() override
    {
        if (sess_ != nullptr) {
            nghttp2_session_del(sess_);
        }
    }

    // ---- 观测量（测试线程读，回调线程写；用 mutex 保护 done/状态）----
    std::string     alpn;        ///< 客户端看到的 ALPN 结果
    int             status = 0;
    std::string     body;
    bool            got_status = false;
    std::error_code linkfail;

    // ---- Linkable ----

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override
    {
        alpn = c.alpn_protocol();   // 握手刚完成，这里一定是协商结果
        return shared_from_this();
    }

    void on_linkfail(const netmech::Address&, std::error_code ec) override
    {
        linkfail = ec;
        notify();
    }

    void on_linkdown(netmech::Conn&) override { notify(); }

    // ---- ConnHandler ----

    bool on_open(std::shared_ptr<netmech::Conn> ep) override
    {
        conn_ = std::move(ep);
        if (!conn_) {
            return false;
        }
        if (!init()) {
            return false;
        }
        pump();
        return do_read();
    }

    void on_read(std::error_code ec, netmech::Buf /*buf*/, std::size_t n) override
    {
        if (ec) {
            return;
        }
        if (n > 0) {
            if (nghttp2_session_mem_recv(sess_, buf_.data(), n) < 0) {
                notify();
                return;
            }
            pump();
        }
        if (nghttp2_session_want_read(sess_) == 0 && nghttp2_session_want_write(sess_) == 0) {
            notify();
            return;
        }
        do_read();
    }

    void on_write(std::error_code ec, netmech::ConstBuf /*buf*/, std::size_t /*n*/) override
    {
        if (ec) {
            return;
        }
        pump();
    }

    void on_close() override
    {
        conn_.reset();   // 断开强引用，避免与 Conn::Impl 成环
        notify();
    }

    /// 等到"流关闭"或"连接关闭"或超时。
    bool wait_closed(int timeout_ms)
    {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return done_; });
    }

    /// 测试线程收尾用：关连接并断开对 Conn 的强引用（不做会成环泄漏）。
    void shutdown()
    {
        if (conn_) {
            conn_->close();
        }
        conn_.reset();
    }

private:
    void notify()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            done_ = true;
        }
        cv_.notify_all();
    }

    bool do_read()
    {
        return conn_ && conn_->read(netmech::Buf{nullptr, buf_.data(), buf_.size()});
    }

    /// 把 nghttp2 里待发的字节交给 Conn（未受理的留到 on_write 续发）。
    void pump()
    {
        if (sess_ == nullptr || !conn_) {
            return;
        }
        while (!pending_.empty()) {
            const auto& sp = pending_.front();
            if (!conn_->write(netmech::ConstBuf{sp, sp->data(), sp->size()})) {
                return;
            }
            pending_.pop_front();
        }
        for (;;) {
            const std::uint8_t* data = nullptr;
            const ssize_t        n    = nghttp2_session_mem_send(sess_, &data);
            if (n <= 0) {
                break;
            }
            // mem_send 的指针只在下次调用前有效 → 拷一份，并作为 owner 持有
            auto sp = std::make_shared<std::string>(reinterpret_cast<const char*>(data),
                                                    static_cast<std::size_t>(n));
            if (!conn_->write(netmech::ConstBuf{sp, sp->data(), sp->size()})) {
                pending_.push_back(std::move(sp));
                return;
            }
        }
    }

    bool init()
    {
        nghttp2_session_callbacks* cbs = nullptr;
        nghttp2_session_callbacks_new(&cbs);
        nghttp2_session_callbacks_set_on_header_callback(cbs, &TlsH2Client::on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, &TlsH2Client::on_data);
        nghttp2_session_callbacks_set_on_stream_close_callback(cbs, &TlsH2Client::on_close_stream);
        nghttp2_session_client_new(&sess_, cbs, this);
        nghttp2_session_callbacks_del(cbs);
        if (sess_ == nullptr) {
            return false;
        }

        // 客户端前言 = magic（nghttp2 自动）+ SETTINGS（必须应用提交）
        if (nghttp2_submit_settings(sess_, NGHTTP2_FLAG_NONE, nullptr, 0) != 0) {
            return false;
        }

        const std::vector<nghttp2_nv> nva = {
            {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":method")),
             const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>("GET")),
             sizeof(":method") - 1, sizeof("GET") - 1, NGHTTP2_NV_FLAG_NONE},
            {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":scheme")),
             const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>("https")),
             sizeof(":scheme") - 1, sizeof("https") - 1, NGHTTP2_NV_FLAG_NONE},
            {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":authority")),
             const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>("localhost")),
             sizeof(":authority") - 1, sizeof("localhost") - 1, NGHTTP2_NV_FLAG_NONE},
            {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":path")),
             const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(path_.c_str())),
             sizeof(":path") - 1, path_.size(), NGHTTP2_NV_FLAG_NONE},
        };
        return nghttp2_submit_request(sess_, nullptr, nva.data(), nva.size(), nullptr,
                                      nullptr) >= 0;
    }

    static int on_header(nghttp2_session*, const nghttp2_frame* frame, const std::uint8_t* name,
                         std::size_t namelen, const std::uint8_t* value, std::size_t valuelen,
                         std::uint8_t, void* ud)
    {
        auto* self = static_cast<TlsH2Client*>(ud);
        if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
            return 0;
        }
        const std::string n(reinterpret_cast<const char*>(name), namelen);
        if (n == ":status") {
            self->status = std::atoi(std::string(reinterpret_cast<const char*>(value),
                                                 valuelen).c_str());
            self->got_status = true;
        }
        return 0;
    }

    static int on_data(nghttp2_session*, std::uint8_t, std::int32_t, const std::uint8_t* data,
                       std::size_t len, void* ud)
    {
        static_cast<TlsH2Client*>(ud)->body.append(reinterpret_cast<const char*>(data), len);
        return 0;
    }

    static int on_close_stream(nghttp2_session*, std::int32_t, std::uint32_t, void* ud)
    {
        static_cast<TlsH2Client*>(ud)->notify();
        return 0;
    }

    std::string       path_;
    std::shared_ptr<netmech::Conn> conn_;
    nghttp2_session*  sess_ = nullptr;
    std::array<std::uint8_t, 16 * 1024> buf_{};
    std::deque<std::shared_ptr<std::string>> pending_;

    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    done_ = false;
};

/// 把请求的 url 回显成正文。
struct PathEcho final : public h2mech::h2::Handler {
    void on_request(h2mech::h2::Request& req, h2mech::h2::Responder& out) override
    {
        scheme = req.scheme;          // h2 over TLS 时对端给的是 https
        out.send_headers(200, {{"content-type", "text/plain"}});
        out.send_data(req.url);
        out.finish();
    }
    std::string scheme;
};

struct TlsFixture {
    std::string cert_path;
    std::string key_path;
    ~TlsFixture()
    {
        if (!cert_path.empty()) std::remove(cert_path.c_str());
        if (!key_path.empty()) std::remove(key_path.c_str());
    }
};

} // namespace

// -----------------------------------------------------------------------------
// 用例
// -----------------------------------------------------------------------------

H2MECH_TEST(h2_tls_alpn_negotiates_h2_and_roundtrips)
{
    TlsFixture cert;
    cert.cert_path = write_temp(".crt.pem", kCertPem);
    cert.key_path  = write_temp(".key.pem", kKeyPem);

    auto srv_tls             = std::make_shared<netmech::TlsConfig>();
    srv_tls->cert_chain_file = cert.cert_path;
    srv_tls->private_key_file = cert.key_path;
    // 服务端不需要显式配 alpn：Server::start() 会强制成 {"h2"}

    auto handler = std::make_shared<PathEcho>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};
    cfg.tls   = srv_tls;

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    // 客户端：自签 + 指纹 pin（产品里的默认形态），只谈 h2
    netmech::TlsConfig cli_tls;
    cli_tls.pinned_sha256 = kFingerprint;
    cli_tls.server_name   = "localhost";
    cli_tls.alpn          = {"h2"};

    auto mech   = netmech::Mechanism::create();
    auto client = std::make_shared<TlsH2Client>("/tls/echo");
    REQUIRE(mech != nullptr);

    netmech::ConnectOptions co;
    co.tls = cli_tls;

    const netmech::Address remote{"127.0.0.1", srv->local().port()};
    const std::error_code  ec = mech->connect(remote, client, co);
    CHECK_MSG(!ec, "connect 受理失败: " + ec.message());

    REQUIRE(client->wait_closed(5000));

    // ALPN 必须协商成 h2（否则服务端会在 on_accept 里直接关连接）
    CHECK_EQ(client->alpn, std::string{"h2"});
    CHECK(client->got_status);
    CHECK_EQ(client->status, 200);
    CHECK_EQ(client->body, std::string{"/tls/echo"});
    CHECK_EQ(handler->scheme, std::string{"https"});   // :scheme 由客户端给

    client->shutdown();
    mech->close_all();
    srv->stop();
}

H2MECH_TEST(h2_tls_rejects_client_without_h2_alpn)
{
    TlsFixture cert;
    cert.cert_path = write_temp(".crt.pem", kCertPem);
    cert.key_path  = write_temp(".key.pem", kKeyPem);

    auto srv_tls              = std::make_shared<netmech::TlsConfig>();
    srv_tls->cert_chain_file  = cert.cert_path;
    srv_tls->private_key_file = cert.key_path;

    auto handler = std::make_shared<PathEcho>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};
    cfg.tls   = srv_tls;

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    // 只谈 http/1.1：握手能成，但 ALPN 协商不出 h2 —— 本库必须直接关掉，
    // 而不是把 h1 的字节喂给 h2 状态机。
    netmech::TlsConfig cli_tls;
    cli_tls.pinned_sha256 = kFingerprint;
    cli_tls.server_name   = "localhost";
    cli_tls.alpn          = {"http/1.1"};

    auto mech   = netmech::Mechanism::create();
    auto client = std::make_shared<TlsH2Client>("/should/not/arrive");
    REQUIRE(mech != nullptr);

    netmech::ConnectOptions co;
    co.tls = cli_tls;
    const std::error_code ec =
        mech->connect(netmech::Address{"127.0.0.1", srv->local().port()}, client, co);
    CHECK_MSG(!ec, "connect 受理失败: " + ec.message());

    REQUIRE(client->wait_closed(5000));

    CHECK(client->alpn != "h2");
    CHECK(!client->got_status);        // 没有响应
    CHECK(!handler->scheme.size());    // handler 根本没被调用

    client->shutdown();
    mech->close_all();
    srv->stop();
}

#else // NETMECH_HAS_TLS && H2MECH_HAS_NGHTTP2

H2MECH_TEST(h2_tls_requires_tls_and_nghttp2_build)
{
    // 本构建没编 TLS 或没编 nghttp2：端到端 TLS 用例不适用。
    CHECK(true);
}

#endif
