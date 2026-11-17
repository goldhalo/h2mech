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
