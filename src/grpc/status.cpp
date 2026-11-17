// =============================================================================
//  gRPC 状态 / 超时 / content-type
// =============================================================================
#include "h2mech/grpc/status.hpp"

#include <cstddef>

namespace h2mech {
namespace grpc {
namespace {

/// ASCII 大小写不敏感比较（不分配内存：这些函数都标了 noexcept）。
bool equals_ci(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - ('A' - 'a')) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - ('A' - 'a')) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/// 在头列表里找第一个同名项（名字大小写不敏感：h2 一律小写，但业务可能自造）。
const std::string* find(const h2::HeaderList& list, std::string_view name) noexcept
{
    for (const h2::Header& h : list) {
        if (equals_ci(h.name, name)) {
            return &h.value;
        }
    }
    return nullptr;
}

} // namespace

const char* code_name(Code c) noexcept
{
    switch (c) {
    case Code::ok:                  return "OK";
    case Code::cancelled:           return "CANCELLED";
    case Code::unknown:             return "UNKNOWN";
    case Code::invalid_argument:    return "INVALID_ARGUMENT";
    case Code::deadline_exceeded:   return "DEADLINE_EXCEEDED";
    case Code::not_found:           return "NOT_FOUND";
    case Code::already_exists:      return "ALREADY_EXISTS";
    case Code::permission_denied:   return "PERMISSION_DENIED";
    case Code::resource_exhausted:  return "RESOURCE_EXHAUSTED";
    case Code::failed_precondition: return "FAILED_PRECONDITION";
    case Code::aborted:             return "ABORTED";
    case Code::out_of_range:        return "OUT_OF_RANGE";
    case Code::unimplemented:       return "UNIMPLEMENTED";
    case Code::internal:            return "INTERNAL";
    case Code::unavailable:         return "UNAVAILABLE";
    case Code::data_loss:           return "DATA_LOSS";
    case Code::unauthenticated:     return "UNAUTHENTICATED";
    }
    return "UNKNOWN";
}

bool parse_code(std::string_view text, Code& out) noexcept
{
    if (text.empty() || text.size() > 3) {
        return false;
    }
    int v = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        v = v * 10 + (ch - '0');
    }
    if (v < 0 || v > 16) {
        return false;
    }
    out = static_cast<Code>(v);
    return true;
}

Status parse_status(const h2::HeaderList& headers, const h2::HeaderList& trailers)
{
    // trailers 更权威：Trailers-Only 时两处相同，正常形态时只有 trailers 有。
    const std::string* raw = find(trailers, "grpc-status");
    if (raw == nullptr) {
        raw = find(headers, "grpc-status");
    }

    Status st;
    if (raw == nullptr) {
        // 缺 grpc-status → 对端没按 gRPC 说话。**不能**当成成功：
        // 那会把"服务端 500 / 代理截断"这类错误静默吞掉。
        st.code = Code::unknown;
        st.message = "缺少 grpc-status（对端未按 gRPC 协议响应）";
        return st;
    }
    if (!parse_code(*raw, st.code)) {
        st.code = Code::unknown;
        st.message = "无法解析 grpc-status: " + *raw;
        return st;
    }
    if (const std::string* msg = find(trailers, "grpc-message")) {
        st.message = *msg;
    } else if (const std::string* msg2 = find(headers, "grpc-message")) {
        st.message = *msg2;
    }
    return st;
}

std::string encode_timeout(std::chrono::milliseconds ms)
{
    if (ms.count() <= 0) {
        return std::string();
    }
    // 规范：1..8 位十进制 + 单位（H/M/S/m/u/n）。
    //
    // 策略是"**尽可能精确**"而不是"尽可能用大单位"：
    // 毫秒能放进 8 位（<= 99,999,999 ms ≈ 27.7 小时）就直接用 'm'，
    // 这样 1500ms 发的是 "1500m" 而不是被取整成 "2S" —— 后者会让超时
    // 比调用方要求的更宽松，通信管理机上"2 秒变 500 毫秒"这种偏差很难解释。
    const long long v = static_cast<long long>(ms.count());
    if (v <= 99'999'999LL) {
        return std::to_string(v) + "m";
    }

    // 更长的超时（少见）：先退到秒，仍超 8 位再退到小时；一律**向上取整**，
    // 宁可多给一点，也不要比调用方要求的更短（短了会先被判 DEADLINE_EXCEEDED）。
    const long long sec = (v + 999) / 1000;
    if (sec <= 99'999'999LL) {
        return std::to_string(sec) + "S";
    }
    const long long hour = (sec + 3599) / 3600;
    return std::to_string(hour) + "H";
}

bool is_grpc_content_type(std::string_view value) noexcept
{
    // 允许 "application/grpc" 与 "application/grpc+proto" / "+json" 等后缀。
    constexpr std::string_view kPrefix = "application/grpc";
    if (value.size() < kPrefix.size()) {
        return false;
    }
    if (!equals_ci(value.substr(0, kPrefix.size()), kPrefix)) {
        return false;
    }
    return value.size() == kPrefix.size() || value[kPrefix.size()] == '+';
}

bool needs_te_trailers(const h2::HeaderList& headers) noexcept
{
    return find(headers, "te") == nullptr;   // 没有 te 头 → 需要补一个
}

} // namespace grpc
} // namespace h2mech
