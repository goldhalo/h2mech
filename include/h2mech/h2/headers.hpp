// =============================================================================
//  h2mech/h2/headers.hpp —— HTTP/1.1 语义 ⇄ HTTP/2 语义的转换层（纯函数）
// =============================================================================
//  ★ 这一层只做"规则"，不碰 IO、不碰 nghttp2、不碰线程。
//    好处：所有 h2 的坑都能用纯单测钉死（不用起 socket、不用证书），
//    而 Http2Session 那部分就只剩状态机与生命周期。
//
//  这里集中处理的差异（RFC 9113 §8.2 / §8.3）：
//    · 伪头（`:method` / `:path` / `:authority` / `:scheme`）必须**全部排在**
//      普通头之前；出现重复或位置错误都是协议错误；
//    · 逐跳头（connection / keep-alive / proxy-connection / transfer-encoding /
//      upgrade / te）在 h2 里没有意义：请求侧 connection/upgrade/
//      proxy-connection 属于**真违规**（说明对端把 h2 当 h1 拼），
//      其余静默丢弃即可；
//    · `:path` 要拆成 url + query（与 h1 的 `util::urlDecode` 对齐）；
//    · `:authority` 要回填成 `host`（h2 里没有 Host 头）；
//    · 响应侧 `content-length` 由本层统一生成（不信业务自填的值），
//      `chunked` 天然不存在（h2 靠 END_STREAM 分帧）。
// =============================================================================
#pragma once

#include "h2mech/export.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace h2mech {
namespace h2 {

/// 一对头字段。`name` 一律是小写（h2 要求）。
struct Header {
    std::string name;
    std::string value;
};

using HeaderList = std::vector<Header>;

// -----------------------------------------------------------------------------
// 基础判定（都是纯函数，单独导出便于上层复用与测试）
// -----------------------------------------------------------------------------

/// ASCII 小写化（只动 A-Z；h2 的头名不允许大写）。
[[nodiscard]] H2MECH_API std::string ascii_lower(std::string_view s);

/// 方法名大写化（`:method` 不保证大写；比较前统一）。
[[nodiscard]] H2MECH_API std::string method_to_upper(std::string_view s);

/// 是否伪头（以 ':' 开头）。
[[nodiscard]] H2MECH_API bool is_pseudo_header(std::string_view name) noexcept;

/// 是否逐跳（hop-by-hop）字段：在 h2 里一律不该出现。
[[nodiscard]] H2MECH_API bool is_hop_by_hop(std::string_view name) noexcept;

/// 响应里必须剔除的头：逐跳 + `host`（谁是 Host 由请求的 :authority 决定）。
[[nodiscard]] H2MECH_API bool is_forbidden_response_header(std::string_view name) noexcept;

/// 请求里出现即构成**协议错误**的头（对端把 h2 当 h1 在拼）。
[[nodiscard]] H2MECH_API bool is_malformed_request_header(std::string_view name) noexcept;

/// 请求里无意义、静默跳过即可的头。
[[nodiscard]] H2MECH_API bool is_ignorable_request_header(std::string_view name) noexcept;

/// 把 `:path` 拆成 url 与 query（query 不含 '?'）。
///
/// @return false = 路径非法（含 '#' 片段、或不是以 '/' 开头且非 "*"）。
[[nodiscard]] H2MECH_API bool split_path(std::string_view path,
                                         std::string&      url,
                                         std::string&      query);

// -----------------------------------------------------------------------------
// 请求头构建
// -----------------------------------------------------------------------------

/// `build_request_head()` 的判定结果。
enum class HeadersStatus {
    ok = 0,
    pseudo_after_regular,      ///< 伪头出现在普通头之后（RFC 9113 §8.3 违规）
    duplicate_pseudo_header,   ///< 同一个伪头出现两次
    unknown_pseudo_header,     ///< 不认识的伪头（含 :protocol，暂不支持）
    missing_method,            ///< 缺 :method
    missing_path,              ///< 缺 :path（含 CONNECT：本库暂不支持）
    invalid_path,              ///< :path 不是 origin-form / "*"（见 split_path）
    malformed_header,          ///< 命中 is_malformed_request_header
    invalid_content_length,    ///< content-length 不是十进制数字或溢出
    duplicate_content_length,  ///< 一对请求里出现多个 content-length
    too_many_headers,          ///< 头部数量超过上限
};

/// 状态码对应的可读文案（日志 / 400 响应体用）。
[[nodiscard]] H2MECH_API const char* to_string(HeadersStatus s) noexcept;

/// 解析后的请求头（尚未带 body）。
struct RequestHead {
    std::string method;      ///< 大写方法名
    std::string path;        ///< :path 原样（日志用）
    std::string url;         ///< :path 去掉 ?query 的部分
    std::string query;       ///< 不含 '?'（无 query 时为空）
    std::string authority;   ///< :authority（可能为空）
    std::string scheme;      ///< :scheme（可能为空；h2c 通常是 http）
    HeaderList  headers;     ///< 普通头（小写名；已回填 host）
    std::uint64_t content_length = 0;
    bool          has_content_length = false;
};

struct RequestHeadResult {
    HeadersStatus status = HeadersStatus::ok;
    RequestHead   head;

    [[nodiscard]] bool ok() const noexcept { return status == HeadersStatus::ok; }
};

/// 把 nghttp2 给的原始头列表（含伪头）转成 `RequestHead`。
///
/// @param[in] raw         原始头列表（顺序即线上顺序）。
/// @param[in] max_headers 头部数量上限（防头部炸弹；<=0 表示用默认 64）。
RequestHeadResult build_request_head(const HeaderList& raw, std::size_t max_headers = 64);

// -----------------------------------------------------------------------------
// 响应头构建
// -----------------------------------------------------------------------------

/// 业务想发的响应（语义层）。
struct ResponseHead {
    int        status  = 200;
    HeaderList headers;   ///< 业务给的头（可能含非法项，本层负责剔除）
};

/// 生成给 nghttp2 的头列表。
///
/// 规则：
///   · `:status` **必须第一个**（RFC 9113 §8.3）；
///   · `content-length` 由本层统一生成（`content_length` 参数），
///     **忽略业务自填的那份** —— 业务写的值常与实际正文不一致；
///   · 逐跳头、`host`、伪头、`content-length` 一律剔除；
///   · `include_content_length = false` 用于"长度未知的流式正文"：
///     宁可不发，也不发一个错的（对端会按错误边界解析，比不发严重）。
[[nodiscard]] H2MECH_API HeaderList build_response_headers(const ResponseHead& head,
                                                           std::uint64_t content_length,
                                                           bool include_content_length = true);

} // namespace h2
} // namespace h2mech
