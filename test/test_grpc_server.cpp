// =============================================================================
//  gRPC 服务端 —— 用本仓自己的 grpc::Client 做对端（h2c 回环）
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/grpc/client.hpp>
#include <h2mech/grpc/server.hpp>

#if defined(H2MECH_HAS_NGHTTP2)

#include <chrono>
#include <memory>
#include <string>

namespace {

using h2mech::grpc::Code;
using h2mech::grpc::Frame;

/// 起一组 server/client（客户端懒连接，第一次 call 才建链）。
struct Fixture {
    std::shared_ptr<h2mech::grpc::Server>  server;
    std::shared_ptr<h2mech::grpc::Client>  client;
    std::shared_ptr<h2mech::grpc::Service> service;

    bool start(std::size_t max_request_bytes = 4u << 20)
    {
        service = std::make_shared<h2mech::grpc::Service>();

        h2mech::grpc::ServerConfig scfg;
        scfg.http.local           = {"127.0.0.1", 0};
        scfg.max_message_bytes    = max_request_bytes;

        server = h2mech::grpc::Server::create(scfg, service);
        if (!server || server->start()) {
            return false;
        }

        h2mech::grpc::ClientConfig ccfg;
        ccfg.remote = {"127.0.0.1", server->local().port()};
        client      = h2mech::grpc::Client::create(ccfg);
        return client != nullptr;
    }

    ~Fixture()
    {
        if (client) {
            client->close();
        }
        if (server) {
            server->stop();
        }
    }
};

} // namespace

H2MECH_TEST(grpc_server_unary_roundtrip)
{
    Fixture f;
    REQUIRE(f.start());

    f.service->add_unary(
        "/test.Echo/Say",
        [](h2mech::grpc::ServerCall& call, const Frame& req) {
            // 回显：正文 = 收到的那条消息
            call.send(req.payload);
            call.finish(h2mech::grpc::Status{});
        });

    const auto r = f.client->call("/test.Echo/Say", "hello-server", {}, std::chrono::seconds(5));

    CHECK_MSG(r.ok(), std::string{"调用失败: "} + h2mech::grpc::code_name(r.status.code));
    CHECK(r.reached);
    CHECK_EQ(r.message, std::string{"hello-server"});
    CHECK_EQ(r.http_status, 200);
}

H2MECH_TEST(grpc_server_unknown_method_is_unimplemented)
{
    Fixture f;
    REQUIRE(f.start());

    // 一个方法都不注册：任何调用都必须是 UNIMPLEMENTED（Trailers-Only 形态）
    CHECK_EQ(f.service->size(), std::size_t{0});

    const auto r = f.client->call("/nope.NotThere/Method", "x", {}, std::chrono::seconds(5));

    CHECK(r.reached);
    CHECK_EQ(r.http_status, 200);        // HTTP 仍是 200
    CHECK(r.status.code == Code::unimplemented);
    CHECK(!r.status.message.empty());    // 带上"未注册的方法: ..."
    CHECK(!r.ok());
}

H2MECH_TEST(grpc_server_error_status_without_body)
{
    Fixture f;
    REQUIRE(f.start());

    f.service->add_unary("/test.Echo/Fail",
                         [](h2mech::grpc::ServerCall& call, const Frame&) {
                             // 不发正文 → 走 Trailers-Only
                             h2mech::grpc::Status st;
                             st.code    = Code::not_found;
                             st.message = "no such user";
                             call.finish(st);
                         });

    const auto r = f.client->call("/test.Echo/Fail", "x", {}, std::chrono::seconds(5));

    CHECK(r.reached);
    CHECK(r.status.code == Code::not_found);
    CHECK_EQ(r.status.message, std::string{"no such user"});
    CHECK(r.message.empty());            // 没有正文
}

H2MECH_TEST(grpc_server_metadata_and_timeout_passthrough)
{
    Fixture f;
    REQUIRE(f.start());

    f.service->add_unary("/test.Echo/Meta",
                         [](h2mech::grpc::ServerCall& call, const Frame&) {
                             std::string token;
                             for (const auto& h : call.metadata()) {
                                 if (h.name == "x-token") {
                                     token = h.value;
                                 }
                             }
                             const std::string out = "token=" + token +
                                                     ";timeout=" + call.timeout_header() +
                                                     ";method=" + call.method();
                             call.send(out);
                             call.finish(h2mech::grpc::Status{});
                         });

    const auto r = f.client->call("/test.Echo/Meta", "x", {{"x-token", "abc"}},
                                  std::chrono::milliseconds(1500));

    CHECK(r.ok());
    // 客户端只要给了 timeout，就必须转成 grpc-timeout（毫秒保精度）
    CHECK_EQ(r.message, std::string{"token=abc;timeout=1500m;method=/test.Echo/Meta"});
}

H2MECH_TEST(grpc_server_handler_exception_becomes_internal)
{
    Fixture f;
    REQUIRE(f.start());

    f.service->add_unary("/test.Echo/Boom",
                         [](h2mech::grpc::ServerCall&, const Frame&) {
                             throw std::runtime_error("boom");
                         });

    const auto r = f.client->call("/test.Echo/Boom", "x", {}, std::chrono::seconds(5));

    // 异常绝不能穿透到 io 线程（那会 terminate 整个进程）
    CHECK(r.reached);
    CHECK(r.status.code == Code::internal);
    CHECK(!r.ok());
}

H2MECH_TEST(grpc_server_oversized_request_message)
{
    Fixture f;
    // 单条消息上限压到 64 字节，便于构造"超限"
    REQUIRE(f.start(64));

    f.service->add_unary("/test.Echo/Say",
                         [](h2mech::grpc::ServerCall& call, const Frame& req) {
                             call.send(req.payload);
                             call.finish(h2mech::grpc::Status{});
                         });

    const auto r = f.client->call("/test.Echo/Say", std::string(200, 'x'), {},
                                  std::chrono::seconds(5));

    CHECK(r.reached);
    CHECK(r.status.code == Code::invalid_argument);
    CHECK(!r.ok());
}

#else // H2MECH_HAS_NGHTTP2

H2MECH_TEST(grpc_server_requires_nghttp2)
{
    CHECK(true);
}

#endif
