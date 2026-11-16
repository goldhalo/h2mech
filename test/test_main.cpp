// =============================================================================
//  h2mech 单元测试（自带极简断言框架，不引第三方）
// =============================================================================
//  与 netmech / httpmech 的测试风格保持一致：一个 main + 一张 cases 表，
//  失败只打印第一现场（文件:行 + 表达式），不依赖 gtest/Catch。
// =============================================================================
#include <h2mech/h2mech.hpp>

#include <cstdio>
#include <string>
#include <system_error>

namespace {

int         g_checks = 0;
int         g_fails  = 0;
const char* g_case   = "";

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_fails;                                                           \
            std::printf("    FAIL [%s] %s:%d  %s\n", g_case, __FILE__, __LINE__, \
                        #expr);                                                  \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!((a) == (b))) {                                                     \
            ++g_fails;                                                           \
            std::printf("    FAIL [%s] %s:%d  %s != %s\n", g_case, __FILE__,     \
                        __LINE__, #a, #b);                                       \
        }                                                                        \
    } while (0)

#define REQUIRE(expr)                                                            \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_fails;                                                           \
            std::printf("    FAIL [%s] %s:%d  REQUIRE %s\n", g_case, __FILE__,   \
                        __LINE__, #expr);                                        \
            return;                                                              \
        }                                                                        \
    } while (0)

struct Case {
    const char* name;
    void (*fn)();
};

// =============================================================================
//  用例
// =============================================================================

void test_error_code_roundtrip()
{
    g_case = "error code roundtrip";

    // is_error_code_enum 特化后，下面这行必须能隐式转换 —— 这是使用者最常写的形态
    const std::error_code ec = h2mech::Errc::cancel;
    CHECK_EQ(ec.value(), static_cast<int>(h2mech::Errc::cancel));
    CHECK_EQ(std::string{ec.category().name()}, std::string{"h2mech"});
    CHECK(!ec.message().empty());
    CHECK(h2mech::is_h2mech_error(ec));

    // 相等性按 (value, category) 判定，不能退化成指针比较
    CHECK(ec == h2mech::make_error_code(h2mech::Errc::cancel));
    CHECK(ec != h2mech::make_error_code(h2mech::Errc::refused_stream));

    // ok 也是合法值（0），不能因为"0 为假"而被误当成"没出错"以外的东西
    CHECK_EQ(h2mech::make_error_code(h2mech::Errc::ok).value(), 0);
}

void test_error_messages_are_readable()
{
    g_case = "error messages readable";

    // 每个码都要有可读文案：否则日志里会出现 "error: " 这种没有信息量的空白。
    const h2mech::Errc all[] = {
        h2mech::Errc::invalid_argument,
        h2mech::Errc::not_started,
        h2mech::Errc::already_started,
        h2mech::Errc::not_connected,
        h2mech::Errc::link_closed,
        h2mech::Errc::unsupported,
        h2mech::Errc::buffer_full,
        h2mech::Errc::timeout,
        h2mech::Errc::protocol_error,
        h2mech::Errc::internal_error,
        h2mech::Errc::flow_control_error,
        h2mech::Errc::settings_timeout,
        h2mech::Errc::stream_closed,
        h2mech::Errc::frame_size_error,
        h2mech::Errc::refused_stream,
        h2mech::Errc::cancel,
        h2mech::Errc::compression_error,
        h2mech::Errc::connect_error,
        h2mech::Errc::enhance_your_calm,
        h2mech::Errc::inadequate_security,
        h2mech::Errc::http_1_1_required,
        h2mech::Errc::goaway,
        h2mech::Errc::stream_limit_exceeded,
        h2mech::Errc::header_list_too_large,
        h2mech::Errc::grpc_status_nonzero,
        h2mech::Errc::grpc_invalid_frame,
        h2mech::Errc::grpc_unsupported_encoding,
    };

    for (const h2mech::Errc e : all) {
        const std::string msg = h2mech::make_error_code(e).message();
        CHECK(!msg.empty());
        CHECK(msg != "unknown h2mech error");
    }
}

void test_foreign_error_not_misclassified()
{
    g_case = "foreign error not misclassified";

    // 跨库透传时（netmech 的 NetErrc 或 std 的 errc）必须能区分开，
    // 否则上层会把"对端关闭"当成"HPACK 出错"去处理。
    const std::error_code foreign = std::make_error_code(std::errc::timed_out);
    CHECK(!h2mech::is_h2mech_error(foreign));
    CHECK(foreign != h2mech::make_error_code(h2mech::Errc::timeout));
}

} // namespace

int main()
{
    const Case cases[] = {
        {"error code roundtrip",        test_error_code_roundtrip},
        {"error messages readable",     test_error_messages_are_readable},
        {"foreign error not misclassified", test_foreign_error_not_misclassified},
    };

    for (const Case& c : cases) {
        const int before = g_fails;
        std::printf("  [%s]\n", c.name);
        c.fn();
        std::printf("    %s\n", g_fails == before ? "ok" : "FAILED");
    }

    std::printf("\n断言: %d 通过 / %d 失败 / %d 总计\n",
                g_checks - g_fails, g_fails, g_checks);
    std::printf("结果: %s\n", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0 ? 0 : 1;
}
