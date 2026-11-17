// =============================================================================
//  gRPC wire 层：消息帧 / 状态 / 超时（纯逻辑，无需网络）
// =============================================================================
#include "test_framework.hpp"

#include <h2mech/grpc/framer.hpp>
#include <h2mech/grpc/status.hpp>

#include <chrono>
#include <cstdint>
#include <string>

using h2mech::grpc::Code;
using h2mech::grpc::Frame;
using h2mech::grpc::MessageReader;
using h2mech::h2::Header;
using h2mech::h2::HeaderList;

// -----------------------------------------------------------------------------
//  消息帧
// -----------------------------------------------------------------------------

H2MECH_TEST(grpc_frame_roundtrip)
{
    const std::string encoded = h2mech::grpc::encode_message("hello");
    REQUIRE(encoded.size() == 5 + 5);
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(encoded[0])), 0);   // 压缩位
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(encoded[1])), 0);   // 长度高字节
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(encoded[4])), 5);   // 长度低字节

    MessageReader reader;
    REQUIRE(reader.feed(encoded));
    Frame f;
    REQUIRE(reader.next(f));
    CHECK(!f.compressed);
    CHECK_EQ(f.payload, std::string{"hello"});
    CHECK(!reader.next(f));          // 没有第二条
    CHECK_EQ(reader.buffered(), std::size_t{0});
}

H2MECH_TEST(grpc_frame_empty_message)
{
    // 空消息是合法的（长度 0），必须能解出"空 payload"而不是"还没收全"
    const std::string encoded = h2mech::grpc::encode_message("");
    REQUIRE(encoded.size() == 5);

    MessageReader reader;
    REQUIRE(reader.feed(encoded));
    Frame f;
    REQUIRE(reader.next(f));
    CHECK(f.payload.empty());
}

H2MECH_TEST(grpc_frame_split_feeds)
{
    // 5 字节前缀被切成任意片都要能拼回来（TCP 不保证消息边界）
    const std::string encoded = h2mech::grpc::encode_message(std::string(300, 'x'));

    for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{64}}) {
        MessageReader reader;
        Frame         f;
        bool          got = false;
        for (std::size_t off = 0; off < encoded.size() && !got; off += chunk) {
            const std::size_t n = (off + chunk <= encoded.size()) ? chunk : encoded.size() - off;
            REQUIRE(reader.feed(reinterpret_cast<const std::uint8_t*>(encoded.data() + off), n));
            got = reader.next(f);
        }
        CHECK(got);
        CHECK_EQ(f.payload.size(), std::size_t{300});
    }
}

H2MECH_TEST(grpc_frame_multiple_messages)
{
    std::string stream = h2mech::grpc::encode_message("a");
    stream += h2mech::grpc::encode_message("bb");
    stream += h2mech::grpc::encode_message("", true);   // 带压缩标志

    MessageReader reader;
    REQUIRE(reader.feed(stream));

    Frame f;
    REQUIRE(reader.next(f));
    CHECK_EQ(f.payload, std::string{"a"});
    REQUIRE(reader.next(f));
    CHECK_EQ(f.payload, std::string{"bb"});
    REQUIRE(reader.next(f));
    CHECK(f.payload.empty());
    CHECK(f.compressed);
    CHECK(!reader.next(f));
}

H2MECH_TEST(grpc_frame_rejects_oversized_length)
{
    // 只发前缀、长度 4 GiB-1：必须直接判失败，而不是等/预分配
    const std::uint8_t prefix[5] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    MessageReader      reader;
    reader.set_max_message_bytes(1u << 20);
    REQUIRE(reader.feed(prefix, sizeof(prefix)));

    Frame f;
    CHECK(!reader.next(f));
    CHECK(reader.failed());

    // 出错后不允许继续用，直到 reset()
    CHECK(!reader.feed(std::string("\x00\x00\x00\x00\x00", 5)));
    reader.reset();
    CHECK(!reader.failed());
}

H2MECH_TEST(grpc_frame_rejects_bad_compressed_bit)
{
    // 压缩位只允许 0/1；0x02 说明对端不是 gRPC（或数据已错位）
    const std::uint8_t frame[6] = {0x02, 0x00, 0x00, 0x00, 0x01, 'x'};
    MessageReader      reader;
    REQUIRE(reader.feed(frame, sizeof(frame)));

    Frame f;
    CHECK(!reader.next(f));
    CHECK(reader.failed());
}

