// =============================================================================
//  gRPC 服务端实现（挂在 h2mech::h2::Server 的**流式**接口上）
// =============================================================================
//  分层：h2::Server（帧/流/读写泵）→ GrpcBridge（协议判定 + 分帧 + 方法表）
//        → 业务处理函数（unary 收一条回一条；bidi 每条请求消息回调一次）。
//
//  ★ 为什么走流式而不是整包：bidi 要求"请求消息到达即交付、响应不等请求结束"，
//    整包模型（收齐才回调）做不到。unary 只是"一条消息的 bidi"。
//
//  ★ 锁：服务端只有**一把**，保护"流 id → 上下文"的 map。
//    为什么需要：GrpcBridge 是所有连接共享的一个对象，而流 id 只在单条连接内
//    唯一 —— 不同 io 线程可能同时插入不同流。业务回调在**锁外**执行。
// =============================================================================
#include "h2mech/grpc/server.hpp"

#if defined(H2MECH_HAS_NGHTTP2)

#include "h2mech/common.hpp"

#include <netmech/common.hpp>

#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace h2mech {
namespace grpc {
namespace {

/// 在头列表里找第一个同名项（大小写不敏感）。
const std::string* find_header(const h2::HeaderList& list, std::string_view name)
{
    for (const h2::Header& h : list) {
        if (h2::ascii_lower(h.name) == h2::ascii_lower(name)) {
            return &h.value;
        }
    }
    return nullptr;
}

/// `grpc-status` / `grpc-message` 两个 trailer。
h2::HeaderList status_headers(const Status& st)
{
    h2::HeaderList out;
    out.push_back(h2::Header{"grpc-status", std::to_string(static_cast<int>(st.code))});
    if (!st.message.empty()) {
        out.push_back(h2::Header{"grpc-message", st.message});
    }
    return out;
}

/// 非 gRPC 请求：用 HTTP 语义答复（对端不是 gRPC 客户端时，给它 grpc-status
/// 只会让它更困惑）。
void http_error(h2::StreamResponder& out, int status, std::string_view text)
{
    out.send_headers(status, {{"content-type", "text/plain; charset=utf-8"}});
    out.write(text);
    out.finish();
}

/// 只有状态、没有正文 ⇒ Trailers-Only（`stream_finish` 会把它们并进响应头）。
void trailers_only(h2::StreamResponder& out, const Status& st)
{
    out.send_headers(200, {{"content-type", "application/grpc"}});
    out.finish(status_headers(st));
}

/// 单条流的解析上下文。
struct StreamCtx {
    MessageReader reader;
    std::string   method;
    h2::HeaderList metadata;
    std::string   timeout;

    bool          is_bidi         = false;
    bool          writer_finished = false;   ///< 业务已 finish（BidiCall 共享）
    // unary 用
    Frame         request;
    bool          has_request = false;
};

/// 桥接：h2 流 → gRPC 调用。
class GrpcBridge final : public h2::StreamHandler {
public:
    GrpcBridge(std::shared_ptr<Service> service, std::size_t max_message_bytes)
        : service_(std::move(service)), max_message_bytes_(max_message_bytes)
    {
    }

    void on_open(const h2::StreamRequest& req, h2::StreamResponder& out) override
    {
        // 1) 协议判定
        if (req.method != "POST") {
            http_error(out, 405, "gRPC 只接受 POST");
            return;
        }
        const std::string* ct = find_header(req.headers, "content-type");
        if (ct == nullptr || !is_grpc_content_type(*ct)) {
            http_error(out, 415, "content-type 必须是 application/grpc");
            return;
        }

        const std::string& path = req.url;

        // 2) 方法表（bidi 优先）
        const bool has_bidi  = service_->find_bidi(path) != nullptr;
        const bool has_unary = service_->find_unary(path) != nullptr;
        if (!has_bidi && !has_unary) {
            Status st;
            st.code    = Code::unimplemented;
            st.message = "未注册的方法: " + path;
            trailers_only(out, st);
            return;
        }

        auto ctx       = std::make_shared<StreamCtx>();
        ctx->method    = path;
        ctx->is_bidi   = has_bidi;
        ctx->reader.set_max_message_bytes(max_message_bytes_);

        // metadata（剔除伪头 / content-type / te / 逐跳头；grpc-timeout 单独留）
        for (const h2::Header& field : req.headers) {
            const std::string n = h2::ascii_lower(field.name);
            if (n.empty() || h2::is_pseudo_header(n) || h2::is_hop_by_hop(n)
                || n == "content-type") {
                continue;
            }
            if (n == "grpc-timeout") {
                ctx->timeout = field.value;
                continue;
            }
            ctx->metadata.push_back(h2::Header{n, field.value});
        }

        // 响应头先"记下"（不提交）：这样"只发状态"才能走成真正的 Trailers-Only
        out.send_headers(200, {{"content-type", "application/grpc"}});

        {
            std::lock_guard<std::mutex> lk(mu_);
            ctxs_[out.stream_id()] = std::move(ctx);
        }
    }

