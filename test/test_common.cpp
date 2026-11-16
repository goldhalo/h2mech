// =============================================================================
//  错误码与公共类型
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/h2mech.hpp>

#include <string>
#include <system_error>

H2MECH_TEST(error_code_roundtrip)
{
    // is_error_code_enum 特化后，下面这行必须能隐式转换 —— 这是使用者最常写的形态
    const std::error_code ec = h2mech::Errc::cancel;
    CHECK_EQ(ec.value(), static_cast<int>(h2mech::Errc::cancel));
    CHECK_EQ(std::string{ec.category().name()}, std::string{"h2mech"});
    CHECK(!ec.message().empty());
    CHECK(h2mech::is_h2mech_error(ec));

    // 相等性按 (value, category) 判定，不能退化成指针比较
    CHECK(ec == h2mech::make_error_code(h2mech::Errc::cancel));
    CHECK(ec != h2mech::make_error_code(h2mech::Errc::refused_stream));

    // ok 是合法值（0），与"没有 error_code"是同一语义
    CHECK_EQ(h2mech::make_error_code(h2mech::Errc::ok).value(), 0);
}

H2MECH_TEST(error_messages_readable)
{
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

H2MECH_TEST(foreign_error_not_misclassified)
{
    // 跨库透传时（netmech 的 NetErrc 或 std 的 errc）必须能区分开，
    // 否则上层会把"对端关闭"当成"HPACK 出错"去处理。
    const std::error_code foreign = std::make_error_code(std::errc::timed_out);
    CHECK(!h2mech::is_h2mech_error(foreign));
    CHECK(foreign != h2mech::make_error_code(h2mech::Errc::timeout));
}
