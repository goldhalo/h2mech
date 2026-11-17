// =============================================================================
//  gRPC unary 客户端（h2 + 可选 TLS）
// =============================================================================
//  结构：Linkable（拿连接）+ ConnHandler（驱动 io）+ nghttp2 客户端会话。
//  调用线程阻塞在条件变量上，真正的收发都在 netmech 的 io 线程里完成。
//
//  ★ 两把锁，嵌套顺序固定为 **io → state**（绝不反过来）：
//    · io_mu_   ：nghttp2 会话、发送队列、Conn（只有持它才能碰 sess_）；
//    · state_mu_：连接标志、当前调用结果、done。
//    nghttp2 的回调是在 mem_recv（持 io_mu_）里同步调用的，回调里再取
//    state_mu_（io → state ✓）；而 `call()` 只在不持 state_mu_ 时才去拿
//    io_mu_（提交请求、取消流），所以不会出现反向嵌套。
// =============================================================================
#include "h2mech/grpc/client.hpp"

#if defined(H2MECH_HAS_NGHTTP2)

#include "h2mech/grpc/framer.hpp"

#include <netmech/conn.hpp>
#include <netmech/iohandler.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>
#include <netmech/tls.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace h2mech {
namespace grpc {
namespace {

constexpr std::size_t kReadChunk = 16 * 1024;

/// 请求正文的来源（nghttp2 的 provider 只给 `void*`，进度得自己记）。
struct BodySrc {
    std::string data;
    std::size_t offset = 0;
};

nghttp2_nv make_nv(const std::string& name, const std::string& value)
{
    nghttp2_nv nv{};
    nv.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
    nv.value    = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
    nv.namelen  = name.size();
    nv.valuelen = value.size();
    nv.flags    = NGHTTP2_NV_FLAG_NONE;   // 让 nghttp2 复制
    return nv;
}

} // namespace

struct Client::Impl final : public netmech::Linkable,
                            public netmech::ConnHandler,
                            public std::enable_shared_from_this<Client::Impl> {
    Impl(std::shared_ptr<netmech::Mechanism> mech, ClientConfig cfg)
        : mech_(std::move(mech)), cfg_(std::move(cfg))
    {
        reader_.set_max_message_bytes(cfg_.max_message_bytes);
    }

    ~Impl() override
    {
        if (sess_ != nullptr) {
            nghttp2_session_del(sess_);
            sess_ = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    //  Linkable
    // -------------------------------------------------------------------------

    std::shared_ptr<netmech::ConnHandler> on_linkup(netmech::Conn& c) override
    {
        alpn_ = c.alpn_protocol();
        {
            // 连接已建立：netmech 现在自己持有 handler 的强引用，
            // 可以放掉"保活"那一份 —— 否则 Impl 会自我持有成环、永不析构。
            std::lock_guard<std::mutex> lk(state_mu_);
            keepalive_.reset();
        }
        return shared_from_this();
    }

    void on_linkfail(const netmech::Address&, std::error_code ec) override
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        connect_failed_ = true;
        connect_ec_     = ec;
        connecting_     = false;
        if (in_flight_) {
            finish_locked(Code::unavailable, "连接失败: " + ec.message(), false);
        }
        cv_.notify_all();
    }

    void on_linkdown(netmech::Conn&) override
    {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            connected_      = false;
            connect_failed_ = true;
            if (in_flight_) {
                finish_locked(Code::unavailable, "连接已断开", false);
            }
        }
        cv_.notify_all();
    }

    // -------------------------------------------------------------------------
    //  ConnHandler
    // -------------------------------------------------------------------------

    bool on_open(std::shared_ptr<netmech::Conn> ep) override
    {
        {
            std::lock_guard<std::mutex> lk(io_mu_);
            conn_ = std::move(ep);
        }
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            connected_  = true;
            connecting_ = false;
        }
        cv_.notify_all();

        if (!init_session()) {
            return false;
        }
        return do_read();
    }

