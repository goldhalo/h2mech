// =============================================================================
//  h1 ⇄ h2 头字段语义转换（纯函数，无需 socket）
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/h2/headers.hpp>

#include <initializer_list>
#include <string>
#include <utility>

namespace {

using h2mech::h2::Header;
using h2mech::h2::HeaderList;
using h2mech::h2::HeadersStatus;

HeaderList nv(std::initializer_list<std::pair<const char*, const char*>> xs)
{
    HeaderList out;
    out.reserve(xs.size());
    for (const auto& kv : xs) {
        out.push_back(Header{kv.first, kv.second});
    }
    return out;
}

/// 列表里是否有该名字（调用方传小写）。
bool has(const HeaderList& list, const char* name)
{
    for (const auto& h : list) {
        if (h.name == name) {
            return true;
        }
    }
    return false;
}

const char* value_of(const HeaderList& list, const char* name)
{
    for (const auto& h : list) {
        if (h.name == name) {
            return h.value.c_str();
        }
    }
    return "";
}

} // namespace

// -----------------------------------------------------------------------------
// 正常请求
// -----------------------------------------------------------------------------

H2MECH_TEST(headers_request_basics)
{
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "post"},                  // 小写方法名也要能认
        {":path", "/api/echo?a=1&b=2"},
        {":scheme", "https"},
        {":authority", "device.local:8443"},
        {"content-type", "application/json"},
        {"content-length", "17"},
    }));

    REQUIRE(r.ok());
    CHECK_EQ(r.head.method, std::string{"POST"});
    CHECK_EQ(r.head.url, std::string{"/api/echo"});
    CHECK_EQ(r.head.query, std::string{"a=1&b=2"});
    CHECK_EQ(r.head.authority, std::string{"device.local:8443"});
    CHECK_EQ(r.head.scheme, std::string{"https"});
    CHECK(r.head.has_content_length);
    CHECK_EQ(r.head.content_length, std::uint64_t{17});

    // :authority 回填 host，且排在普通头前面（h1 侧习惯按序读）
    REQUIRE(!r.head.headers.empty());
    CHECK_EQ(r.head.headers.front().name, std::string{"host"});
    CHECK_EQ(r.head.headers.front().value, std::string{"device.local:8443"});
    CHECK(has(r.head.headers, "content-type"));
    CHECK(has(r.head.headers, "content-length"));
}

H2MECH_TEST(headers_host_from_raw_wins)
{
    // 原始头里已经有 host 时不要被 :authority 覆盖（对端可能故意给不同的值，
    // 覆盖会让"看到的 Host"和"路由用的 :authority"不一致，更难排查）。
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "GET"},
        {":path", "/"},
        {":authority", "a.local"},
        {"host", "b.local"},
    }));
    REQUIRE(r.ok());
    CHECK_EQ(value_of(r.head.headers, "host"), std::string{"b.local"});
}

H2MECH_TEST(headers_no_query)
{
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "GET"},
        {":path", "/health"},
        {":authority", "x"},
    }));
    REQUIRE(r.ok());
    CHECK_EQ(r.head.url, std::string{"/health"});
    CHECK(r.head.query.empty());
}

H2MECH_TEST(headers_names_are_lowercased)
{
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "GET"},
        {":path", "/"},
        {"Content-Type", "text/plain"},   // 对端给大写（h1 转换来的场景）
    }));
    REQUIRE(r.ok());
    CHECK(has(r.head.headers, "content-type"));
    CHECK(!has(r.head.headers, "Content-Type"));
}

// -----------------------------------------------------------------------------
// 协议错误
// -----------------------------------------------------------------------------

H2MECH_TEST(headers_pseudo_after_regular_rejected)
{
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "GET"},
        {"x-a", "1"},
        {":path", "/"},                   // 伪头出现在普通头之后 → 违规
    }));
    CHECK(r.status == HeadersStatus::pseudo_after_regular);
}

H2MECH_TEST(headers_duplicate_pseudo_rejected)
{
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "GET"},
        {":method", "POST"},
        {":path", "/"},
    }));
    CHECK(r.status == HeadersStatus::duplicate_pseudo_header);
}

H2MECH_TEST(headers_missing_required_pseudo)
{
    {
        const auto r = h2mech::h2::build_request_head(nv({{":path", "/"}}));
        CHECK(r.status == HeadersStatus::missing_method);
    }
    {
        // CONNECT 的 :path 为空 —— 本库暂不支持，按缺 :path 拒绝
        const auto r = h2mech::h2::build_request_head(nv({{":method", "CONNECT"}}));
        CHECK(r.status == HeadersStatus::missing_path);
    }
}

H2MECH_TEST(headers_unknown_pseudo_rejected)
{
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "GET"},
        {":path", "/"},
        {":protocol", "websocket"},       // extended CONNECT，暂不支持
    }));
    CHECK(r.status == HeadersStatus::unknown_pseudo_header);
}

H2MECH_TEST(headers_malformed_request_header_rejected)
{
    // connection / upgrade / proxy-connection 是真违规（对端把 h2 当 h1 拼）
    CHECK(h2mech::h2::build_request_head(nv({
              {":method", "GET"}, {":path", "/"}, {"connection", "keep-alive"}}))
              .status == HeadersStatus::malformed_header);
    CHECK(h2mech::h2::build_request_head(nv({
              {":method", "GET"}, {":path", "/"}, {"upgrade", "h2c"}}))
              .status == HeadersStatus::malformed_header);
    CHECK(h2mech::h2::build_request_head(nv({
              {":method", "GET"}, {":path", "/"}, {"proxy-connection", "x"}}))
              .status == HeadersStatus::malformed_header);
}