// -----------------------------------------------------------------------------
//  状态
// -----------------------------------------------------------------------------

H2MECH_TEST(grpc_status_from_trailers)
{
    h2mech::grpc::Status st =
        h2mech::grpc::parse_status({}, {{"grpc-status", "0"}});
    CHECK(st.ok());
    CHECK_EQ(std::string{h2mech::grpc::code_name(st.code)}, std::string{"OK"});

    st = h2mech::grpc::parse_status({}, {{"grpc-status", "5"}, {"grpc-message", "no such user"}});
    CHECK(st.code == Code::not_found);
    CHECK_EQ(st.message, std::string{"no such user"});
    CHECK_EQ(std::string{h2mech::grpc::code_name(st.code)}, std::string{"NOT_FOUND"});
}

H2MECH_TEST(grpc_status_trailers_only_form)
{
    // Trailers-Only：没有正文时 grpc-status 直接放在响应头里
    const h2mech::grpc::Status st =
        h2mech::grpc::parse_status({{"content-type", "application/grpc"},
                                    {"grpc-status", "7"},
                                    {"grpc-message", "denied"}},
                                   {});
    CHECK(st.code == Code::permission_denied);
    CHECK_EQ(st.message, std::string{"denied"});
}

H2MECH_TEST(grpc_status_missing_is_not_ok)
{
    // 缺 grpc-status 必须当成错误：否则"服务端 500 / 代理截断"会被静默吞掉
    const h2mech::grpc::Status st =
        h2mech::grpc::parse_status({{"content-type", "text/html"}}, {});
    CHECK(!st.ok());
    CHECK(st.code == Code::unknown);

    // 非法值同理
    const h2mech::grpc::Status bad =
        h2mech::grpc::parse_status({}, {{"grpc-status", "not-a-number"}});
    CHECK(bad.code == Code::unknown);

    // 越界（17）也拒
    Code c = Code::ok;
    CHECK(!h2mech::grpc::parse_code("17", c));
    CHECK(h2mech::grpc::parse_code("16", c));
    CHECK(c == Code::unauthenticated);
}

// -----------------------------------------------------------------------------
//  超时与 content-type
// -----------------------------------------------------------------------------

H2MECH_TEST(grpc_timeout_encoding)
{
    using namespace std::chrono;
    // 毫秒能放进 8 位就发毫秒（保精度，不做"2 秒变 500 毫秒"的取整）
    CHECK_EQ(h2mech::grpc::encode_timeout(milliseconds{250}), std::string{"250m"});
    CHECK_EQ(h2mech::grpc::encode_timeout(milliseconds{1500}), std::string{"1500m"});
    CHECK_EQ(h2mech::grpc::encode_timeout(milliseconds{0}), std::string{});
    CHECK_EQ(h2mech::grpc::encode_timeout(milliseconds{-5}), std::string{});

    // 超过 8 位十进制 → 退到秒，且向上取整（宁可多给）
    CHECK_EQ(h2mech::grpc::encode_timeout(milliseconds{100'000'000LL}), std::string{"100000S"});
    CHECK_EQ(h2mech::grpc::encode_timeout(milliseconds{99'999'999LL}), std::string{"99999999m"});
}

H2MECH_TEST(grpc_content_type_and_te)
{
    CHECK(h2mech::grpc::is_grpc_content_type("application/grpc"));
    CHECK(h2mech::grpc::is_grpc_content_type("application/grpc+proto"));
    CHECK(h2mech::grpc::is_grpc_content_type("Application/GRPC+json"));
    CHECK(!h2mech::grpc::is_grpc_content_type("application/grpc-web"));   // 是另一套协议
    CHECK(!h2mech::grpc::is_grpc_content_type("text/plain"));
    CHECK(!h2mech::grpc::is_grpc_content_type(""));

    const HeaderList without_te{{"content-type", "application/grpc"}};
    CHECK(h2mech::grpc::needs_te_trailers(without_te));

    const HeaderList with_te{{"te", "trailers"}};
    CHECK(!h2mech::grpc::needs_te_trailers(with_te));
}
