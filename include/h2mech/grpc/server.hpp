// =============================================================================
//  h2mech/grpc/server.hpp —— gRPC 服务端（wire 级，挂在 h2::Server 上）
// =============================================================================
//  ★ 定位：设备**进程内提供** gRPC 服务。方法路径就是 h2 的 `:path`
//    （`/pkg.Service/Method`），响应由 `grpc-status`（trailers）收尾。
//
//  ★ 两种响应形态（gRPC 规范，必须都对）
//    1. 有正文：HEADERS(content-type) → DATA → HEADERS(trailers: grpc-status)
//    2. **Trailers-Only**：没有正文时，`grpc-status` 直接放在**响应头**里，
//       HEADERS 直接带 END_STREAM，不单发 trailers。
//    错误应答（UNIMPLEMENTED / INVALID_ARGUMENT / UNAUTHENTICATED）走第 2 种 ——
//    写成第 1 种时，某些客户端（含 grpc-go）会认为"缺 status"而报 UNKNOWN。
//
//  ★ 同步派发
//    处理函数在**该连接的 io 线程**上同步执行完成（h2::Server 就是整包派发）。
//    业务不要在里面阻塞；真正需要异步时要先把 `ServerCall` 拷出来再回复 ——
//    当前版本不支持（`ServerCall` 只在回调期内有效）。
//
//  ★ 不做 protobuf
//    请求/响应都是**已序列化**的字节，proto 由应用侧 protoc 生成。
// =============================================================================
#pragma once

#include "h2mech/export.hpp"
#include "h2mech/grpc/framer.hpp"
#include "h2mech/grpc/status.hpp"
#include "h2mech/h2/headers.hpp"
#include "h2mech/h2/server.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace netmech {
class Runtime;
} // namespace netmech

namespace h2mech {
namespace grpc {

/// 一次服务端调用（**只在处理函数执行期间有效**）。
class H2MECH_API ServerCall {
public:
    ServerCall(const ServerCall&)            = delete;
    ServerCall& operator=(const ServerCall&) = delete;
    ~ServerCall();

    /// 方法路径，即 h2 的 `:path`（如 `/helloworld.Greeter/SayHello`）。
    [[nodiscard]] const std::string& method() const noexcept { return method_; }

    /// 请求 metadata（已剔除伪头 / `content-type` / `te` / 逐跳头）。
    [[nodiscard]] const h2::HeaderList& metadata() const noexcept { return metadata_; }

    /// 对端给的 `grpc-timeout` 原值（空 = 未设置）。解析成毫秒由业务决定。
    [[nodiscard]] const std::string& timeout_header() const noexcept { return timeout_; }

    /// 发一条响应消息。可多次调用（unary 用一次；流式由业务自行节奏）。
    /// @return false = 流已作废（对端 RST / 已 finish）
    bool send(std::string_view payload);

    /// 结束调用。
    ///
    /// @param[in] status    gRPC 状态（`ok()` 为 true 表示成功）
    /// @param[in] trailers  业务 trailers（会拼在 `grpc-status` 之后）
    ///
    /// @par 什么时候是 Trailers-Only
    /// 此前**没有** `send()` 过 ⇒ `grpc-status` 直接进响应头（规范形态），
    /// 否则作为真正的 trailers 发出。
    ///
    /// @return false = 流已作废
    bool finish(Status status, h2::HeaderList trailers = {});

    /// 是否已经 `finish()` 过（框架用：处理函数忘了收尾时要兜底，否则流会挂着）。
    [[nodiscard]] bool finished() const noexcept { return finished_; }

public:
    /// @warning 仅供库内部构造（框架在派发前建好）。不要自己 new 一个。
    ServerCall(std::string method, h2::HeaderList metadata, std::string timeout,
               h2::StreamResponder& out);

private:

    std::string          method_;
    h2::HeaderList       metadata_;
    std::string          timeout_;
    h2::StreamResponder* out_ = nullptr;
    bool                 headers_sent_ = false;
    bool                 finished_     = false;
};

// =============================================================================
//  双向流（bidi）
// =============================================================================

/// 一次双向流调用的上下文。
///
/// @warning 只在**回调期间**有效（和 `ServerCall` 一样）：不要把它存起来、
///          也不要跨线程用 —— 底层 `StreamResponder` 是每次回调新建的。
class H2MECH_API BidiCall {
public:
    BidiCall(const BidiCall&)            = delete;
    BidiCall& operator=(const BidiCall&) = delete;
    ~BidiCall();

