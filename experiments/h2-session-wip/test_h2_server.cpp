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

/// 发一个请求并收完整响应（阻塞，直到流关闭或对端断开）。
bool do_request(std::uint16_t port, const char* method, const char* path, const std::string& body,
                ClientResult& out)
{
    RawConn conn;
    if (!conn.connect(port)) {
        std::fprintf(stderr, "[dbg] client connect failed port=%u\n",
                     static_cast<unsigned>(port));
        return false;
    }
    std::fprintf(stderr, "[dbg] client connected port=%u\n", static_cast<unsigned>(port));

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

    const std::int32_t id = nghttp2_submit_request(sess, nullptr, nva.data(), nva.size(),
                                                   nullptr, nullptr);
    bool ok = (id >= 0);

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

    // 注意：当前 do_request 不发送请求体（data provider 传 nullptr），
    // 所以这里验证的是"无正文的 POST 也能正常收尾"。带正文的用例等
    // S3 把 data provider 接上再补（见 README 阶段表）。
    ClientResult r;
    REQUIRE(do_request(srv->local().port(), "POST", "/echo", "", r));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(handler->last_method, std::string{"POST"});

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

    // stop() 之后不应再受理（同一个 server 重复 start 走 already_started 分支）
    CHECK(srv->start() != std::error_code{});
    srv->stop();
}

#else  // H2MECH_HAS_NGHTTP2

H2MECH_TEST(h2c_requires_nghttp2)
{
    // 关掉 nghttp2 的构建只编头字段层：这里不做任何事，用例保持"能过"。
    CHECK(true);
}

#endif // H2MECH_HAS_NGHTTP2
