// =============================================================================
//  h2mech - h1 ⇄ h2 头字段语义转换（纯函数实现）
// =============================================================================
#include "h2mech/h2/headers.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace h2mech {
namespace h2 {
namespace {

constexpr std::size_t kDefaultMaxHeaders = 64;

/// 大小写不敏感比较（只对 ASCII 生效，头名本来就是 ASCII）。
[[nodiscard]] bool equals_ci(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - ('A' - 'a'));
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - ('A' - 'a'));
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/// 十进制 uint64 解析：不接受空串、不接受非数字、溢出返回 false。
[[nodiscard]] bool parse_u64(std::string_view s, std::uint64_t& out) noexcept
{
    if (s.empty()) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (v > (UINT64_MAX - digit) / 10) {
            return false;   // 溢出
        }
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

/// 该名字是否已在列表里（小写比较）。
[[nodiscard]] bool has_header(const HeaderList& list, std::string_view name)
{
    for (const Header& h : list) {
        if (equals_ci(h.name, name)) {
            return true;
        }
    }
    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// 基础判定
// -----------------------------------------------------------------------------

std::string ascii_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - ('A' - 'a')) : ch);
    }
    return out;
}

std::string method_to_upper(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        out.push_back((ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - ('a' - 'A')) : ch);
    }
    return out;
}

bool is_pseudo_header(std::string_view name) noexcept
{
    return !name.empty() && name.front() == ':';
}

bool is_hop_by_hop(std::string_view name) noexcept
{
    return equals_ci(name, "connection") || equals_ci(name, "keep-alive")
           || equals_ci(name, "proxy-connection") || equals_ci(name, "transfer-encoding")
           || equals_ci(name, "upgrade") || equals_ci(name, "te");
}

bool is_forbidden_response_header(std::string_view name) noexcept
{
    // host 也剔除：响应里"谁是 Host"由请求的 :authority 决定，回显没有意义。
    return is_hop_by_hop(name) || equals_ci(name, "host");
}

bool is_malformed_request_header(std::string_view name) noexcept
{
    // 这三项是**真违规**：客户端在 h2 上发它们，说明把 h2 当 h1 在拼报文，
    // 后半段大概率也不对。按流错误处理比硬着头皮服务更安全。
    return equals_ci(name, "connection") || equals_ci(name, "upgrade")
           || equals_ci(name, "proxy-connection");
}

bool is_ignorable_request_header(std::string_view name) noexcept
{
    // 这些只是"无意义"：h2 天然分帧，没有 keep-alive / chunked 的概念；
    // `te` 在 h2 里唯一合法值是 trailers，而我们不消费它，跳过即可。
    return equals_ci(name, "keep-alive") || equals_ci(name, "transfer-encoding")
           || equals_ci(name, "te");
}

bool split_path(std::string_view path, std::string& url, std::string& query)
{
    url.clear();
    query.clear();
    if (path.empty()) {
        return false;
    }

    // RFC 9113 §8.3.1：:path 允许 origin-form（/x?y）、absolute-form
    // （http://h/x）以及 OPTIONS 的 "*"。这里只做**形状**校验，不做规范化。
    const bool ok_shape = path.front() == '/' || path == "*"
                          || path.substr(0, 7) == "http://"
                          || path.substr(0, 8) == "https://";
    if (!ok_shape) {
        return false;
    }
    if (path.find('#') != std::string_view::npos) {
        return false;   // 片段标识符不该出现在请求目标里
    }

    const std::size_t q = path.find('?');
    if (q == std::string_view::npos) {
        url.assign(path);
        return true;
    }
    url.assign(path.substr(0, q));
    query.assign(path.substr(q + 1));
    return true;
}

