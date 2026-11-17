// =============================================================================
//  h2mech/grpc/framer.hpp —— gRPC 消息帧（wire 层，**不碰 protobuf**）
// =============================================================================
//  gRPC 的"消息"就是一个极简的前缀头：
//
//      +--------+-------------------+---------------------------+
//      | 1 字节 |     4 字节大端     |         N 字节            |
//      | 压缩位 |   消息体长度 N     |       消息体（proto 字节）  |
//      +--------+-------------------+---------------------------+
//
//  ★ 这一层只做"分帧"，不解析消息体：
//    消息体是 protobuf，但那是应用侧的事（protoc 生成）。库绑进 protobuf 就等于
//    把 protoc 的升级周期也背上，而设备侧的场景是"平台改字段我们跟着发版"。
//
//  ★ 必须支持**增量解析**：5 字节前缀可能在任意位置被 TCP 切断
//    （DATA 帧边界与消息边界无关）。所以这里是一个喂字节的状态机，
//    不是"一次 parse 一整块"的纯函数。
// =============================================================================
#pragma once

#include "h2mech/export.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace h2mech {
namespace grpc {

/// 一条完整的 gRPC 消息。
struct Frame {
    bool        compressed = false;   ///< 前缀头的 bit0（本库不做解压，只透传标记）
    std::string payload;              ///< 消息体（proto 字节）
};

/// 编码一条消息。长度超过 `max_message_bytes` 时返回空串（由调用方报错）。
[[nodiscard]] H2MECH_API std::string encode_message(std::string_view payload,
                                                    bool               compressed = false);

/// 增量消息解析器。
///
/// @par 用法
/// @code
///   MessageReader reader;
///   reader.set_max_message_bytes(4u << 20);
///   // 在 h2 的 on_data 里：
///   if (!reader.feed(data, len)) { /* 帧格式错误，终止本次调用 */ }
///   Frame f;
///   while (reader.next(f)) { on_message(f); }
/// @endcode
class H2MECH_API MessageReader {
public:
    MessageReader() = default;

    MessageReader(const MessageReader&)            = delete;
    MessageReader& operator=(const MessageReader&) = delete;

    /// 单条消息体上限（默认 4 MiB）。
    ///
    /// 没有它，对端只要发一个 `FF FF FF FF` 的长度前缀就能让我们预分配 4 GiB ——
    /// 这是最廉价的拒绝服务手法。
    void set_max_message_bytes(std::size_t bytes) noexcept { max_message_bytes_ = bytes; }
    [[nodiscard]] std::size_t max_message_bytes() const noexcept { return max_message_bytes_; }

    /// 喂字节。
    ///
    /// @return false = 帧格式错误（长度超上限、或压缩位非 0/1）。
    ///         出错后解析器进入 failed 状态，必须 `reset()` 才能复用。
    bool feed(const std::uint8_t* data, std::size_t len);
    bool feed(std::string_view data)
    {
        return feed(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

    /// 取出一条完整消息。
    /// @return false = 当前缓冲里还没有完整消息（不是错误；看 failed()）
    bool next(Frame& out);

    [[nodiscard]] bool        failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size() - pos_; }

    /// 清空状态（含 failed 标志）。
    void reset()
    {
        buf_.clear();
        pos_     = 0;
        failed_  = false;
    }

private:
    std::string buf_;                       ///< 未消费的字节
    std::size_t pos_ = 0;                   ///< 已消费到哪
    std::size_t max_message_bytes_ = 4u << 20;
    bool        failed_ = false;
};

} // namespace grpc
} // namespace h2mech
