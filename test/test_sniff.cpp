// =============================================================================
//  同端口 h1 / h2c 嗅探（纯函数）
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/h2/sniff.hpp>

#include <string>

using h2mech::h2::ProtocolHint;
using h2mech::h2::sniff_protocol;

H2MECH_TEST(sniff_empty_is_need_more)
{
    // 还没收到字节 ≠ 不是 h2c
    CHECK(sniff_protocol("") == ProtocolHint::need_more);
}

H2MECH_TEST(sniff_full_preface_is_h2c)
{
    CHECK(sniff_protocol(h2mech::h2::h2c_preface()) == ProtocolHint::h2c);

    // 前言后面还跟着 SETTINGS 等字节（真实情况）也一样判定
    std::string more{h2mech::h2::h2c_preface()};
    more.append("\x00\x00\x00\x04\x00\x00\x00\x00\x00", 9);
    CHECK(sniff_protocol(more) == ProtocolHint::h2c);
}

H2MECH_TEST(sniff_partial_preface_is_need_more)
{
    const std::string magic{h2mech::h2::h2c_preface()};
    for (std::size_t i = 1; i < magic.size(); ++i) {
        CHECK(sniff_protocol(std::string_view{magic}.substr(0, i)) == ProtocolHint::need_more);
    }
    // 到 24 字节才算 h2c
    CHECK(sniff_protocol(magic) == ProtocolHint::h2c);
}

H2MECH_TEST(sniff_http1_requests_are_http1)
{
    CHECK(sniff_protocol("GET / HTTP/1.1\r\nHost: x\r\n\r\n") == ProtocolHint::http1);
    CHECK(sniff_protocol("POST /a HTTP/1.1\r\n") == ProtocolHint::http1);
    CHECK(sniff_protocol("HEAD / HTTP/1.0\r\n") == ProtocolHint::http1);
    CHECK(sniff_protocol("OPTIONS * HTTP/1.1\r\n") == ProtocolHint::http1);

    // 第一个字节就能定：不必等满 24 字节
    CHECK(sniff_protocol("G") == ProtocolHint::http1);
}

H2MECH_TEST(sniff_prefix_but_wrong_byte_is_http1)
{
    std::string almost{h2mech::h2::h2c_preface()};
    almost[3] = 'X';   // "PRI X ..." —— 差一个字节就不能当 h2c
    CHECK(sniff_protocol(almost) == ProtocolHint::http1);

    // 大小写不同也不行（前言是逐字节精确匹配）
    CHECK(sniff_protocol("pri * HTTP/2.0\r\n\r\nSM\r\n\r\n") == ProtocolHint::http1);
}

H2MECH_TEST(sniff_tls_handshake_is_http1)
{
    // TLS ClientHello 的第一个字节是 0x16；连接上出现它说明对端在说 TLS，
    // 但我们（这个端口）没开 TLS —— 判成 http1 让 h1 层去回错误即可。
    const std::string tls_hello{"\x16\x03\x01\x00\xa5", 5};
    CHECK(sniff_protocol(tls_hello) == ProtocolHint::http1);
}
