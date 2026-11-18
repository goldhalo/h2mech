// =============================================================================
//  gRPC 双向流（**单连接、单流、边发边收**）
// =============================================================================
//  用裸 h2 帧驱动（不依赖客户端实现），这样断言的是**服务端行为**：
//
//    发 gRPC 消息 "one"（不带 END_STREAM）
//    ← 必须**立刻**收到回显 "one"（请求还没结束）
//    发 gRPC 消息 "two"（带 END_STREAM）
//    ← 收到 "two" + END_STREAM
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/grpc/framer.hpp>
#include <h2mech/grpc/server.hpp>

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
#    define H2_BIDI_CLOSE closesocket
#else
void ensure_winsock() {}
#    define H2_BIDI_CLOSE ::close
#endif

std::string frame(std::uint8_t type, std::uint8_t flags, std::int32_t sid, std::string_view payload)
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

void add_literal(std::string& out, std::string_view name, std::string_view value)
{
    out.push_back('\x00');
    out.push_back(static_cast<char>(name.size()));
    out.append(name.data(), name.size());
    out.push_back(static_cast<char>(value.size()));
    out.append(value.data(), value.size());
}

/// 裸 h2 连接（阻塞 + 超时）。
class Link {
public:
    ~Link() { close(); }

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

    bool write_all(std::string_view s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const int n = static_cast<int>(::send(fd_, s.data() + off,
                                                  static_cast<int>(s.size() - off), 0));
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
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
            H2_BIDI_CLOSE(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

/// 从收到的字节里抽出 DATA 负载与 END_STREAM 标志。
struct Collected {
    std::string data;
    bool        end_stream = false;
};

void scan(std::string& buf, Collected& out)
{
    for (;;) {
        if (buf.size() < 9) {
            return;
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(buf.data());
        const std::uint32_t len = (static_cast<std::uint32_t>(p[0]) << 16)
                                  | (static_cast<std::uint32_t>(p[1]) << 8)
                                  | static_cast<std::uint32_t>(p[2]);
        if (buf.size() < 9 + len) {
            return;
        }
        const std::uint8_t type  = p[3];
        const std::uint8_t flags = p[4];
        if (type == 0x00) {                       // DATA
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

/// 把收到的 DATA 字节解成 gRPC 消息列表。
std::vector<std::string> messages_of(const std::string& data)
{
    std::vector<std::string> out;
    h2mech::grpc::MessageReader reader;
    if (!reader.feed(data)) {
        return out;
    }
    h2mech::grpc::Frame f;
    while (reader.next(f)) {
        out.push_back(f.payload);
    }
    return out;
}

} // namespace

H2MECH_TEST(grpc_bidi_server_relays_messages_before_end_stream)
{
    auto service = std::make_shared<h2mech::grpc::Service>();
    // 双向流：每收到一条就立刻回一条（不回完不收尾）
    service->add_bidi_stream("/test.Chat/Relay",
                             [](h2mech::grpc::BidiCall& call, const h2mech::grpc::Frame& req) {
                                 call.send(req.payload);
                             });

    h2mech::grpc::ServerConfig scfg;
    scfg.http.local = {"127.0.0.1", 0};

    auto srv = h2mech::grpc::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    Link link;
    REQUIRE(link.connect(srv->local().port()));

    // 前言 + HEADERS（POST + application/grpc + te），不带 END_STREAM
    std::string out;
    out.append("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24);
    out += frame(0x04, 0x00, 0, std::string_view{});   // SETTINGS

    std::string hp;
    add_literal(hp, ":method", "POST");
    add_literal(hp, ":scheme", "http");
    add_literal(hp, ":path", "/test.Chat/Relay");
    add_literal(hp, ":authority", "localhost");
    add_literal(hp, "content-type", "application/grpc");
    add_literal(hp, "te", "trailers");
    out += frame(0x01, 0x04, 1, hp);                   // END_HEADERS
    REQUIRE(link.write_all(out));

    // 第 1 条请求消息
    REQUIRE(link.write_all(frame(0x00, 0x00, 1, h2mech::grpc::encode_message("one"))));

    std::string pending;
    Collected   got;
    std::uint8_t buf[4096];
    for (int i = 0; i < 8 && messages_of(got.data).empty(); ++i) {
        const int n = link.read_some(buf, sizeof(buf));
        REQUIRE(n > 0);
        pending.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        scan(pending, got);
    }

    // 关键：**请求还没结束**，第一条回显必须已经到
    auto msgs = messages_of(got.data);
    REQUIRE(msgs.size() == 1);
    CHECK_EQ(msgs[0], std::string{"one"});
    CHECK(!got.end_stream);

    // 第 2 条 + END_STREAM
    REQUIRE(link.write_all(
        frame(0x00, 0x01, 1, h2mech::grpc::encode_message("two"))));

    for (int i = 0; i < 8 && !got.end_stream; ++i) {
        const int n = link.read_some(buf, sizeof(buf));
        REQUIRE(n > 0);
        pending.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        scan(pending, got);
    }

    msgs = messages_of(got.data);
    REQUIRE(msgs.size() == 2);
    CHECK_EQ(msgs[0], std::string{"one"});
    CHECK_EQ(msgs[1], std::string{"two"});
    CHECK(got.end_stream);   // 对端半关闭 → 服务端收尾

    srv->stop();
}

H2MECH_TEST(grpc_bidi_unknown_method_is_unimplemented)
{
    auto service = std::make_shared<h2mech::grpc::Service>();

    h2mech::grpc::ServerConfig scfg;
    scfg.http.local = {"127.0.0.1", 0};

    auto srv = h2mech::grpc::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    Link link;
    REQUIRE(link.connect(srv->local().port()));

    std::string out;
    out.append("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24);
    out += frame(0x04, 0x00, 0, std::string_view{});

    std::string hp;
    add_literal(hp, ":method", "POST");
    add_literal(hp, ":scheme", "http");
    add_literal(hp, ":path", "/no.Such/Method");
    add_literal(hp, ":authority", "localhost");
    add_literal(hp, "content-type", "application/grpc");
    out += frame(0x01, 0x05, 1, hp);   // END_HEADERS | END_STREAM
    REQUIRE(link.write_all(out));

    // 只要不崩、能收到 END_STREAM 即可（状态码在 HEADERS 里，未解 HPACK）
    std::string  pending;
    Collected    got;
    std::uint8_t buf[4096];
    for (int i = 0; i < 8 && !got.end_stream; ++i) {
        const int n = link.read_some(buf, sizeof(buf));
        REQUIRE(n > 0);
        pending.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        scan(pending, got);
    }
    CHECK(got.end_stream);
    CHECK(messages_of(got.data).empty());   // 没有正文

    srv->stop();
}

#else // H2MECH_HAS_NGHTTP2

H2MECH_TEST(grpc_bidi_requires_nghttp2)
{
    CHECK(true);
}

#endif