    void on_data(std::string_view data, bool end_stream, h2::StreamResponder& out) override
    {
        std::shared_ptr<StreamCtx> ctx;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = ctxs_.find(out.stream_id());
            if (it == ctxs_.end()) {
                return;
            }
            ctx = it->second;
        }

        if (!data.empty() && !ctx->reader.feed(data)) {
            Status st;
            st.code    = Code::invalid_argument;
            st.message = "请求消息格式错误（不是合法的 gRPC 帧）";
            trailers_only(out, st);
            erase(out.stream_id());
            return;
        }

        Frame frame;
        while (ctx->reader.next(frame)) {
            if (ctx->is_bidi) {
                const BidiHandler* h = service_->find_bidi(ctx->method);
                if (h != nullptr && !ctx->writer_finished) {
                    BidiCall call{ctx->method, ctx->metadata, ctx->timeout, out,
                                  &ctx->writer_finished};
                    try {
                        (*h)(call, frame);
                    } catch (const std::exception& e) {
                        call.finish(Status{Code::internal,
                                           std::string("处理函数抛出异常: ") + e.what()});
                    } catch (...) {
                        call.finish(Status{Code::internal, "处理函数抛出未知异常"});
                    }
                }
            } else if (!ctx->has_request) {
                ctx->request     = std::move(frame);
                ctx->has_request = true;
            }
        }

        if (!end_stream) {
            return;
        }

        // 对端半关闭
        if (ctx->is_bidi) {
            if (!ctx->writer_finished) {
                // "请求-响应式"双向流：对端不再发，就收尾
                out.finish(status_headers(Status{}));
                ctx->writer_finished = true;
            }
            erase(out.stream_id());
            return;
        }

        // unary：这时候才轮到业务
        const UnaryHandler* h = service_->find_unary(ctx->method);
        if (h == nullptr) {
            trailers_only(out, Status{Code::unimplemented, "方法已注销"});
            erase(out.stream_id());
            return;
        }
        if (!ctx->has_request) {
            Status st;
            st.code    = Code::invalid_argument;
            st.message = "unary 方法必须带一条请求消息";
            trailers_only(out, st);
            erase(out.stream_id());
            return;
        }

        ServerCall call{ctx->method, ctx->metadata, ctx->timeout, out};
        try {
            (*h)(call, ctx->request);
        } catch (const std::exception& e) {
            call.finish(Status{Code::internal, std::string("处理函数抛出异常: ") + e.what()});
        } catch (...) {
            call.finish(Status{Code::internal, "处理函数抛出未知异常"});
        }
        if (!call.finished()) {
            call.finish(Status{Code::internal, "处理函数没有调用 finish()"});
        }
        erase(out.stream_id());
    }

    void on_close() override
    {
        std::lock_guard<std::mutex> lk(mu_);
        ctxs_.clear();
    }

private:
    void erase(std::int32_t id)
    {
        std::lock_guard<std::mutex> lk(mu_);
        ctxs_.erase(id);
    }

    std::shared_ptr<Service> service_;
    std::size_t              max_message_bytes_;

    std::mutex                                              mu_;
    std::unordered_map<std::int32_t, std::shared_ptr<StreamCtx>> ctxs_;
};

} // namespace

// =============================================================================
//  ServerCall（unary）
// =============================================================================

ServerCall::ServerCall(std::string method, h2::HeaderList metadata, std::string timeout,
                       h2::StreamResponder& out)
    : method_(std::move(method))
    , metadata_(std::move(metadata))
    , timeout_(std::move(timeout))
    , out_(&out)
{
}