    void on_read(std::error_code ec, netmech::Buf /*buf*/, std::size_t n) override
    {
        if (ec) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(io_mu_);
            if (sess_ == nullptr) {
                return;
            }
            if (n > 0 && nghttp2_session_mem_recv(sess_, rx_.data(), n) < 0) {
                // 协议错误：这条连接不能再用（nghttp2 的状态机已经不可信）
                close_locked();
                return;
            }
            pump_locked();
            if (nghttp2_session_want_read(sess_) == 0 && nghttp2_session_want_write(sess_) == 0) {
                close_locked();
                return;
            }
        }
        do_read();
    }

    void on_write(std::error_code ec, netmech::ConstBuf /*buf*/, std::size_t /*n*/) override
    {
        if (ec) {
            return;
        }
        std::lock_guard<std::mutex> lk(io_mu_);
        pump_locked();
    }

    void on_close() override
    {
        std::lock_guard<std::mutex> lk(io_mu_);
        conn_.reset();   // 断开与 Conn::Impl 的强引用（否则成环）
        pending_.clear();
        pending_bytes_ = 0;
    }

    // -------------------------------------------------------------------------
    //  对外：一次 unary 调用
    // -------------------------------------------------------------------------

    UnaryResult call(std::string_view method_path, std::string_view request,
                     h2::HeaderList metadata, std::chrono::milliseconds timeout)
    {
        UnaryResult out;
        const auto tmo = (timeout.count() > 0) ? timeout : cfg_.default_timeout;
        if (tmo.count() <= 0) {
            // 仍然要有上限：没有超时的阻塞调用在设备侧等于挂死
            out.status = Status{Code::invalid_argument, "timeout 必须为正"};
            return out;
        }

        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (in_flight_) {
                out.status = Status{Code::internal, "已有调用在进行（本客户端串行）"};
                return out;
            }
            in_flight_       = true;
            done_            = false;
            result_          = UnaryResult{};
            resp_headers_.clear();
            trailers_.clear();
        }

        // 1) 确保连接（首次调用自动建链）
        if (!ensure_connected(tmo)) {
            std::lock_guard<std::mutex> lk(state_mu_);
            const std::string why = connect_ec_ ? connect_ec_.message() : "连接超时";
            out.status  = Status{Code::unavailable, "连接失败: " + why};
            out.reached = false;
            in_flight_  = false;
            return out;
        }

        // 2) 提交请求
        if (!submit(method_path, request, metadata, tmo)) {
            std::lock_guard<std::mutex> lk(state_mu_);
            out.status  = Status{Code::internal, "提交请求失败（连接不可用）"};
            out.reached = false;
            in_flight_  = false;
            return out;
        }

        // 3) 等结果
        {
            std::unique_lock<std::mutex> lk(state_mu_);
            if (!cv_.wait_for(lk, tmo, [this] { return done_; })) {
                lk.unlock();
                cancel_stream();          // 告诉对端别再算了
                lk.lock();
                out.status  = Status{Code::deadline_exceeded,
                                     "调用超时(" + std::to_string(tmo.count()) + "ms)"};
                out.reached = false;
            } else {
                out = result_;
            }
            in_flight_ = false;
            done_      = false;
        }
        return out;
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> lk(io_mu_);
        close_locked();
    }

    bool connected() const noexcept
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        return connected_;
    }

