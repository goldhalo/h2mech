// =============================================================================
//  h2mech - 公共类型与错误码
// =============================================================================
//  ★ 本头**不引 asio / OpenSSL / nghttp2**：公共依赖只有 C++ 标准库。
//    与 netmech / httpmech 的约定一致 —— 装出去的包不该把传输层或协议引擎
//    的头文件塞给使用者。
//
//  ★ 错误码分三段，前缀即可判断来源：
//      · 库本身（not_started / invalid_argument …）
//      · HTTP/2 协议（RFC 9113 §7 的那批错误码 + 本地判定）
//      · gRPC（wire 层的帧/状态）
//    为什么把 h2 的协议错误码也做成 error_code：对端发来 RST_STREAM /
//    GOAWAY 时业务需要"按码分流"（重试 / 放弃 / 降级），用一个枚举比让业务
//    自己解析字符串可靠得多。
// =============================================================================
#pragma once

#include "h2mech/export.hpp"

#include <cstdint>
#include <string>
#include <system_error>

namespace h2mech {

/// 错误码。
///
/// @note 数值不对外承诺稳定（不要序列化）；用枚举值比较即可。
enum class Errc {
    ok = 0,

    // ---------------------------------------------------------------------
    // 库本身
    // ---------------------------------------------------------------------
    invalid_argument,      ///< 参数非法（空 handler、缺必需字段…）
    not_started,           ///< Server/Client 尚未 start()
    already_started,       ///< 重复 start()
    not_connected,         ///< 连接尚未就绪（握手未完成）
    link_closed,           ///< 连接已关闭
    unsupported,           ///< 本构建不支持（例如未编 TLS 却要求 h2 over TLS）
    buffer_full,           ///< 发送队列满（反压，**不是错误**，应保留 pending 后重试）
    timeout,               ///< 握手 / 请求超时

    // ---------------------------------------------------------------------
    // HTTP/2（RFC 9113 §7：错误码与线上 RST_STREAM 的值一一对应）
    // ---------------------------------------------------------------------
    protocol_error,          ///< PROTOCOL_ERROR
    internal_error,          ///< INTERNAL_ERROR
    flow_control_error,      ///< FLOW_CONTROL_ERROR
    settings_timeout,        ///< SETTINGS_TIMEOUT
    stream_closed,           ///< STREAM_CLOSED
    frame_size_error,        ///< FRAME_SIZE_ERROR
    refused_stream,          ///< REFUSED_STREAM（对端拒绝，可安全重试）
    cancel,                  ///< CANCEL（对端主动取消，别重试）
    compression_error,       ///< COMPRESSION_ERROR（HPACK 出错，连接不可恢复）
    connect_error,           ///< CONNECT_ERROR
    enhance_your_calm,       ///< ENHANCE_YOUR_CALM
    inadequate_security,     ///< INADEQUATE_SECURITY
    http_1_1_required,       ///< HTTP_1_1_REQUIRED

    // 本地判定（不对应线上错误码）
    goaway,                  ///< 对端发过 GOAWAY：这条连接不再能开新流
    stream_limit_exceeded,   ///< 并发流超过本地上限
    header_list_too_large,   ///< 头部列表超过上限（HPACK 解压后）

    // ---------------------------------------------------------------------
    // gRPC（wire 层）
    // ---------------------------------------------------------------------
    grpc_status_nonzero,         ///< 对端回了非 0 的 grpc-status（细节在 message 里）
    grpc_invalid_frame,          ///< 5 字节前缀/长度与实际数据不符
    grpc_unsupported_encoding,   ///< grpc-encoding 不是 identity（暂不支持解压）
};

/// 错误码类别（`std::error_code::category()`）。
[[nodiscard]] H2MECH_API const std::error_category& error_category() noexcept;

/// `Errc` → `std::error_code`（已特化 `std::is_error_code_enum`，可直接隐式转换）。
[[nodiscard]] H2MECH_API std::error_code make_error_code(Errc e) noexcept;

/// 该 `std::error_code` 是否属于本库类别。
///
/// @return true = 由 `h2mech::Errc` 产生；false = 别处来的（例如 netmech 的
///         `NetErrc`，跨库透传时先用它分流，再决定怎么映射）。
[[nodiscard]] H2MECH_API bool is_h2mech_error(const std::error_code& ec) noexcept;

} // namespace h2mech

// 让 `std::error_code ec = h2mech::Errc::cancel;` 能编译。
namespace std {
template <>
struct is_error_code_enum<h2mech::Errc> : true_type {};
} // namespace std