ServerCall::~ServerCall() = default;

bool ServerCall::send(std::string_view payload)
{
    if (out_ == nullptr || finished_) {
        return false;
    }
    headers_sent_ = true;   // 响应头在 on_open 里已经记下了
    return out_->write(encode_message(payload));
}

bool ServerCall::finish(Status status, h2::HeaderList trailers)
{
    if (out_ == nullptr || finished_) {
        return false;
    }
    finished_ = true;

    h2::HeaderList tail = status_headers(status);
    for (auto& h : trailers) {
        tail.push_back(std::move(h));
    }
    // StreamResponder::finish 会按"有没有发过正文"自动选：
    //   没发过 → 并进响应头（Trailers-Only）
    //   发过   → 作为真正的 trailers
    return out_->finish(tail);
}

// =============================================================================
//  BidiCall
// =============================================================================

BidiCall::BidiCall(std::string method, h2::HeaderList metadata, std::string timeout,
                   h2::StreamResponder& out, bool* finished_flag)
    : method_(std::move(method))
    , metadata_(std::move(metadata))
    , timeout_(std::move(timeout))
    , out_(&out)
    , finished_flag_(finished_flag)
{
    if (finished_flag_ != nullptr) {
        finished_ = *finished_flag_;
    }
}

BidiCall::~BidiCall() = default;

bool BidiCall::send(std::string_view payload)
{
    if (out_ == nullptr || finished_) {
        return false;
    }
    return out_->write(encode_message(payload));
}

bool BidiCall::finish(Status status, h2::HeaderList trailers)
{
    if (out_ == nullptr || finished_) {
        return false;
    }
    finished_ = true;
    if (finished_flag_ != nullptr) {
        *finished_flag_ = true;
    }

    h2::HeaderList tail = status_headers(status);
    for (auto& h : trailers) {
        tail.push_back(std::move(h));
    }
    return out_->finish(tail);
}

// =============================================================================
//  Service
// =============================================================================

void Service::add_unary(std::string method_path, UnaryHandler handler)
{
    if (method_path.empty() || !handler) {
        return;
    }
    unary_[std::move(method_path)] = std::move(handler);
}

void Service::add_bidi_stream(std::string method_path, BidiHandler handler)
{
    if (method_path.empty() || !handler) {
        return;
    }
    bidi_[std::move(method_path)] = std::move(handler);
}

bool Service::has(std::string_view method_path) const
{
    return find_bidi(method_path) != nullptr || find_unary(method_path) != nullptr;
}

const UnaryHandler* Service::find_unary(std::string_view method_path) const noexcept
{
    auto it = unary_.find(std::string(method_path));
    return it == unary_.end() ? nullptr : &it->second;
}

const BidiHandler* Service::find_bidi(std::string_view method_path) const noexcept
{
    auto it = bidi_.find(std::string(method_path));
    return it == bidi_.end() ? nullptr : &it->second;
}

// =============================================================================
//  Server
// =============================================================================

struct Server::Impl {
    ServerConfig                cfg;
    std::shared_ptr<Service>    service;
    std::shared_ptr<h2::Server> http;
};

Server::Server(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Server::~Server() = default;

std::shared_ptr<Server> Server::create(ServerConfig cfg, std::shared_ptr<Service> service,
                                       std::shared_ptr<netmech::Runtime> runtime)
{
    if (!service) {
        return nullptr;
    }
    auto bridge = std::make_shared<GrpcBridge>(service, cfg.max_message_bytes);
    auto http   = h2::Server::create(cfg.http, bridge, std::move(runtime));
    if (!http) {
        return nullptr;
    }

    auto impl     = std::make_shared<Impl>();
    impl->cfg     = std::move(cfg);
    impl->service = std::move(service);
    impl->http    = std::move(http);
    return std::shared_ptr<Server>(new Server(std::move(impl)));
}

std::error_code Server::start()
{
    return m_impl->http->start();
}

void Server::stop() noexcept
{
    m_impl->http->stop();
}

netmech::Address Server::local() const
{
    return m_impl->http->local();
}

bool Server::running() const noexcept
{
    return m_impl->http->running();
}

} // namespace grpc
} // namespace h2mech

#endif // H2MECH_HAS_NGHTTP2