private:
    // -------------------------------------------------------------------------
    //  连接
    // -------------------------------------------------------------------------

    bool ensure_connected(std::chrono::milliseconds tmo)
    {
        bool need_connect = false;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            // 只看标志位：连接真的断了会走 on_linkdown（它把 connected_ 清掉）
            if (connected_) {
                return true;
            }
            if (!connecting_) {
                connecting_      = true;
                connect_failed_  = false;
                connect_ec_.clear();
                need_connect     = true;
            }
        }

        if (need_connect) {
            netmech::ConnectOptions co;
            co.timeout_ms = static_cast<std::uint32_t>(tmo.count());
            if (cfg_.tls) {
                netmech::TlsConfig tls = *cfg_.tls;
                tls.alpn               = {"h2"};   // 本库只谈 h2
                co.tls                 = std::move(tls);
            }

            // 保活：connect 是异步的，返回后不能立刻失去对 linkable 的引用
            std::weak_ptr<netmech::Linkable> lb =
                std::static_pointer_cast<netmech::Linkable>(shared_from_this());
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                keepalive_ = lb.lock();
            }

            const std::error_code ec = mech_->connect(cfg_.remote, lb, co);
            if (ec) {
                std::lock_guard<std::mutex> lk(state_mu_);
                connecting_     = false;
                connect_failed_ = true;
                connect_ec_     = ec;
                keepalive_.reset();
                return false;
            }
        }

        std::unique_lock<std::mutex> lk(state_mu_);
        cv_.wait_for(lk, tmo, [this] { return connected_ || connect_failed_; });
        return connected_;
    }

    bool init_session()
    {
        nghttp2_session_callbacks* cbs = nullptr;
        if (nghttp2_session_callbacks_new(&cbs) != 0) {
            return false;
        }
        nghttp2_session_callbacks_set_on_header_callback(cbs, &Impl::on_header_cb);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, &Impl::on_data_cb);
        nghttp2_session_callbacks_set_on_stream_close_callback(cbs, &Impl::on_stream_close_cb);

        std::lock_guard<std::mutex> lk(io_mu_);
        const int rv = nghttp2_session_client_new(&sess_, cbs, this);
        cbs_         = cbs;
        if (rv != 0) {
            sess_ = nullptr;
            return false;
        }
        // 客户端前言 = magic（nghttp2 自动）+ SETTINGS（必须应用提交）
        if (nghttp2_submit_settings(sess_, NGHTTP2_FLAG_NONE, nullptr, 0) != 0) {
            return false;
        }
        pump_locked();
        return true;
    }

    // -------------------------------------------------------------------------
    //  请求提交
    // -------------------------------------------------------------------------

    bool submit(std::string_view method_path, std::string_view request,
                const h2::HeaderList& metadata, std::chrono::milliseconds tmo)
    {
        std::vector<nghttp2_nv> nva;
        std::vector<std::string> names;   // 必须活到 submit 返回（nv 里是裸指针）
        std::vector<std::string> values;
        names.reserve(8 + metadata.size());
        values.reserve(8 + metadata.size());

        auto add = [&](std::string name, std::string value) {
            names.push_back(std::move(name));
            values.push_back(std::move(value));
            nva.push_back(make_nv(names.back(), values.back()));
        };

        const std::string authority =
            cfg_.authority.empty() ? (cfg_.remote.to_string()) : cfg_.authority;

        add(":method", "POST");
        add(":scheme", cfg_.tls ? "https" : "http");
        add(":path", std::string(method_path));
        add(":authority", authority);
        add("content-type", "application/grpc");
        add("te", "trailers");
        if (const std::string t = encode_timeout(tmo); !t.empty()) {
            add("grpc-timeout", t);
        }
        for (const h2::Header& h : metadata) {
            const std::string n = h2::ascii_lower(h.name);
            // 伪头与逐跳头一律剔除：它们由本层生成，业务塞进来只会造成冲突
            if (n.empty() || h2::is_pseudo_header(n) || h2::is_hop_by_hop(n)
                || n == "content-type") {
                continue;
            }
            add(n, h.value);
        }

        body_src_.data   = encode_message(request);
        body_src_.offset = 0;

        std::lock_guard<std::mutex> lk(io_mu_);
        if (sess_ == nullptr || !conn_) {
            return false;
        }
        reader_.reset();

        nghttp2_data_provider provider{};
        provider.source.ptr    = &body_src_;
        provider.read_callback = &Impl::body_read_cb;

        stream_id_ = nghttp2_submit_request(sess_, nullptr, nva.data(), nva.size(), &provider,
                                            nullptr);
        if (stream_id_ < 0) {
            return false;
        }
        pump_locked();
        return true;
    }

    void cancel_stream()
    {
        std::lock_guard<std::mutex> lk(io_mu_);
        if (sess_ != nullptr && stream_id_ > 0) {
            nghttp2_submit_rst_stream(sess_, NGHTTP2_FLAG_NONE, stream_id_, NGHTTP2_CANCEL);
            pump_locked();
        }
    }

    // -------------------------------------------------------------------------
    //  发送泵
    // -------------------------------------------------------------------------

    void pump_locked()
    {
        if (sess_ == nullptr || !conn_) {
            return;
        }
        while (!pending_.empty()) {
            const auto& sp = pending_.front();
            if (!conn_->write(netmech::ConstBuf{sp, sp->data(), sp->size()})) {
                return;
            }
            pending_bytes_ -= sp->size();
            pending_.pop_front();
        }
        for (;;) {
            const std::uint8_t* data = nullptr;
            const ssize_t        n    = nghttp2_session_mem_send(sess_, &data);
            if (n <= 0) {
                break;
            }
            auto sp = std::make_shared<std::string>(reinterpret_cast<const char*>(data),
                                                    static_cast<std::size_t>(n));
            if (!conn_->write(netmech::ConstBuf{sp, sp->data(), sp->size()})) {
                pending_bytes_ += sp->size();
                pending_.push_back(std::move(sp));
                return;
            }
        }
    }

    bool do_read()
    {
        std::shared_ptr<netmech::Conn> c;
        {
            // conn_ 统一由 io_mu_ 保护（on_open/on_close/close 都在这把锁下改）
            std::lock_guard<std::mutex> lk(io_mu_);
            c = conn_;
        }
        return c && c->read(netmech::Buf{nullptr, rx_.data(), rx_.size()});
    }

    void close_locked()
    {
        if (conn_) {
            conn_->close();
        }
    }

    void finish_locked(Code code, std::string message, bool reached)
    {
        result_.status      = Status{code, std::move(message)};
        result_.reached     = reached;
        result_.http_status = http_status_;
        done_               = true;
    }

    // -------------------------------------------------------------------------
    //  nghttp2 回调（在 mem_recv 内部同步调用，此时持 io_mu_）
    // -------------------------------------------------------------------------

    static int on_header_cb(nghttp2_session*, const nghttp2_frame* frame, const std::uint8_t* name,
                            std::size_t namelen, const std::uint8_t* value, std::size_t valuelen,
                            std::uint8_t, void* ud)
    {
        auto* self = static_cast<Impl*>(ud);
        const std::string n(reinterpret_cast<const char*>(name), namelen);
        const std::string v(reinterpret_cast<const char*>(value), valuelen);

        std::lock_guard<std::mutex> lk(self->state_mu_);
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            self->resp_headers_.push_back(h2::Header{n, v});
            if (n == ":status") {
                self->http_status_ = std::atoi(v.c_str());
            }
        } else if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
            self->trailers_.push_back(h2::Header{n, v});
        }
        return 0;
    }

    static int on_data_cb(nghttp2_session*, std::uint8_t, std::int32_t, const std::uint8_t* data,
                          std::size_t len, void* ud)
    {
        auto* self = static_cast<Impl*>(ud);
        if (!self->reader_.feed(data, len)) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;   // 帧格式错误 → 本流失败
        }
        Frame f;
        while (self->reader_.next(f)) {
            std::lock_guard<std::mutex> lk(self->state_mu_);
            self->result_.message += f.payload;   // 目前只支持"一条消息"
        }
        return 0;
    }

    static int on_stream_close_cb(nghttp2_session*, std::int32_t, std::uint32_t /*ec*/, void* ud)
    {
        auto* self = static_cast<Impl*>(ud);
        {
            std::lock_guard<std::mutex> lk(self->state_mu_);
            if (!self->done_) {
                self->result_.status = parse_status(self->resp_headers_, self->trailers_);
                self->result_.reached = true;
                self->result_.http_status = self->http_status_;
                self->done_ = true;
            }
        }
        self->cv_.notify_all();
        return 0;
    }

    static ssize_t body_read_cb(nghttp2_session*, std::int32_t, std::uint8_t* buf,
                                std::size_t length, std::uint32_t* flags,
                                nghttp2_data_source* source, void* /*ud*/)
    {
        // ★ nghttp2_data_source 只有 {int fd; void* ptr;} —— **没有偏移字段**，
        //   进度必须自己记（以为它有 offset 是很自然的误解，编译期不会报）。
        auto* body = static_cast<BodySrc*>(source->ptr);
        if (body == nullptr) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        const std::size_t left = body->data.size() - body->offset;
        if (left == 0) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        const std::size_t n = (left < length) ? left : length;
        std::memcpy(buf, body->data.data() + body->offset, n);
        body->offset += n;
        if (body->offset >= body->data.size()) {
            *flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(n);
    }

    std::shared_ptr<netmech::Mechanism> mech_;
    ClientConfig                        cfg_;

    // 连接状态
    std::shared_ptr<netmech::Conn> conn_;
    std::shared_ptr<netmech::Linkable> keepalive_;
    std::string                        alpn_;

    // nghttp2（只有持 io_mu_ 时碰）
    mutable std::mutex                                io_mu_;
    nghttp2_session*                                  sess_ = nullptr;
    nghttp2_session_callbacks*                        cbs_  = nullptr;
    std::array<std::uint8_t, kReadChunk>              rx_{};
    std::deque<std::shared_ptr<std::string>>          pending_;
    std::size_t                                       pending_bytes_ = 0;
    BodySrc                                           body_src_;
    std::int32_t                                      stream_id_ = -1;
    MessageReader                                     reader_;

    // 调用状态
    mutable std::mutex      state_mu_;
    std::condition_variable cv_;
    bool                    in_flight_      = false;
    bool                    done_           = false;
    bool                    connected_      = false;
    bool                    connecting_     = false;
    bool                    connect_failed_ = false;
    std::error_code         connect_ec_;
    UnaryResult             result_{};
    h2::HeaderList          resp_headers_;
    h2::HeaderList          trailers_;
    int                     http_status_    = 0;
};

