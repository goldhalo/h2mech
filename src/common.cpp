// =============================================================================
//  h2mech - 错误码实现
// =============================================================================
#include "h2mech/common.hpp"

namespace h2mech {
namespace {

class Category final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "h2mech"; }

    [[nodiscard]] std::string message(int value) const override
    {
        switch (static_cast<Errc>(value)) {
        case Errc::ok:                     return "ok";
        case Errc::invalid_argument:       return "invalid argument";
        case Errc::not_started:            return "not started";
        case Errc::already_started:        return "already started";
        case Errc::not_connected:          return "not connected";
        case Errc::link_closed:            return "link closed";
        case Errc::unsupported:            return "unsupported in this build";
        case Errc::buffer_full:            return "send queue full (backpressure)";
        case Errc::timeout:                return "timeout";

        case Errc::protocol_error:         return "HTTP/2 PROTOCOL_ERROR";
        case Errc::internal_error:         return "HTTP/2 INTERNAL_ERROR";
        case Errc::flow_control_error:     return "HTTP/2 FLOW_CONTROL_ERROR";
        case Errc::settings_timeout:       return "HTTP/2 SETTINGS_TIMEOUT";
        case Errc::stream_closed:          return "HTTP/2 STREAM_CLOSED";
        case Errc::frame_size_error:       return "HTTP/2 FRAME_SIZE_ERROR";
        case Errc::refused_stream:         return "HTTP/2 REFUSED_STREAM (可安全重试)";
        case Errc::cancel:                 return "HTTP/2 CANCEL (对端主动取消，别重试)";
        case Errc::compression_error:      return "HTTP/2 COMPRESSION_ERROR (HPACK 出错)";
        case Errc::connect_error:          return "HTTP/2 CONNECT_ERROR";
        case Errc::enhance_your_calm:      return "HTTP/2 ENHANCE_YOUR_CALM";
        case Errc::inadequate_security:    return "HTTP/2 INADEQUATE_SECURITY";
        case Errc::http_1_1_required:      return "HTTP/2 HTTP_1_1_REQUIRED";

        case Errc::goaway:                 return "对端已发 GOAWAY，不能再开新流";
        case Errc::stream_limit_exceeded:  return "并发流超过本地上限";
        case Errc::header_list_too_large:  return "头部列表超过上限";

        case Errc::grpc_status_nonzero:        return "gRPC 返回非 0 grpc-status";
        case Errc::grpc_invalid_frame:         return "gRPC 消息帧长度与实际数据不符";
        case Errc::grpc_unsupported_encoding:  return "gRPC 压缩方式不支持";
        }
        return "unknown h2mech error";
    }
};

} // namespace

const std::error_category& error_category() noexcept
{
    // 函数内 static：避免静态初始化顺序问题（跨 DLL/so 也安全）。
    static const Category kInstance;
    return kInstance;
}

std::error_code make_error_code(Errc e) noexcept
{
    return std::error_code{static_cast<int>(e), error_category()};
}

bool is_h2mech_error(const std::error_code& ec) noexcept
{
    return &ec.category() == &error_category();
}

} // namespace h2mech
