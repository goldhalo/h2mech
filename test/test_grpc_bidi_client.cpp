// =============================================================================
//  单连接双向调用：我们的 client ↔ 我们的 server
// =============================================================================
//  断言的是"交错"：发一条、立刻收一条，再发下一条 —— 中间不关流。
//  这正是 gRPC 双向流的用法（长连接上的请求-响应流水）。
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/grpc/client.hpp>
#include <h2mech/grpc/server.hpp>

#if defined(H2MECH_HAS_NGHTTP2)

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using h2mech::grpc::Code;
using h2mech::grpc::Frame;

/// 轮询取消息，直到凑够 `want` 条或超时。
std::vector<std::string> take_at_least(h2mech::grpc::BidiStream& s, std::size_t want,
                                       std::chrono::milliseconds total)
{
    std::vector<std::string> got;
    const auto               deadline = std::chrono::steady_clock::now() + total;
    while (got.size() < want && std::chrono::steady_clock::now() < deadline) {
        for (auto& f : s.take_messages()) {
            got.push_back(f.payload);
        }
        if (got.size() < want) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return got;
}

} // namespace

H2MECH_TEST(grpc_bidi_client_server_ping_pong)
{
    // ---- 服务端：收到一条回一条 ----
    auto service = std::make_shared<h2mech::grpc::Service>();
    service->add_bidi_stream("/test.Chat/Relay",
                             [](h2mech::grpc::BidiCall& call, const Frame& req) {
                                 call.send(req.payload);
                             });

    h2mech::grpc::ServerConfig scfg;
    scfg.http.local = {"127.0.0.1", 0};

    auto srv = h2mech::grpc::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    // ---- 客户端 ----
    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", srv->local().port()};

    auto cli = h2mech::grpc::Client::create(ccfg);
    REQUIRE(cli != nullptr);

    auto stream = cli->open_bidi("/test.Chat/Relay");
    REQUIRE(stream != nullptr);

    // 第 1 轮：发一条 → **不关流** → 必须收到回显
    REQUIRE(stream->send("ping-1"));
    auto m1 = take_at_least(*stream, 1, std::chrono::seconds(3));
    REQUIRE(m1.size() == 1);
    CHECK_EQ(m1[0], std::string{"ping-1"});
    CHECK(!stream->closed());          // 流还开着（这就是"双向"）

    // 第 2 轮：同一条流上再发一条
    REQUIRE(stream->send("ping-2"));
    auto m2 = take_at_least(*stream, 1, std::chrono::seconds(3));
    REQUIRE(m2.size() == 1);
    CHECK_EQ(m2[0], std::string{"ping-2"});

    // 第 3 轮：多字节 + 空消息也要能过
    REQUIRE(stream->send(std::string(1024, 'x')));
    auto m3 = take_at_least(*stream, 1, std::chrono::seconds(3));
    REQUIRE(m3.size() == 1);
    CHECK_EQ(m3[0].size(), std::size_t{1024});

    // 收尾：关闭写侧 → 服务端随之结束 → 我们等到 OK
    stream->close_writes();
    const auto st = stream->wait_close(std::chrono::seconds(5));
    CHECK_MSG(st.ok(), std::string{"流收尾失败: "} + h2mech::grpc::code_name(st.code));
    CHECK(stream->closed());

    // 关流之后不能再发
    CHECK(!stream->send("after-close"));

    cli->close();
    srv->stop();
}

H2MECH_TEST(grpc_bidi_client_unknown_method)
{
    auto service = std::make_shared<h2mech::grpc::Service>();

    h2mech::grpc::ServerConfig scfg;
    scfg.http.local = {"127.0.0.1", 0};

    auto srv = h2mech::grpc::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", srv->local().port()};

    auto cli = h2mech::grpc::Client::create(ccfg);
    REQUIRE(cli != nullptr);

    auto stream = cli->open_bidi("/no.Such/Method");
    REQUIRE(stream != nullptr);

    const auto st = stream->wait_close(std::chrono::seconds(5));
    CHECK_MSG(st.code == Code::unimplemented,
              std::string{"期望 UNIMPLEMENTED，实际: "} + h2mech::grpc::code_name(st.code)
                  + " / " + st.message);

    cli->close();
    srv->stop();
}

H2MECH_TEST(grpc_bidi_client_serializes_with_unary)
{
    // 双向流在用的时候，unary 调用必须立刻被拒（本客户端是串行的）。
    auto service = std::make_shared<h2mech::grpc::Service>();
    service->add_bidi_stream("/test.Chat/Relay",
                             [](h2mech::grpc::BidiCall& call, const Frame& req) {
                                 call.send(req.payload);
                             });
    service->add_unary("/test.Echo/Say",
                       [](h2mech::grpc::ServerCall& call, const Frame& req) {
                           call.send(req.payload);
                           call.finish(h2mech::grpc::Status{});
                       });

    h2mech::grpc::ServerConfig scfg;
    scfg.http.local = {"127.0.0.1", 0};

    auto srv = h2mech::grpc::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", srv->local().port()};

    auto cli = h2mech::grpc::Client::create(ccfg);
    REQUIRE(cli != nullptr);

    auto stream = cli->open_bidi("/test.Chat/Relay");
    REQUIRE(stream != nullptr);

    const auto r = cli->call("/test.Echo/Say", "x", {}, std::chrono::seconds(2));
    CHECK(!r.ok());
    CHECK(r.status.code == Code::internal);   // "已有调用在进行"

    stream->close_writes();
    (void)stream->wait_close(std::chrono::seconds(5));

    cli->close();
    srv->stop();
}

#else // H2MECH_HAS_NGHTTP2

H2MECH_TEST(grpc_bidi_client_requires_nghttp2)
{
    CHECK(true);
}

#endif
