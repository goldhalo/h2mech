// =============================================================================
//  h2 流式接口（bidi 的地基）—— 手搓帧的裸客户端
// =============================================================================
//  为什么手搓帧而不引 nghttp2 client：要证明的是"**请求还没发完，响应就来了**"
//  这个时序，自己拼字节反而最直观：
//
//    发 HEADERS(POST /echo，不带 END_STREAM)
//    发 DATA "a"（不带 END_STREAM）
//    ← 必须立刻收到 DATA "a"（服务端在请求未结束时就能回）
//    发 DATA "b"（带 END_STREAM）
//    ← 收到 DATA "b" + END_STREAM
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/h2/server.hpp>

#if defined(H2MECH_HAS_NGHTTP2)

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "ws2_32.lib")
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
void ensure_winsock()
{
    static bool once = [] {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
        return true;
    }();
    (void)once;
}
#    define H2_CLOSE closesocket
#else
void ensure_winsock() {}
#    define H2_CLOSE ::close
#endif

const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/// 极简裸 socket（阻塞 + 接收超时兜底）。
class Raw {
public:
    ~Raw() { close(); }

    bool connect(std::uint16_t port)
    {
        ensure_winsock();
        fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }
#if defined(_WIN32)
        DWORD tv = 3000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = 3;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        return true;
    }

    bool write_all(const void* data, std::size_t n)
    {
        const auto* p   = static_cast<const std::uint8_t*>(data);
        std::size_t off = 0;
        while (off < n) {
            const int sent = static_cast<int>(
                ::send(fd_, reinterpret_cast<const char*>(p + off), static_cast<int>(n - off), 0));
            if (sent <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(sent);
        }
        return true;
    }

    int read_some(std::uint8_t* buf, std::size_t n)
    {
        return static_cast<int>(::recv(fd_, reinterpret_cast<char*>(buf), static_cast<int>(n), 0));
    }

    void close()
    {
        if (fd_ >= 0) {
            H2_CLOSE(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

/// 拼一个帧头 + 负载。
std::string make_frame(std::uint8_t type, std::uint8_t flags, std::int32_t sid,
                       std::string_view payload)
{
    std::string f;
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    f.push_back(static_cast<char>((len >> 16) & 0xFF));
    f.push_back(static_cast<char>((len >> 8) & 0xFF));
    f.push_back(static_cast<char>(len & 0xFF));
    f.push_back(static_cast<char>(type));
    f.push_back(static_cast<char>(flags));
    f.push_back(0);
    f.push_back(0);
    f.push_back(0);
    f.push_back(static_cast<char>(sid & 0xFF));
    f.append(payload.data(), payload.size());
    return f;
}

/// HPACK 字面量（无索引、无 huffman）：0x00 + 名字 + 值。
void add_literal(std::string& out, std::string_view name, std::string_view value)
{
    out.push_back('\x00');
    out.push_back(static_cast<char>(name.size()));
    out.append(name.data(), name.size());
    out.push_back(static_cast<char>(value.size()));
    out.append(value.data(), value.size());
}

/// 读到的响应里逐帧扫，收集 DATA 负载与是否见到 END_STREAM。
struct Scan {
    std::string data;
    bool        end_stream = false;
};

/// 从 buf 里尽可能多地解析帧（跨多次 read 的缓冲由调用方维护）。
void scan_frames(std::string& buf, Scan& out)
{
    for (;;) {
        if (buf.size() < 9) {
            return;
        }
        const auto* p   = reinterpret_cast<const std::uint8_t*>(buf.data());
        const std::uint32_t len =
            (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8)
            | static_cast<std::uint32_t>(p[2]);
        if (buf.size() < 9 + len) {
            return;
        }
        const std::uint8_t type  = p[3];
        const std::uint8_t flags = p[4];

        if (type == 0x00) {   // DATA
            out.data.append(buf.data() + 9, len);
            if ((flags & 0x01) != 0) {
                out.end_stream = true;
            }
        } else if (type == 0x01 && (flags & 0x01) != 0) {   // HEADERS + END_STREAM
            out.end_stream = true;
        }
        buf.erase(0, 9 + len);
    }
}

/// 服务端：把收到的每一段原样回写，对端半关闭时结束。
class EchoStream final : public h2mech::h2::StreamHandler {
public:
    void on_open(const h2mech::h2::StreamRequest& req, h2mech::h2::StreamResponder& out) override
    {
        method = req.method;
        path   = req.url;
        // 先把响应头发出去：证明"请求还没收完就能发响应"
        out.send_headers(200, {{"content-type", "text/plain"}});
    }

    void on_data(std::string_view data, bool end_stream,
                 h2mech::h2::StreamResponder& out) override
    {
        if (!data.empty()) {
            out.write(data);         // 立刻回显（不等 END_STREAM）
            echoed += std::string{data};
        }
        if (end_stream) {
            out.finish();            // 对端半关闭 → 结束响应
        }
    }

    std::string method;
    std::string path;
    std::string echoed;
};

} // namespace

H2MECH_TEST(h2_stream_bidi_echo_is_prompt)
{
    auto handler = std::make_shared<EchoStream>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    Raw raw;
    REQUIRE(raw.connect(srv->local().port()));

    // 1) 连接前言 = magic + SETTINGS
    std::string out;
    out.append(kPreface, 24);
    out += make_frame(0x04, 0x00, 0, std::string_view{});   // SETTINGS

    // 2) HEADERS：POST /echo，content-type: text/plain（**不带 END_STREAM**）
    std::string hpack;
    add_literal(hpack, ":method", "POST");
    add_literal(hpack, ":scheme", "http");
    add_literal(hpack, ":path", "/echo");
    add_literal(hpack, ":authority", "localhost");
    add_literal(hpack, "content-type", "text/plain");
    out += make_frame(0x01, 0x04, 1, hpack);   // END_HEADERS（没有 END_STREAM）

    REQUIRE(raw.write_all(out.data(), out.size()));

    // 3) 发一段正文（仍然不带 END_STREAM）
    const std::string chunk_a = make_frame(0x00, 0x00, 1, "hello-");
    REQUIRE(raw.write_all(chunk_a.data(), chunk_a.size()));

    // 4) 关键断言：**在请求还没结束的时候**就该收到回显
    std::string pending;
    Scan        scan;
    std::uint8_t buf[4096];
    for (int i = 0; i < 8 && scan.data.empty(); ++i) {
        const int n = raw.read_some(buf, sizeof(buf));
        REQUIRE(n > 0);
        pending.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        scan_frames(pending, scan);
    }
    CHECK_EQ(scan.data, std::string{"hello-"});
    CHECK(!scan.end_stream);          // 还没结束

    // 5) 再发一段并 END_STREAM
    const std::string chunk_b = make_frame(0x00, 0x01, 1, "world");
    REQUIRE(raw.write_all(chunk_b.data(), chunk_b.size()));

    for (int i = 0; i < 8 && !scan.end_stream; ++i) {
        const int n = raw.read_some(buf, sizeof(buf));
        REQUIRE(n > 0);
        pending.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        scan_frames(pending, scan);
    }

    CHECK_EQ(scan.data, std::string{"hello-world"});
    CHECK(scan.end_stream);
    CHECK_EQ(handler->method, std::string{"POST"});
    CHECK_EQ(handler->path, std::string{"/echo"});

    srv->stop();
}

H2MECH_TEST(h2_stream_server_can_reply_before_any_request_data)
{
    // 服务端流：on_open 里就 write() + finish()，请求一个字节都没发。
    struct Greeter final : public h2mech::h2::StreamHandler {
        void on_open(const h2mech::h2::StreamRequest&, h2mech::h2::StreamResponder& out) override
        {
            out.send_headers(200, {{"content-type", "text/plain"}});
            out.write("hi-");
            out.write("there");
            out.finish();
        }
        void on_data(std::string_view, bool, h2mech::h2::StreamResponder&) override {}
    };

    auto handler = std::make_shared<Greeter>();

    h2mech::h2::Server::Config cfg;
    cfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(cfg, handler);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    Raw raw;
    REQUIRE(raw.connect(srv->local().port()));

    std::string out;
    out.append(kPreface, 24);
    out += make_frame(0x04, 0x00, 0, std::string_view{});

    std::string hpack;
    add_literal(hpack, ":method", "POST");
    add_literal(hpack, ":scheme", "http");
    add_literal(hpack, ":path", "/greet");
    add_literal(hpack, ":authority", "localhost");
    out += make_frame(0x01, 0x04, 1, hpack);
    REQUIRE(raw.write_all(out.data(), out.size()));

    std::string  pending;
    Scan         scan;
    std::uint8_t buf[4096];
    for (int i = 0; i < 8 && !scan.end_stream; ++i) {
        const int n = raw.read_some(buf, sizeof(buf));
        REQUIRE(n > 0);
        pending.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        scan_frames(pending, scan);
    }

    CHECK_EQ(scan.data, std::string{"hi-there"});
    CHECK(scan.end_stream);

    srv->stop();
}

#else // H2MECH_HAS_NGHTTP2

H2MECH_TEST(h2_stream_requires_nghttp2)
{
    CHECK(true);
}

#endif
