// =============================================================================
//  gRPC unary 客户端 —— 对本仓的 h2 服务端跑真回环（h2c）
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/grpc/client.hpp>
#include <h2mech/grpc/framer.hpp>
#include <h2mech/h2/server.hpp>

#if defined(H2MECH_HAS_NGHTTP2)

#include <memory>
#include <string>

namespace {

using h2mech::grpc::Code;

/// 极简 gRPC 服务：/test.Echo/Say 回显，/test.Echo/Fail 返回 NOT_FOUND。
struct EchoService final : public h2mech::h2::Handler {
    void on_request(h2mech::h2::Request& req, h2mech::h2::Responder& out) override
    {
        ++calls;
        for (const auto& h : req.headers) {
            if (h.name == "content-type") {
                content_type = h.value;
            }
        }

        auto respond_error = [&out](const char* status, const char* msg) {
            // Trailers-Only：没有正文时 grpc-status 直接放在响应头里
            out.send_headers(200, {{"content-type", "application/grpc"},
                                   {"grpc-status", status},
                                   {"grpc-message", msg}});
            out.finish();
        };

        if (req.url == "/test.Echo/Fail") {
            respond_error("5", "no such thing");
            return;
        }

        // 解一帧请求消息（服务端同样用 wire 层的解析器）
        h2mech::grpc::MessageReader reader;
        h2mech::grpc::Frame        f;
        if (!reader.feed(req.body) || !reader.next(f)) {
            respond_error("13", "bad request frame");
            return;
        }

        out.send_headers(200, {{"content-type", "application/grpc"}});
        out.send_data(h2mech::grpc::encode_message(f.payload));
        out.finish({{"grpc-status", "0"}});
    }

    int         calls = 0;
    std::string content_type;
};

} // namespace

H2MECH_TEST(grpc_client_unary_roundtrip)
{
    auto service = std::make_shared<EchoService>();

    h2mech::h2::Server::Config scfg;
    scfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", srv->local().port()};

    auto cli = h2mech::grpc::Client::create(ccfg);
    REQUIRE(cli != nullptr);

    const auto r = cli->call("/test.Echo/Say", "hello-grpc", {}, std::chrono::seconds(5));

    CHECK_MSG(r.ok(), std::string{"调用失败: "} + h2mech::grpc::code_name(r.status.code));
    CHECK(r.reached);
    CHECK_EQ(static_cast<int>(r.status.code), 0);
    CHECK_EQ(r.message, std::string{"hello-grpc"});
    CHECK_EQ(r.http_status, 200);
    CHECK_EQ(service->calls, 1);

    // 客户端必须按 gRPC 的约定发头，否则真实的服务端会拒绝
    CHECK_EQ(service->content_type, std::string{"application/grpc"});

    cli->close();
    srv->stop();
}

H2MECH_TEST(grpc_client_error_status_is_reported)
{
    auto service = std::make_shared<EchoService>();

    h2mech::h2::Server::Config scfg;
    scfg.local = {"127.0.0.1", 0};

    auto srv = h2mech::h2::Server::create(scfg, service);
    REQUIRE(srv != nullptr);
    REQUIRE(!srv->start());

    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote = {"127.0.0.1", srv->local().port()};

    auto cli = h2mech::grpc::Client::create(ccfg);
    REQUIRE(cli != nullptr);

    const auto r = cli->call("/test.Echo/Fail", "x", {}, std::chrono::seconds(5));

    // 关键：HTTP 是 200，但调用**失败** —— 这就是"把 200 当成功"会踩的坑
    CHECK(r.reached);
    CHECK_EQ(r.http_status, 200);
    CHECK(!r.ok());
    CHECK(r.status.code == Code::not_found);
    CHECK_EQ(r.status.message, std::string{"no such thing"});

    cli->close();
    srv->stop();
}

H2MECH_TEST(grpc_client_connect_failure_is_unavailable)
{
    // 127.0.0.1:1 基本可以确定没有监听 —— 用来验证"连不上"的归类。
    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote           = {"127.0.0.1", 1};
    ccfg.connect_timeout  = std::chrono::milliseconds(500);
    ccfg.default_timeout  = std::chrono::milliseconds(1000);

    auto cli = h2mech::grpc::Client::create(ccfg);
    REQUIRE(cli != nullptr);

    const auto r = cli->call("/test.Echo/Say", "x", {}, std::chrono::milliseconds(1500));

    CHECK(!r.reached);
    CHECK(!r.ok());
    CHECK(r.status.code == Code::unavailable);
    CHECK(!r.status.message.empty());
}

#else // H2MECH_HAS_NGHTTP2

H2MECH_TEST(grpc_client_requires_nghttp2)
{
    CHECK(true);
}

#endif
