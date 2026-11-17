// =============================================================================
//  gRPC 消息帧编解码
// =============================================================================
#include "h2mech/grpc/framer.hpp"

#include <cstring>

namespace h2mech {
namespace grpc {
namespace {

constexpr std::size_t kPrefixLen = 5;

} // namespace

std::string encode_message(std::string_view payload, bool compressed)
{
    // 长度字段是 4 字节大端：单条消息最大 4 GiB - 1。
    if (payload.size() > 0xFFFFFFFFu) {
        return std::string();
    }
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());

    std::string out;
    out.reserve(kPrefixLen + payload.size());
    out.push_back(compressed ? '\x01' : '\x00');
    out.push_back(static_cast<char>((len >> 24) & 0xFF));
    out.push_back(static_cast<char>((len >> 16) & 0xFF));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out.append(payload.data(), payload.size());
    return out;
}

bool MessageReader::feed(const std::uint8_t* data, std::size_t len)
{
    if (failed_) {
        return false;
    }
    if (data != nullptr && len > 0) {
        buf_.append(reinterpret_cast<const char*>(data), len);
    }
    return true;
}

bool MessageReader::next(Frame& out)
{
    if (failed_) {
        return false;
    }
    const std::size_t avail = buf_.size() - pos_;
    if (avail < kPrefixLen) {
        return false;   // 前缀还没收全
    }

    const auto* p = reinterpret_cast<const std::uint8_t*>(buf_.data() + pos_);

    // 压缩位只允许 0/1：其它值说明对端不是 gRPC（或数据已经错位），
    // 继续解析只会把垃圾当成长度，直接判失败更安全。
    if (p[0] > 1) {
        failed_ = true;
        return false;
    }
    const std::uint32_t len = (static_cast<std::uint32_t>(p[1]) << 24)
                              | (static_cast<std::uint32_t>(p[2]) << 16)
                              | (static_cast<std::uint32_t>(p[3]) << 8)
                              | static_cast<std::uint32_t>(p[4]);

    if (len > max_message_bytes_) {
        failed_ = true;   // 防"一个前缀就让我们预分配 4 GiB"
        return false;
    }
    if (avail < kPrefixLen + len) {
        return false;     // 正文还没收全
    }

    out.compressed = (p[0] == 1);
    out.payload.assign(buf_.data() + pos_ + kPrefixLen, len);
    pos_ += kPrefixLen + len;
    return true;
}

} // namespace grpc
} // namespace h2mech
