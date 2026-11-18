// =============================================================================
//  h2mech/h2/server.hpp —— HTTP/2 服务端（h2c 与 h2 over TLS）
// =============================================================================
//  ★ 分层
//      Conn（netmech）→ Session（本文件）→ Handler（业务）
//    Session 负责：preface 校验、nghttp2 状态机驱动、流表、读写泵。
//    Handler 只管"拿到语义化请求，交回响应"。
//
//  ★ 线程模型（继承 netmech 的约定）
//    一个 io_context 一条线程，同一连接的回调严格串行 —— 所以 Session 内部
//    **不需要锁**，也不需要 strand。Handler 的回调在该连接的 io 线程上执行，
//    不要在里面阻塞。
//
//  ★ 响应模型（S2 阶段）
//    整包：`send_headers()` 之后 `send_data()` 攒进缓冲，`finish()` 一次性
//    提交（nghttp2 的 data provider 从缓冲里吐）。流式分块在 S3 之后再做。
// =============================================================================
#pragma once

#include "h2mech/common.hpp"
#include "h2mech/export.hpp"
#include "h2mech/h2/headers.hpp"

#include <netmech/common.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace netmech {
class Mechanism;
class Runtime;
struct TlsConfig;
} // namespace netmech

namespace h2mech {
namespace h2 {

struct Request {
    std::string method;      ///< 大写
    std::string url;         ///< 不含 query
    std::string query;
    std::string authority;
    std::string scheme;
    HeaderList  headers;     ///< 小写名；已回填 host
    std::string body;        ///< 收齐后的正文（S2：整包）
};

/// 单条流的响应写出器。**只在 on_request 的回调期间有效**，不要存起来。
///
/// @par 典型用法
/// @code
///   void on_request(Request& req, Responder& out) override {
///       out.send_headers(200, {{"content-type", "text/plain"}});
///       out.send_data("hello");
///       out.finish();                     // 发 END_STREAM
///   }
/// @endcode
class H2MECH_API Responder {
public:
    Responder(const Responder&)            = delete;
    Responder& operator=(const Responder&) = delete;

    /// 设定状态与响应头（生成 `:status` 与 `content-length` 由库负责）。
    /// @return false = 该流已作废（对端 RST / 已回过响应）。
    bool send_headers(int status, const HeaderList& headers = {});

    /// 追加正文（整包模型：攒到 finish() 才真正提交）。
    bool send_data(std::string_view data);

    /// 结束响应。@param[in] trailers 非空时作为 trailers 发出（gRPC 的
    /// `grpc-status` 就放这里）；否则 DATA/HEADERS 直接带 END_STREAM。
    bool finish(const HeaderList& trailers = {});

    /// 中止该流（RST_STREAM）。用于"请求非法/超限"这类必须让对端知道的场合。
    bool abort(Errc code);

    [[nodiscard]] std::int32_t stream_id() const noexcept { return stream_id_; }

    /// @warning 仅供库内部构造：不要自己 new 一个 Responder。
    Responder(std::int32_t stream_id, void* session_state) noexcept
        : stream_id_(stream_id), state_(session_state)
    {
    }

private:
    std::int32_t stream_id_;
    void*        state_;   ///< Session 内部状态（Session 保证回调期间有效）
};

/// 业务需要实现的接口。
class H2MECH_API Handler {
public:
    virtual ~Handler() = default;

    /// 请求收齐（END_STREAM）后回调 —— 在该连接的 io 线程上。
    /// @note 实现里**必须**以 `out.finish()` 或 `out.abort()` 结束，否则该流
    ///       会一直挂着（S2 不提供自动超时）。
    virtual void on_request(Request& req, Responder& out) = 0;

    /// 连接关闭（已无在途流）。默认空实现。
    virtual void on_close() {}

    /// 协议级错误（对端发了非法帧等）。连接随后会被关闭。
    virtual void on_protocol_error(std::error_code ec) { (void)ec; }
};

// =============================================================================
//  流式接口（bidi / 服务端流 / 客户端流）
// =============================================================================
//  与上面的 `Handler`（**整包**：收齐请求体才回调一次、响应攒齐才发）相对，
//  这一套是"边收边发"：
//
//    on_open()  ──► on_data() × N ──► on_data(end_stream=true)
//         │                                  │
//         └────► out.write() 可以在任意时刻调用（哪怕请求还没收完）
//
//  这才是 gRPC 双向流的地基：请求消息到达即交付，响应不必等请求结束。
//
//  ★ 全部回调都在该连接的 io 线程上，串行执行 —— 不需要加锁。
//  ★ `out` 只在回调期间有效；但允许**跨回调**多次 `write()`（同一个流）。
// =============================================================================

/// 流式请求头（没有 body）。
struct StreamRequest {
    std::string method;      ///< 大写
    std::string url;         ///< 不含 query
    std::string query;
    std::string authority;
    std::string scheme;
    HeaderList  headers;     ///< 小写名；已回填 host
};

/// 流式响应写出器（同一流的多次 write 之间保持顺序）。
///
/// @note 只有第一次 `write()`（或 `send_headers()`）才真正提交响应头；
///       在那之前业务仍然可以选择"只发状态、不发正文"（Trailers-Only 的前提）。
class H2MECH_API StreamResponder {
public:
    StreamResponder(const StreamResponder&)            = delete;
    StreamResponder& operator=(const StreamResponder&) = delete;