// =============================================================================
//  Client
// =============================================================================

Client::Client(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Client::~Client() = default;

std::shared_ptr<Client> Client::create(ClientConfig cfg, std::shared_ptr<netmech::Runtime> runtime)
{
    auto mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, std::move(runtime));
    if (!mech) {
        return nullptr;
    }
    return create(std::move(mech), std::move(cfg));
}

std::shared_ptr<Client> Client::create(std::shared_ptr<netmech::Mechanism> mech, ClientConfig cfg)
{
    if (!mech) {
        return nullptr;
    }
    return std::shared_ptr<Client>(
        new Client(std::make_shared<Impl>(std::move(mech), std::move(cfg))));
}

UnaryResult Client::call(std::string_view method_path, std::string_view request,
                         h2::HeaderList metadata, std::chrono::milliseconds timeout)
{
    if (!m_impl) {
        UnaryResult r;
        r.status = Status{Code::internal, "client 未初始化"};
        return r;
    }
    return m_impl->call(method_path, request, std::move(metadata), timeout);
}

void Client::close() noexcept
{
    if (m_impl) {
        m_impl->close();
    }
}

bool Client::connected() const noexcept
{
    return m_impl && m_impl->connected();
}

} // namespace grpc
} // namespace h2mech

#endif // H2MECH_HAS_NGHTTP2
