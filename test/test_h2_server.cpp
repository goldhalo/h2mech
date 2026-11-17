// =============================================================================
//  h2c 服务端端到端（用 nghttp2 的 **客户端** 模式当对端）
// =============================================================================
//  为什么客户端也自己写而不用 curl/h2load：
//    · 不引外部工具，CI 里零依赖；
//    · nghttp2 同一份库既能当 server 也能当 client，正好做"自己跟自己打通"。
//  为什么用**阻塞 socket**：测试只需要"发一个请求、收一个响应"，
//  用 netmech 的异步客户端会把测试写成一坨回调；阻塞版 20 行就够。
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/h2/server.hpp>

#if defined(H2MECH_HAS_NGHTTP2)

#include <nghttp2/nghttp2.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#    define H2_CLOSE_SOCKET closesocket
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <sys/socket.h>
#    include <unistd.h>
#    define H2_CLOSE_SOCKET ::close
#endif

namespace {

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit()
    {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
void ensure_winsock()
{
    static WinsockInit once;
    (void)once;
}
#else
void ensure_winsock() {}
#endif

/// 一条阻塞 TCP 连接（测试专用）。
class RawConn {
public:
    ~RawConn() { close(); }

    bool connect(std::uint16_t port)
    {
        ensure_winsock();
        fd_ = static_cast<decltype(fd_)>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }
        return true;
    }

    bool write_all(const std::uint8_t* data, std::size_t len)
    {
        std::size_t off = 0;
        while (off < len) {
            const int n = static_cast<int>(::send(fd_, reinterpret_cast<const char*>(data + off),
                                                  static_cast<int>(len - off), 0));
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// @return >0 读到的字节数；0 = 对端关闭；<0 = 出错
    int read_some(std::uint8_t* buf, std::size_t len)
    {
        return static_cast<int>(::recv(fd_, reinterpret_cast<char*>(buf),
                                       static_cast<int>(len), 0));
    }

    void close()
    {
        if (fd_ >= 0) {
            H2_CLOSE_SOCKET(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

struct ClientResult {
    bool        ok = false;
    int         status = 0;
    std::string body;
    h2mech::h2::HeaderList trailers;
};

struct ClientCtx {
    ClientResult result;
    bool         done = false;
    bool         got_status = false;
};

int client_on_header(nghttp2_session*, const nghttp2_frame* frame, const std::uint8_t* name,
                     std::size_t namelen, const std::uint8_t* value, std::size_t valuelen,
                     std::uint8_t, void* ud)
{
    auto* ctx = static_cast<ClientCtx*>(ud);
    const std::string n(reinterpret_cast<const char*>(name), namelen);
    const std::string v(reinterpret_cast<const char*>(value), valuelen);

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        if (n == ":status") {
            ctx->result.status = std::atoi(v.c_str());
            ctx->got_status    = true;
        }
    } else if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
        ctx->result.trailers.push_back(h2mech::h2::Header{n, v});   // trailers
    }
    return 0;
}

int client_on_data(nghttp2_session*, std::uint8_t, std::int32_t, const std::uint8_t* data,
                   std::size_t len, void* ud)
{
    auto* ctx = static_cast<ClientCtx*>(ud);
    ctx->result.body.append(reinterpret_cast<const char*>(data), len);
    return 0;
}

int client_on_stream_close(nghttp2_session*, std::int32_t, std::uint32_t, void* ud)
{
    static_cast<ClientCtx*>(ud)->done = true;
    return 0;
}

/// 请求正文的来源（nghttp2 的 data provider 只给裸指针，偏移量得自己记）。
struct BodySource {
    std::string data;
    std::size_t offset = 0;
};

ssize_t client_body_read(nghttp2_session*, std::int32_t, std::uint8_t* buf, std::size_t length,
                         std::uint32_t* flags, nghttp2_data_source* source, void*)
{
    auto* s = static_cast<BodySource*>(source->ptr);
    if (s == nullptr) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    const std::size_t left = s->data.size() - s->offset;
    if (left == 0) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    const std::size_t n = (left < length) ? left : length;
    std::memcpy(buf, s->data.data() + s->offset, n);
    s->offset += n;
    if (s->offset == s->data.size()) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<ssize_t>(n);
}

/// 发一个请求并收完整响应（阻塞，直到流关闭或对端断开）。
bool do_request(std::uint16_t port, const char* method, const char* path, const std::string& body,
                ClientResult& out)
{
    RawConn conn;
    if (!conn.connect(port)) {
        return false;
    }

    ClientCtx ctx;
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_header_callback(cbs, &client_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, &client_on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, &client_on_stream_close);

    nghttp2_session* sess = nullptr;
    nghttp2_session_client_new(&sess, cbs, &ctx);
    nghttp2_session_callbacks_del(cbs);

    const std::vector<nghttp2_nv> nva = {
        {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":method")),
         const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(method)),
         sizeof(":method") - 1, std::strlen(method), NGHTTP2_NV_FLAG_NONE},
        {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":scheme")),
         const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>("http")),
         sizeof(":scheme") - 1, sizeof("http") - 1, NGHTTP2_NV_FLAG_NONE},
        {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":authority")),
         const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>("localhost")),
         sizeof(":authority") - 1, sizeof("localhost") - 1, NGHTTP2_NV_FLAG_NONE},
        {const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(":path")),
         const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(path)),
         sizeof(":path") - 1, std::strlen(path), NGHTTP2_NV_FLAG_NONE},
    };

    // ★ 客户端连接前言是 "magic + SETTINGS"（RFC 9113 §3.4）。
    //   nghttp2 只自动发 magic，SETTINGS 必须由应用提交；漏了它，服务端会
    //   **静默丢弃**后续 HEADERS（mem_recv 照收不误、返回全量消费，但一个回调
    //   都不触发）—— 这个现象极易被误判成"回调注册错了"或"库有 bug"。
    bool ok = (nghttp2_submit_settings(sess, NGHTTP2_FLAG_NONE, nullptr, 0) == 0);

    // 请求正文：provider 的 source 指向一个活到本次调用结束的对象（同步驱动，
    // 栈上就够；真正的异步客户端要按流持有它）。
    BodySource           body_src{body, 0};
    nghttp2_data_provider provider{};
    nghttp2_data_provider* provider_ptr = nullptr;
    if (!body.empty()) {
        provider.source.ptr  = &body_src;
        provider.read_callback = &client_body_read;
        provider_ptr         = &provider;
    }

    const std::int32_t id = ok ? nghttp2_submit_request(sess, nullptr, nva.data(), nva.size(),
                                                        provider_ptr, nullptr)
                               : -1;
    ok = ok && (id >= 0);

    // 先把待发字节吐给 socket（客户端前言也在这里发出）
    for (;;) {
        const std::uint8_t* data = nullptr;
        const ssize_t        n    = nghttp2_session_mem_send(sess, &data);
        if (n <= 0) {
            break;
        }
        if (!conn.write_all(data, static_cast<std::size_t>(n))) {
            ok = false;
            break;
        }
    }

    std::uint8_t buf[16 * 1024];
    while (ok && !ctx.done) {
        const int n = conn.read_some(buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (nghttp2_session_mem_recv(sess, buf, static_cast<std::size_t>(n)) < 0) {
            ok = false;
            break;
        }
        for (;;) {
            const std::uint8_t* data = nullptr;
            const ssize_t        m    = nghttp2_session_mem_send(sess, &data);
            if (m <= 0) {
                break;
            }
            if (!conn.write_all(data, static_cast<std::size_t>(m))) {
                ok = false;
                break;
            }
        }
    }

    nghttp2_session_del(sess);

    out.ok = ok && ctx.got_status;
    if (out.ok) {
        out.status   = ctx.result.status;
        out.body     = ctx.result.body;
        out.trailers = ctx.result.trailers;
    }
    return out.ok;
}

