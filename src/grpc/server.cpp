// =============================================================================
//  gRPC 服务端实现（挂在 h2mech::h2::Server 上）
// =============================================================================
//  分层：h2::Server（帧/流/读写泵）→ GrpcBridge（协议判定 + 分帧 + 方法表）
//        → 业务处理函数（拿 ServerCall 自己 send/finish）。
//
//  ★ 这里**没有锁**：h2::Server 的回调天然在单条连接的 io 线程上串行执行，
//    一条调用从头到尾都在同一个回调里完成（同步派发），不存在跨线程共享状态。
//    方法表在 start() 之前建好，之后只读。
// =============================================================================
#include "h2mech/grpc/server.hpp"

#if defined(H2MECH_HAS_NGHTTP2)

#include "h2mech/common.hpp"

#include <netmech/common.hpp>

#include <exception>
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

/// `grpc-status` / `grpc-message` 两个 trailer（Trailers-Only 时进响应头）。
h2::HeaderList status_headers(const Status& st)
{
    h2::HeaderList out;
    out.push_back(h2::Header{"grpc-status", std::to_string(static_cast<int>(st.code))});
    if (!st.message.empty()) {
        // 规范要求 %-encode 非 ASCII/控制字符；本库目前原样透传并在文档里说明
        // （设备侧的错误文案基本都是 ASCII；真要中文得由业务自己编码）。
        out.push_back(h2::Header{"grpc-message", st.message});
    }
    return out;
}

/// 非 gRPC 请求的答复：走 HTTP 语义（gRPC 客户端看到的是"协议不符"）。
void http_error(h2::Responder& out, int status, std::string_view text)
{
    out.send_headers(status, {{"content-type", "text/plain; charset=utf-8"}});
    out.send_data(text);
    out.finish();
}

/// **Trailers-Only**：没有正文时 grpc-status 直接进响应头，HEADERS 带 END_STREAM。
void trailers_only(h2::Responder& out, const Status& st)
{
    h2::HeaderList headers;
    headers.push_back(h2::Header{"content-type", "application/grpc"});
    for (auto& h : status_headers(st)) {
        headers.push_back(std::move(h));
    }
    out.send_headers(200, headers);
    out.finish();
}

/// 桥接：把 h2 请求翻译成 gRPC 调用。
class GrpcBridge final : public h2::Handler {
public:
    GrpcBridge(std::shared_ptr<Service> service, std::size_t max_message_bytes)
        : service_(std::move(service)), max_message_bytes_(max_message_bytes)
    {
    }

    void on_request(h2::Request& req, h2::Responder& out) override
    {
        // 1) 协议判定：gRPC 一律 POST + application/grpc。
        //    用 HTTP 状态码回答（而不是 grpc-status），因为对端这时根本不是
        //    gRPC 客户端 —— 给它 grpc-status 只会让它更困惑。
        if (req.method != "POST") {
            http_error(out, 405, "gRPC 只接受 POST");
            return;
        }
        const std::string* ct = find_header(req.headers, "content-type");
        if (ct == nullptr || !is_grpc_content_type(*ct)) {
            http_error(out, 415, "content-type 必须是 application/grpc");
            return;
        }

        // 2) 方法表查找（表在 start() 前建好，之后只读 → 无需加锁）
        const std::string&  path = req.url;
        const UnaryHandler* h    = service_->find(path);
        if (h == nullptr) {
            Status st;
            st.code    = Code::unimplemented;
            st.message = "未注册的方法: " + path;
            trailers_only(out, st);
            return;
        }

        // 3) 解一条请求消息（unary 只接受恰好一条）
        MessageReader reader;
        reader.set_max_message_bytes(max_message_bytes_);
        Frame request;
        if (!reader.feed(req.body) || !reader.next(request)) {
            Status st;
            st.code    = Code::invalid_argument;
            st.message = "请求消息格式错误（不是合法的 gRPC 帧）";
            trailers_only(out, st);
            return;
        }
        Frame extra;
        if (reader.next(extra)) {
            Status st;
            st.code    = Code::invalid_argument;
            st.message = "unary 方法只接受一条消息";
            trailers_only(out, st);
            return;
        }

        // 4) metadata（剔除伪头 / content-type / te / 逐跳头；grpc-timeout 单独留）
        h2::HeaderList metadata;
        std::string    timeout;
        for (const h2::Header& field : req.headers) {
            const std::string n = h2::ascii_lower(field.name);
            if (n.empty() || h2::is_pseudo_header(n) || h2::is_hop_by_hop(n)
                || n == "content-type") {
                continue;
            }
            if (n == "grpc-timeout") {
                timeout = field.value;
                continue;
            }
            metadata.push_back(h2::Header{n, field.value});
        }

        // 5) 交给业务。异常绝不允许穿透到 io 线程（那会直接 terminate）。
        ServerCall call(path, std::move(metadata), std::move(timeout), out);
        try {
            (*h)(call, request);
        } catch (const std::exception& e) {
            call.finish(Status{Code::internal, std::string("处理函数抛出异常: ") + e.what()});
            return;
        } catch (...) {
            call.finish(Status{Code::internal, "处理函数抛出未知异常"});
            return;
        }

        // 6) 兜底：业务忘了 finish() 时不能让流一直挂着
        if (!call.finished()) {
            call.finish(Status{Code::internal, "处理函数没有调用 finish()"});
        }
    }

private:
    std::shared_ptr<Service> service_;
    std::size_t              max_message_bytes_;
};

} // namespace

// =============================================================================
//  ServerCall
// =============================================================================

ServerCall::ServerCall(std::string method, h2::HeaderList metadata, std::string timeout,
                       h2::Responder& out)
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
    if (!headers_sent_) {
        // 首条正文才发 HEADERS：这样 Trailers-Only 才可能成立（见 finish）。
        if (!out_->send_headers(200, {{"content-type", "application/grpc"}})) {
            return false;
        }
        headers_sent_ = true;
    }
    return out_->send_data(encode_message(payload));
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

    if (!headers_sent_) {
        // **Trailers-Only**：grpc-status 进响应头，HEADERS 直接 END_STREAM。
        // 目标方（grpc-go / grpc-cpp）靠这个形态识别"没有正文的应答"。
        h2::HeaderList headers;
        headers.push_back(h2::Header{"content-type", "application/grpc"});
        for (auto& h : tail) {
            headers.push_back(std::move(h));
        }
        if (!out_->send_headers(200, headers)) {
            return false;
        }
        return out_->finish();
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

bool Service::has(std::string_view method_path) const
{
    return find(method_path) != nullptr;
}

const UnaryHandler* Service::find(std::string_view method_path) const noexcept
{
    auto it = unary_.find(std::string(method_path));
    return it == unary_.end() ? nullptr : &it->second;
}

// =============================================================================
//  Server
// =============================================================================

struct Server::Impl {
    ServerConfig             cfg;
    std::shared_ptr<Service> service;
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

    auto impl      = std::make_shared<Impl>();
    impl->cfg      = std::move(cfg);
    impl->service  = std::move(service);
    impl->http     = std::move(http);
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
