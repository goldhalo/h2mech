// =============================================================================
//  h2mech/grpc/client.hpp —— gRPC unary 客户端（h2 + TLS，wire 级）
// =============================================================================
//  ★ 定位：**设备往平台调用**的场景（上报、拉配置），一条连接、一次一个调用。
//    不做连接池、不做重试、不做负载均衡 —— 这些在"单设备对单平台"的形态下
//    收益极低，而每加一个都要跟着一套超时/退避/状态机。
//
//  ★ 同步接口、异步实现
//    `call()` 在调用线程上阻塞（条件变量），真正的收发在 netmech 的 io 线程上
//    由回调驱动。调用线程**不要**是 io 线程（显然）也不要持有会影响回调的锁。
//
//  ★ 不做 protobuf
//    请求/响应都是**已序列化的字节**，proto 由应用侧生成（protoc）。
// =============================================================================
#pragma once

#include "h2mech/common.hpp"
#include "h2mech/export.hpp"
#include "h2mech/grpc/status.hpp"
#include "h2mech/h2/headers.hpp"

#include <netmech/common.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace netmech {
class Mechanism;
class Runtime;
struct TlsConfig;
} // namespace netmech

namespace h2mech {
namespace grpc {

/// 客户端配置。
struct ClientConfig {
    /// 目标地址（IP 字面量；本库不做 DNS 解析）。
    netmech::Address remote{"127.0.0.1", 50051};

    /// 非空 ⇒ h2 over TLS（ALPN 由本库固定为 `h2`）。
    /// 客户端必须给 `pinned_sha256` 或 `verify_certificate = true`。
    std::shared_ptr<const netmech::TlsConfig> tls;

    /// TCP 建链（+TLS 握手）超时。
    std::chrono::milliseconds connect_timeout{3000};

    /// `call()` 没给 timeout 时用的默认值。
    std::chrono::milliseconds default_timeout{10000};

    /// 单条响应消息的大小上限（防对端用长度前缀逼我们分配巨量内存）。
    std::size_t max_message_bytes = 4u << 20;

    /// 写进 `:authority` 的值。空 = 用 `remote` 的 `IP:port`。
    std::string authority;
};

/// 一次 unary 调用的结果。
struct UnaryResult {
    /// gRPC 状态（来自 trailers / Trailers-Only 的响应头）。
    Status status;

    /// 响应消息（已序列化的字节；`status.ok()` 时才有意义）。
    std::string message;

    /// HTTP 层的状态码（仅观测用；gRPC 正常永远是 200）。
    int http_status = 0;

    /// 是否收到了完整响应（false = 连接失败/超时，此时 `status` 是本库给的归类）。
    bool reached = false;

    [[nodiscard]] bool ok() const noexcept { return reached && status.ok(); }
};

/// gRPC unary 客户端（单连接、串行）。
class H2MECH_API Client : public std::enable_shared_from_this<Client> {
public:
    /// 创建（不建链；第一次 `call()` 时自动连）。
    /// @param[in] runtime 空 = netmech 的进程级默认运行时
    static std::shared_ptr<Client> create(ClientConfig                      cfg,
                                          std::shared_ptr<netmech::Runtime> runtime = nullptr);

    /// 复用外部 Mechanism（同一进程多个库共用 io 池时用）。
    static std::shared_ptr<Client> create(std::shared_ptr<netmech::Mechanism> mech,
                                          ClientConfig                      cfg);

    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    /// 发起一次 unary 调用并等待结果。
    ///
    /// @param[in] method_path 形如 `/helloworld.Greeter/SayHello`
    /// @param[in] request     已序列化的请求消息
    /// @param[in] metadata    额外的请求头（会被转成小写；伪头/逐跳头会被剔除）
    /// @param[in] timeout     本次调用的总超时（<=0 = 用 ClientConfig 的默认值）
    ///
    /// @return 调用结果。**不会抛异常**（内部错误都映射成 `Status`）。
    ///
    /// @note 串行：已有调用在进行时，本次立即返回 `Code::internal`
    ///       并带 message "已有调用在进行"（一个设备对平台的链路，够用）。
    [[nodiscard]] UnaryResult call(std::string_view   method_path,
                                   std::string_view   request,
                                   h2::HeaderList     metadata = {},
                                   std::chrono::milliseconds timeout = {});

    /// 关闭连接（幂等）。之后再 `call()` 会自动重连。
    void close() noexcept;

    /// 当前连接是否可用（诊断用）。
    [[nodiscard]] bool connected() const noexcept;

public:
    struct Impl;   ///< 内部实现（不透明，定义在 src/grpc/client.cpp）

private:
    /// 只允许 `create()` 构造。
    explicit Client(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> m_impl;
};

} // namespace grpc
} // namespace h2mech