    /// 显式发响应头（status + headers）。不调用时由第一次 write 自动发 200。
    /// @return false = 该流已作废
    bool send_headers(int status, const HeaderList& headers = {});

    /// 发一段响应正文（可多次、可跨回调）。
    /// @note 内部走 nghttp2 的 deferred data provider：积压由 h2 流控与
    ///       netmech 的发送队列共同兜住，业务不需要自己节流。
    bool write(std::string_view data);

    /// 结束响应。此前若没发过头/正文 ⇒ 直接以 HEADERS(END_STREAM) 收尾。
    bool finish(const HeaderList& trailers = {});

    /// 中止该流（RST_STREAM）。
    bool abort(Errc code);

    [[nodiscard]] std::int32_t stream_id() const noexcept { return stream_id_; }

    /// @warning 仅供库内部构造。
    StreamResponder(std::int32_t stream_id, void* session_state) noexcept
        : stream_id_(stream_id), state_(session_state)
    {
    }

private:
    std::int32_t stream_id_;
    void*        state_;
};

/// 流式处理器。
class H2MECH_API StreamHandler {
public:
    virtual ~StreamHandler() = default;

    /// 请求头到达。可以在这里就 `out.write()`（服务端流），也可以等消息。
    virtual void on_open(const StreamRequest& req, StreamResponder& out) = 0;

    /// 请求正文到达。`end_stream == true` 时表示对端不再发了（半关闭）。
    ///
    /// @param[in] out 本次流的新写出器 —— **每次回调都传一个**。
    ///            不要缓存它：`StreamResponder` 的生命周期只到本次回调结束
    ///            （缓存下来再用就是 use-after-free，编译器不会拦你）。
    /// @note 数据**不缓冲**：`data` 同样只在本次回调内有效，要留就自己拷。
    virtual void on_data(std::string_view data, bool end_stream, StreamResponder& out) = 0;

    /// 连接关闭（已无在途流）。默认空实现。
    virtual void on_close() {}

    /// 协议级错误。连接随后会被关闭。
    virtual void on_protocol_error(std::error_code ec) { (void)ec; }
};

/// HTTP/2 服务端。
class H2MECH_API Server : public std::enable_shared_from_this<Server> {
public:
    struct Impl;   ///< 内部实现（不透明，定义在 src/h2/server.cpp）

    struct Config {
        /// 监听地址（端口 0 = 系统分配，起来后用 local() 取实际端口）。
        netmech::Address local{"0.0.0.0", 0};

        std::uint32_t backlog = 128;

        /// SETTINGS_MAX_CONCURRENT_STREAMS（每连接）。
        std::uint32_t max_concurrent_streams = 128;

        /// 单个请求正文上限（超限 → RST_STREAM + 关闭连接）。
        std::size_t max_body_bytes = 4u << 20;

        /// **单条连接**上所有流"待发响应正文"的总字节上限（默认 8 MiB）。
        ///
        /// 为什么需要它：`max_body_bytes` 是**每条请求**的上限，128 条并发流
        /// 各自攒 4 MiB 就是 512 MiB —— 一个慢客户端（或故意不收的客户端）
        /// 就能把进程撑爆。超过本上限时 `Responder::send_data` 返回 false，
        /// 业务应当自行 `abort()`（或减少正文）。
        std::size_t max_buffered_response_bytes = 8u << 20;

        /// 头部数量上限（防头部炸弹）。
        std::size_t max_headers = 64;

        /// 非空 ⇒ 只接受 **TLS + ALPN `h2`** 的连接（h2 over TLS）。
        /// 空 ⇒ h2c（prior knowledge，不做 `Upgrade: h2c`）。
        std::shared_ptr<const netmech::TlsConfig> tls;
    };

    /// 创建（不监听）。`runtime` 为空时用 netmech 的进程级默认运行时。
    static std::shared_ptr<Server> create(Config                          cfg,
                                          std::shared_ptr<Handler>        handler,
                                          std::shared_ptr<netmech::Runtime> runtime = nullptr);

    /// 复用外部 Mechanism（同一进程多个库共用一套 io 池时用）。
    static std::shared_ptr<Server> create(std::shared_ptr<netmech::Mechanism> mech,
                                          Config                           cfg,
                                          std::shared_ptr<Handler>         handler);

    /// 流式版本（bidi / 服务端流 / 客户端流）。
    /// @note 同一个 Server 只能用**一种**处理器：要么整包 `Handler`，
    ///       要么流式 `StreamHandler`（混用会让"请求体是否缓冲"变得不可预测）。
    static std::shared_ptr<Server> create(Config                            cfg,
                                          std::shared_ptr<StreamHandler>    handler,
                                          std::shared_ptr<netmech::Runtime> runtime = nullptr);

    static std::shared_ptr<Server> create(std::shared_ptr<netmech::Mechanism> mech,
                                          Config                            cfg,
                                          std::shared_ptr<StreamHandler>    handler);

    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// 开始监听。重复调用返回 `Errc::already_started`。
    [[nodiscard]] std::error_code start();

    /// 停止监听并关闭所有连接。幂等。
    void stop() noexcept;

    /// 实际监听地址（`cfg.local.port == 0` 时可用）。
    [[nodiscard]] netmech::Address local() const;

    [[nodiscard]] bool running() const noexcept;

private:
    Server();

    std::shared_ptr<Impl> m_impl;
};

} // namespace h2
} // namespace h2mech