/// 测试用 handler：把收到的正文回显，路径决定行为。
struct EchoHandler final : public h2mech::h2::Handler {
    void on_request(h2mech::h2::Request& req, h2mech::h2::Responder& out) override
    {
        last_method = req.method;
        last_url    = req.url;
        last_query  = req.query;
        last_host_header.clear();
        for (const auto& h : req.headers) {
            if (h.name == "host") {
                last_host_header = h.value;
            }
        }

        if (req.url == "/trailers") {
            out.send_headers(200, {{"content-type", "application/grpc"}});
            out.finish({{"grpc-status", "0"}});   // Trailers-Only 的常见形态
            return;
        }
        if (req.url == "/bad") {
            out.abort(h2mech::Errc::cancel);      // 让对端看到 RST_STREAM
            return;
        }

        out.send_headers(200, {{"content-type", "text/plain"}});
        out.send_data(req.body.empty() ? std::string_view{"empty"} : std::string_view{req.body});
        out.finish();
    }

    std::string last_method;
    std::string last_url;
    std::string last_query;
    std::string last_host_header;
};

} // namespace

// -----------------------------------------------------------------------------
// 用例
// -----------------------------------------------------------------------------

H2MECH_TEST(h2c_get_roundtrip)
{
    auto handler = std::make_shared<EchoHandler>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());
    REQUIRE(srv->local().port() != 0);

    ClientResult r;
    REQUIRE(do_request(srv->local().port(), "GET", "/hello?a=1", "", r));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string{"empty"});

    // 服务端看到的语义化请求
    CHECK_EQ(handler->last_method, std::string{"GET"});
    CHECK_EQ(handler->last_url, std::string{"/hello"});
    CHECK_EQ(handler->last_query, std::string{"a=1"});
    CHECK_EQ(handler->last_host_header, std::string{"localhost"});   // :authority 回填

    srv->stop();
}

