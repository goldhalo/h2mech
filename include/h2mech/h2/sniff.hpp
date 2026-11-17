// =============================================================================
//  h2mech/h2/sniff.hpp —— 同端口区分 h2c 与 HTTP/1.1（靠客户端前言）
// =============================================================================
//  h2c 的"prior knowledge"形态没有协商步骤：客户端连上就直接发
//
//      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"     （24 字节，RFC 9113 §3.4）
//
//  而任何 HTTP/1.1 请求都以方法名开头（`GET ` / `POST ` / …），永远不会以
//  `PRI *` 开头。所以只要**嗅探前 24 字节**就能分流，不需要 TLS/ALPN。
//
//  ★ 用法（必须把"已经读到的字节"还给被选中的协议处理器）
//  @code
//    // 连接建立后先 peek 一段（不要消费掉）
//    const auto hint = h2mech::h2::sniff_protocol(peeked);
//    switch (hint) {
//    case h2mech::h2::ProtocolHint::need_more:  continue reading;
//    case h2mech::h2::ProtocolHint::h2c:        h2_session.feed(peeked);  break;
//    case h2mech::h2::ProtocolHint::http1:      h1_parser.feed(peeked);   break;
//    }
//  @endcode
//  漏掉"feed(peeked)"那一步是这类实现的经典 bug：嗅探过的字节不会再从内核来一次。
//
//  ★ 本库不替你做分流：netmech 的连接已经交给了某个 handler，谁做嗅探谁负责
//    re-feed。这里只提供那个**纯函数判定**，因为它值得单独测（边界比想象的多）。
// =============================================================================
#pragma once

#include <cstddef>
#include <string_view>

namespace h2mech {
namespace h2 {

/// h2c 客户端连接前言的长度。
inline constexpr std::size_t kH2cPrefaceLen = 24;

/// h2c 客户端连接前言（"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"）。
[[nodiscard]] inline constexpr std::string_view h2c_preface() noexcept
{
    return std::string_view{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", kH2cPrefaceLen};
}

/// 嗅探结论。
enum class ProtocolHint {
    need_more,   ///< 已读字节仍是前言的**前缀**，但不足 24 字节 —— 继续读
    h2c,         ///< 24 字节全匹配 → 走 HTTP/2
    http1,       ///< 与前言不符（任何位置不相同）→ 走 HTTP/1.1
};

/// 判断连接最前面的字节序列属于哪种协议。
///
/// @param[in] first_bytes 该连接上**已读到**的字节（可以少于 24 字节，也可以多于）
///
/// @return `need_more`（继续读）/ `h2c`（已完整匹配）/ `http1`（不匹配）
///
/// @note 空输入返回 `need_more` —— "还没收到东西"不等于"不是 h2c"。
/// @note 判定是**逐字节最短路径**：`G` 一进来就能判 http1，不必等 24 字节；
///       这对 h1 的首字节延迟有意义（不必为每个 h1 连接都攒满 24 字节）。
[[nodiscard]] inline ProtocolHint sniff_protocol(std::string_view first_bytes) noexcept
{
    const std::string_view magic = h2c_preface();
    const std::size_t      n     = (first_bytes.size() < magic.size()) ? first_bytes.size()
                                                                       : magic.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (first_bytes[i] != magic[i]) {
            return ProtocolHint::http1;
        }
    }
    if (first_bytes.size() >= magic.size()) {
        return ProtocolHint::h2c;
    }
    return ProtocolHint::need_more;
}

} // namespace h2
} // namespace h2mech
