// =============================================================================
//  h2mech/grpc/status.hpp —— gRPC 状态、超时与 content-type 约定
// =============================================================================
//  ★ gRPC 的"调用结果"不在 HTTP 状态码里，而在 **trailers** 的
//    `grpc-status`（数字）+ `grpc-message`（可选文本）里。
//    HTTP 状态永远是 200（除了代理层故障）—— 这一点最容易写错：
//    把 HTTP 200 当成"调用成功"，就会把 UNAUTHENTICATED 当成成功。
//
//  ★ Trailers-Only：服务端在**没有正文**时，直接把 grpc-status 放进响应头
//    并结束流（不单独发 trailers）。所以解析时要把"响应头"和"trailers"
//    合起来看。
// =============================================================================
#pragma once

#include "h2mech/export.hpp"
#include "h2mech/h2/headers.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace h2mech {
namespace grpc {

/// gRPC 官方状态码（与 google.rpc.Code 一致，0..16）。
enum class Code : std::int32_t {
    ok                  = 0,
    cancelled           = 1,
    unknown             = 2,
    invalid_argument    = 3,
    deadline_exceeded   = 4,
    not_found           = 5,
    already_exists      = 6,
    permission_denied   = 7,
    resource_exhausted  = 8,
    failed_precondition = 9,
    aborted             = 10,
    out_of_range        = 11,
    unimplemented       = 12,
    internal            = 13,
    unavailable         = 14,
    data_loss           = 15,
    unauthenticated     = 16,
};

/// 状态码的规范名（`OK` / `NOT_FOUND` …），用于日志。
[[nodiscard]] H2MECH_API const char* code_name(Code c) noexcept;

/// 解析十进制状态码（"0".."16"；越界或非数字返回 false）。
[[nodiscard]] H2MECH_API bool parse_code(std::string_view text, Code& out) noexcept;

/// 一次调用的最终状态。
struct Status {
    Code        code = Code::ok;
    std::string message;   ///< grpc-message（可能为空；不是给机器解析的）

    [[nodiscard]] bool ok() const noexcept { return code == Code::ok; }
};

/// 从响应头 **或** trailers 里解析状态。
///
/// @param[in] headers 响应头（Trailers-Only 形态时 status 就在这里）
/// @param[in] trailers 真正的 trailers（普通形态）
///
/// @par 优先级
/// 先看 trailers（更权威），没有再退回响应头；两处都没有 → 返回
/// `unknown`（**不是** ok：缺 grpc-status 说明对端没按 gRPC 说话，
/// 当成成功会把错误吞掉）。
[[nodiscard]] H2MECH_API Status parse_status(const h2::HeaderList& headers,
                                             const h2::HeaderList& trailers);

/// 生成 `grpc-timeout` 头的值（最大值 8 位十进制 + 1 位单位）。
///
/// 单位按精度选：>= 1 天用 H，>= 1 小时用 H/M，毫秒/微秒/纳秒各取最精确的那个。
/// @return 空串 = 不设超时（ms <= 0）
[[nodiscard]] H2MECH_API std::string encode_timeout(std::chrono::milliseconds ms);

/// 该 content-type 是否是 gRPC（允许 `application/grpc+proto` 等后缀）。
[[nodiscard]] H2MECH_API bool is_grpc_content_type(std::string_view value) noexcept;

/// gRPC 要求请求头里带的 `te: trailers` —— 少了它，某些服务端会拒绝。
[[nodiscard]] H2MECH_API bool needs_te_trailers(const h2::HeaderList& headers) noexcept;

} // namespace grpc
} // namespace h2mech