const char* to_string(HeadersStatus s) noexcept
{
    switch (s) {
    case HeadersStatus::ok:                      return "ok";
    case HeadersStatus::pseudo_after_regular:    return "伪头出现在普通头之后";
    case HeadersStatus::duplicate_pseudo_header: return "伪头重复";
    case HeadersStatus::unknown_pseudo_header:   return "不认识的伪头";
    case HeadersStatus::missing_method:          return "缺少 :method";
    case HeadersStatus::missing_path:            return "缺少 :path";
    case HeadersStatus::invalid_path:            return ":path 形状非法";
    case HeadersStatus::malformed_header:        return "出现 h2 禁止的请求头";
    case HeadersStatus::invalid_content_length:  return "content-length 非数字或溢出";
    case HeadersStatus::duplicate_content_length:return "content-length 重复";
    case HeadersStatus::too_many_headers:        return "头部数量超过上限";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
// 请求
// -----------------------------------------------------------------------------

RequestHeadResult build_request_head(const HeaderList& raw, std::size_t max_headers)
{
    RequestHeadResult out;
    const std::size_t limit = (max_headers == 0) ? kDefaultMaxHeaders : max_headers;

    if (raw.size() > limit) {
        out.status = HeadersStatus::too_many_headers;
        return out;
    }

    RequestHead& head      = out.head;
    bool        saw_regular = false;
    bool        saw_method = false;
    bool        saw_path   = false;
    bool        saw_authority = false;
    bool        saw_scheme = false;

    for (const Header& h : raw) {
        const std::string name = ascii_lower(h.name);
        if (name.empty()) {
            out.status = HeadersStatus::malformed_header;
            return out;
        }

        if (is_pseudo_header(name)) {
            if (saw_regular) {
                out.status = HeadersStatus::pseudo_after_regular;
                return out;
            }
            if (name == ":method") {
                if (saw_method) {
                    out.status = HeadersStatus::duplicate_pseudo_header;
                    return out;
                }
                saw_method  = true;
                head.method = method_to_upper(h.value);
            } else if (name == ":path") {
                if (saw_path) {
                    out.status = HeadersStatus::duplicate_pseudo_header;
                    return out;
                }
                saw_path  = true;
                head.path = h.value;
                if (!split_path(head.path, head.url, head.query)) {
                    out.status = HeadersStatus::invalid_path;
                    return out;
                }
            } else if (name == ":authority") {
                if (saw_authority) {
                    out.status = HeadersStatus::duplicate_pseudo_header;
                    return out;
                }
                saw_authority  = true;
                head.authority = h.value;
            } else if (name == ":scheme") {
                if (saw_scheme) {
                    out.status = HeadersStatus::duplicate_pseudo_header;
                    return out;
                }
                saw_scheme  = true;
                head.scheme = h.value;
            } else {
                // :protocol（extended CONNECT）、:status 之类出现在请求里 —— 都不支持
                out.status = HeadersStatus::unknown_pseudo_header;
                return out;
            }
            continue;
        }

        saw_regular = true;

        if (is_malformed_request_header(name)) {
            out.status = HeadersStatus::malformed_header;
            return out;
        }
        if (is_ignorable_request_header(name)) {
            continue;
        }
        if (name == "content-length") {
            if (head.has_content_length) {
                out.status = HeadersStatus::duplicate_content_length;
                return out;
            }
            std::uint64_t v = 0;
            if (!parse_u64(h.value, v)) {
                out.status = HeadersStatus::invalid_content_length;
                return out;
            }
            head.has_content_length = true;
            head.content_length     = v;
            head.headers.push_back(Header{name, h.value});
            continue;
        }

        head.headers.push_back(Header{name, h.value});
    }

    if (!saw_method || head.method.empty()) {
        out.status = HeadersStatus::missing_method;
        return out;
    }
    if (!saw_path || head.path.empty()) {
        // CONNECT 的 :path 本来就是空的（authority-form）；本库暂不支持 CONNECT，
        // 统一按"缺 :path"拒绝，避免出现"看起来成功但没路由键"的半成品请求。
        out.status = HeadersStatus::missing_path;
        return out;
    }

    // :authority → host（h2 没有 Host 头；业务侧按 h1 习惯读 host）
    if (!head.authority.empty() && !has_header(head.headers, "host")) {
        head.headers.insert(head.headers.begin(), Header{"host", head.authority});
    }

    out.status = HeadersStatus::ok;
    return out;
}

// -----------------------------------------------------------------------------
// 响应
// -----------------------------------------------------------------------------

HeaderList build_response_headers(const ResponseHead& head,
                                  std::uint64_t      content_length,
                                  bool               include_content_length)
{
    HeaderList out;
    out.reserve(head.headers.size() + 2);

    // :status 必须第一个（RFC 9113 §8.3）
    out.push_back(Header{":status", std::to_string(head.status)});

    if (include_content_length) {
        out.push_back(Header{"content-length", std::to_string(content_length)});
    }

    for (const Header& h : head.headers) {
        const std::string name = ascii_lower(h.name);
        if (name.empty() || is_pseudo_header(name)) {
            continue;   // 业务塞进来的伪头会与上面生成的冲突，直接丢掉
        }
        if (is_forbidden_response_header(name)) {
            continue;   // 逐跳头发出去就是协议错误
        }
        if (name == "content-length") {
            continue;   // 以本函数生成的为准（业务值常与正文不一致）
        }
        out.push_back(Header{name, h.value});
    }
    return out;
}

} // namespace h2
} // namespace h2mech