H2MECH_TEST(headers_ignorable_headers_skipped)
{
    // keep-alive / transfer-encoding / te 只是无意义：跳过，不算错
    const auto r = h2mech::h2::build_request_head(nv({
        {":method", "POST"},
        {":path", "/x"},
        {"keep-alive", "timeout=5"},
        {"transfer-encoding", "chunked"},
        {"te", "trailers"},
        {"content-type", "application/grpc"},
    }));
    REQUIRE(r.ok());
    CHECK(!has(r.head.headers, "keep-alive"));
    CHECK(!has(r.head.headers, "transfer-encoding"));
    CHECK(!has(r.head.headers, "te"));
    CHECK(has(r.head.headers, "content-type"));
}

H2MECH_TEST(headers_path_forms)
{
    std::string url;
    std::string query;

    CHECK(h2mech::h2::split_path("/a/b?x=1", url, query));
    CHECK_EQ(url, std::string{"/a/b"});
    CHECK_EQ(query, std::string{"x=1"});

    CHECK(h2mech::h2::split_path("*", url, query));   // OPTIONS *
    CHECK_EQ(url, std::string{"*"});

    CHECK(h2mech::h2::split_path("http://h/x?y", url, query));   // absolute-form
    CHECK_EQ(url, std::string{"http://h/x"});

    CHECK(!h2mech::h2::split_path("x/y", url, query));      // 不是 origin-form
    CHECK(!h2mech::h2::split_path("/x#frag", url, query));  // 不该带片段
    CHECK(!h2mech::h2::split_path("", url, query));
}

H2MECH_TEST(headers_content_length_validation)
{
    {
        const auto r = h2mech::h2::build_request_head(nv({
            {":method", "POST"}, {":path", "/"}, {"content-length", "abc"}}));
        CHECK(r.status == HeadersStatus::invalid_content_length);
    }
    {
        // 溢出：20 个 9 超过 uint64
        const auto r = h2mech::h2::build_request_head(nv({
            {":method", "POST"}, {":path", "/"},
            {"content-length", "99999999999999999999"}}));
        CHECK(r.status == HeadersStatus::invalid_content_length);
    }
    {
        const auto r = h2mech::h2::build_request_head(nv({
            {":method", "POST"}, {":path", "/"},
            {"content-length", "1"}, {"content-length", "2"}}));
        CHECK(r.status == HeadersStatus::duplicate_content_length);
    }
}

H2MECH_TEST(headers_too_many_rejected)
{
    HeaderList raw = nv({{":method", "GET"}, {":path", "/"}});
    for (int i = 0; i < 5; ++i) {
        raw.push_back(Header{"x-" + std::to_string(i), "v"});
    }
    // 上限 4：一共 7 项 → 直接拒（防头部炸弹）
    const auto r = h2mech::h2::build_request_head(raw, 4);
    CHECK(r.status == HeadersStatus::too_many_headers);
}

// -----------------------------------------------------------------------------
// 响应
// -----------------------------------------------------------------------------

H2MECH_TEST(headers_response_rules)
{
    h2mech::h2::ResponseHead head;
    head.status = 204;
    head.headers = nv({
        {"Content-Type", "text/plain"},
        {"connection", "close"},          // 逐跳 → 丢
        {"keep-alive", "timeout=5"},      // 逐跳 → 丢
        {"transfer-encoding", "chunked"},  // 逐跳 → 丢（业务可能从 h1 逻辑带过来）
        {"host", "wrong.local"},          // 响应里没有意义 → 丢
        {":status", "200"},               // 业务塞的伪头 → 丢
        {"content-length", "999"},        // 业务自填 → 丢，用参数重新生成
        {"x-trace", "abc"},
    });

    const HeaderList out = h2mech::h2::build_response_headers(head, 0, true);

    REQUIRE(out.size() >= 2);
    // :status 必须第一个，且用 head.status（不是业务塞的那个）
    CHECK_EQ(out.front().name, std::string{":status"});
    CHECK_EQ(out.front().value, std::string{"204"});

    CHECK(has(out, "content-length"));
    CHECK_EQ(value_of(out, "content-length"), std::string{"0"});

    CHECK(has(out, "content-type"));
    CHECK(has(out, "x-trace"));

    CHECK(!has(out, "connection"));
    CHECK(!has(out, "keep-alive"));
    CHECK(!has(out, "transfer-encoding"));
    CHECK(!has(out, "host"));

    // ":status" 只能有一个
    int pseudo_count = 0;
    for (const auto& h : out) {
        if (h.name == ":status") {
            ++pseudo_count;
        }
    }
    CHECK_EQ(pseudo_count, 1);
}

H2MECH_TEST(headers_response_without_content_length)
{
    // 长度未知的流式正文：宁可不发 content-length，也不发一个错的
    h2mech::h2::ResponseHead head;
    head.status  = 200;
    head.headers = nv({{"content-type", "application/octet-stream"}});

    const HeaderList out = h2mech::h2::build_response_headers(head, 0, false);
    CHECK(!has(out, "content-length"));
    CHECK_EQ(out.front().value, std::string{"200"});
}

H2MECH_TEST(headers_predicates_case_insensitive)
{
    using namespace h2mech::h2;
    CHECK(is_hop_by_hop("Connection"));
    CHECK(is_hop_by_hop("TE"));
    CHECK(!is_hop_by_hop("content-type"));

    CHECK(is_forbidden_response_header("Host"));
    CHECK(is_malformed_request_header("Upgrade"));
    CHECK(is_ignorable_request_header("Keep-Alive"));
    CHECK(is_pseudo_header(":path"));
    CHECK(!is_pseudo_header("path"));

    CHECK_EQ(ascii_lower("AbC-1"), std::string{"abc-1"});
    CHECK_EQ(method_to_upper("get"), std::string{"GET"});
}