    [[nodiscard]] const std::string& method() const noexcept { return method_; }
    [[nodiscard]] const h2::HeaderList& metadata() const noexcept { return metadata_; }
    [[nodiscard]] const std::string& timeout_header() const noexcept { return timeout_; }

    /// 发一条响应消息（在收到任意一条请求消息的回调里都能发）。
    bool send(std::string_view payload);

    /// 结束调用（对端再来消息也不会再回调业务）。
    bool finish(Status status, h2::HeaderList trailers = {});

    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// @warning 仅供库内部构造（`finished_flag` 指向框架保存的“本流是否已收尾”）。
    BidiCall(std::string method, h2::HeaderList metadata, std::string timeout,
             h2::StreamResponder& out, bool* finished_flag);

private:
    std::string          method_;
    h2::HeaderList       metadata_;
    std::string          timeout_;
    h2::StreamResponder* out_          = nullptr;
    bool*                finished_flag_ = nullptr;
    bool                 finished_      = false;
};

/// 双向流处理函数：**每条请求消息回调一次**。
///
/// @note 对端半关闭（END_STREAM）时，若业务还没 `finish()`，框架会用 `OK` 兜底
///       收尾 —— 这是"请求-响应式"双向流（echo/chat）最常用的形态；
///       需要"半关闭后继续推"的场景请在那之前自己 `finish()`。
using BidiHandler = std::function<void(BidiCall& call, const Frame& request)>;

/// unary 处理函数：拿到一条请求消息，自己决定 send / finish。
using UnaryHandler = std::function<void(ServerCall& call, const Frame& request)>;

/// 方法注册表（`/pkg.Service/Method` → 处理函数）。
class H2MECH_API Service {
public:
    /// 注册 unary 方法。
    /// @note 路径要写成 `/pkg.Service/Method`（带前导斜杠，与 h2 的 `:path` 一致）。
    ///       重复注册会覆盖前一个 —— 启动期的配置问题，日志里能看出来。
    void add_unary(std::string method_path, UnaryHandler handler);

    /// 注册双向流方法（每条请求消息回调一次）。
    /// @note 同一路径同时注册 unary 与 bidi 时以 bidi 优先。
    void add_bidi_stream(std::string method_path, BidiHandler handler);

    [[nodiscard]] bool has(std::string_view method_path) const;

    /// 查找处理函数（内部/诊断用）。
    /// @return 找不到返回 nullptr；指针只在 Service 存活期间有效，**不要存**。
    [[nodiscard]] const UnaryHandler* find_unary(std::string_view method_path) const noexcept;
    [[nodiscard]] const BidiHandler*  find_bidi(std::string_view method_path) const noexcept;

    /// 已注册的方法数（诊断用）。
    [[nodiscard]] std::size_t size() const noexcept { return unary_.size() + bidi_.size(); }

private:
    std::unordered_map<std::string, UnaryHandler> unary_;
    std::unordered_map<std::string, BidiHandler>  bidi_;
};

/// gRPC 服务端配置。
struct ServerConfig {
    /// 底层 h2 的参数（监听地址、TLS、并发流、正文上限…全都用它）。
    h2::Server::Config http;

    /// 单条请求消息上限（5 字节前缀里的长度不能超过它）。
    std::size_t max_message_bytes = 4u << 20;
};

/// gRPC 服务端：一个 `h2::Server` + 一张方法表。
class H2MECH_API Server : public std::enable_shared_from_this<Server> {
public:
    struct Impl;   ///< 内部实现（不透明）

    /// 创建（不监听）。
    static std::shared_ptr<Server> create(ServerConfig                      cfg,
                                          std::shared_ptr<Service>          service,
                                          std::shared_ptr<netmech::Runtime> runtime = nullptr);

    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// 开始监听。重复调用返回 `Errc::already_started`。
    [[nodiscard]] std::error_code start();

    /// 停止监听并关闭所有连接（幂等）。
    void stop() noexcept;

    /// 实际监听地址。
    [[nodiscard]] netmech::Address local() const;

    [[nodiscard]] bool running() const noexcept;

private:
    explicit Server(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> m_impl;
};

} // namespace grpc
} // namespace h2mech