H2MECH_TEST(h2c_post_body_roundtrip)
{
    auto handler = std::make_shared<EchoHandler>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    // 带正文的 POST：请求 DATA 帧 → 服务端按块收进 Request::body → 回显。
    ClientResult r;
    REQUIRE(do_request(srv->local().port(), "POST", "/echo", std::string{"hello-body"}, r));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string{"hello-body"});   // 回环一致
    CHECK_EQ(handler->last_method, std::string{"POST"});

    // 大一点的正文（会跨多个 DATA 帧）：验证分块拼接没有丢字节/错序
    std::string big(200 * 1024, 'x');
    ClientResult r2;
    REQUIRE(do_request(srv->local().port(), "POST", "/echo", big, r2));
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body.size(), big.size());

    srv->stop();
}

H2MECH_TEST(h2c_trailers_are_delivered)
{
    auto handler = std::make_shared<EchoHandler>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    ClientResult r;
    REQUIRE(do_request(srv->local().port(), "GET", "/trailers", "", r));
    CHECK_EQ(r.status, 200);

    // gRPC 的 grpc-status 就走这条路：DATA/HEADERS 之后单独一帧 trailers
    bool found = false;
    for (const auto& h : r.trailers) {
        if (h.name == "grpc-status") {
            found = true;
            CHECK_EQ(h.value, std::string{"0"});
        }
    }
    CHECK(found);

    srv->stop();
}

H2MECH_TEST(h2c_restart_after_stop)
{
    auto handler = std::make_shared<EchoHandler>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());
    const std::uint16_t port = srv->local().port();

    ClientResult r;
    REQUIRE(do_request(port, "GET", "/a", "", r));
    CHECK_EQ(r.status, 200);

    srv->stop();
    CHECK(!srv->running());

    // stop() 之后 `local()` 不能继续报旧端口（端口 0 的监听器重启后是新端口）
    CHECK_EQ(srv->local().port(), static_cast<std::uint16_t>(0));

    // 允许重启：改配置后 stop → start 是运维常见动作，没必要逼用户重建对象。
    REQUIRE(!srv->start());
    CHECK(srv->running());
    CHECK(srv->local().port() != 0);

    ClientResult again;
    REQUIRE(do_request(srv->local().port(), "GET", "/b", "", again));
    CHECK_EQ(again.status, 200);

    srv->stop();
}

#else  // H2MECH_HAS_NGHTTP2

H2MECH_TEST(h2c_requires_nghttp2)
{
    // 关掉 nghttp2 的构建只编头字段层：这里不做任何事，用例保持"能过"。
    CHECK(true);
}

#endif // H2MECH_HAS_NGHTTP2
