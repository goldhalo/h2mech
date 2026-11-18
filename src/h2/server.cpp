// =============================================================================
//  h2mech - HTTP/2 服务端（S2：h2c 单流闭环）
// =============================================================================
//  本文件把 nghttp2 的状态机接在 netmech 的 Conn 上。三段式：
//      读：Conn::read → nghttp2_session_mem_recv → 回调里攒请求 → Handler
//      写：Handler 交回响应 → nghttp2_session_mem_send → Conn::write
//      关：Conn 关闭 / 协议错误 → nghttp2_session_del
//
//  ★ 两条铁律（踩过才知道）
//    1. `nghttp2_session_mem_send` 返回的指针**只在下次调用前有效** —— 必须
//       拷一份再交给 Conn::write（我们拷进 shared_ptr<std::string>，顺便当
//       ConstBuf 的 owner）。
//    2. `Conn::write` 是非阻塞 FIFO，返回 false = 这次**没受理**（队列满），
//       必须自己留着下次再发，不能原地重试打转。
// =============================================================================
#include "h2mech/h2/server.hpp"

#if defined(H2MECH_HAS_NGHTTP2)

#include "h2mech/common.hpp"

#include <netmech/conn.hpp>
#include <netmech/iohandler.hpp>
#include <netmech/linkable.hpp>
#include <netmech/mechanism.hpp>
#include <netmech/tls.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace h2mech {
namespace h2 {
namespace {

constexpr std::size_t kReadChunk      = 16 * 1024;
constexpr std::size_t kMaxResponseBody = 16u << 20;

/// 单条连接上"已从 nghttp2 取出、但还没交给 netmech 队列"的字节上限。
/// 见 Session::flush() 的说明：没有它，一次 flush 就能把 nghttp2 里所有
/// 待发帧搬进我们自己的 deque，对端不读时内存照样涨。
constexpr std::size_t kMaxPendingBytes = 256 * 1024;

std::string_view sv(const uint8_t* p, std::size_t n)
{
    return std::string_view{reinterpret_cast<const char*>(p), n};
}

// =============================================================================
//  单条流的状态
// =============================================================================
struct StreamState {
    Request     req;
    HeaderList  raw_headers;      ///< nghttp2 给的原始头（含伪头）
    bool        headers_done = false;
    bool        end_stream   = false;
    bool        dispatched   = false;
    bool        responded    = false;
    bool        body_overflow = false;

    // 响应（整包模型：finish() 时一次性提交）
    int         status = 0;
    HeaderList  resp_headers;
    HeaderList  trailers;
    std::string resp_body;
    std::size_t resp_offset = 0;
    std::size_t buffered    = 0;   ///< 本流已占用的"待发正文"字节（用于连接级配额回收）

    // ---- 流式响应（stream_mode_ == true 时用）----
    std::deque<std::string> out_queue;      ///< 待发正文分片（write() 的产物）
    bool                    resp_submitted = false;   ///< 响应头是否已经提交
    bool                    writer_eof     = false;   ///< 业务已 finish()
    bool                    handler_opened = false;   ///< on_open 是否已回调
};

// =============================================================================
//  Session：一条连接
// =============================================================================
class Session final : public netmech::ConnHandler,
                      public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<Handler> handler, Server::Config cfg)
        : handler_(std::move(handler)), cfg_(std::move(cfg))
    {
    }

    /// 流式会话（bidi / 服务端流 / 客户端流）。
    // 注意初始化顺序必须与成员声明一致（否则 GCC 的 -Wreorder 会当错误）
    Session(std::shared_ptr<StreamHandler> handler, Server::Config cfg)
        : stream_handler_(std::move(handler)), stream_mode_(true), cfg_(std::move(cfg))
    {
    }

    ~Session() override
    {
        if (sess_ != nullptr) {
            nghttp2_session_del(sess_);
            sess_ = nullptr;
        }
        if (cbs_ != nullptr) {
            nghttp2_session_callbacks_del(cbs_);
            cbs_ = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    //  ConnHandler
    // -------------------------------------------------------------------------

    bool on_open(std::shared_ptr<netmech::Conn> ep) override
    {
        conn_ = std::move(ep);
        if (!conn_) {
            return false;
        }
        conn_->set_send_queue_limit(cfg_.max_body_bytes * 2, 256);

        if (!init_session()) {
            return false;
        }

        // 起第一笔读（TCP 的 FIN 只在下一次 read 时才被感知）
        return do_read();
    }

    void on_read(std::error_code ec, netmech::Buf /*buf*/, std::size_t n) override
    {
        if (ec) {
            return;   // 连接即将关闭，on_close 里收尾
        }
        if (n == 0) {
            do_read();
            return;
        }

        const ssize_t rv = nghttp2_session_mem_recv(
            sess_, rx_.data(), static_cast<std::size_t>(n));
        if (rv < 0) {
            // ★ 客户端的连接前言（magic）不对也会走到这里：nghttp2 会返回
            //   NGHTTP2_ERR_BAD_CLIENT_MAGIC —— 所以 h2c 不需要我们手工比对
            //   preface，交给它校验更准（错误码也更细）。
            //
            // ★ 另一个容易误判成"库坏了"的现象：客户端**没先发 SETTINGS**
            //   （RFC 9113 §3.4 要求 magic 之后第一帧必须是 SETTINGS）时，
            //   nghttp2 会把后续 HEADERS 照收不误（mem_recv 返回全量消费），
            //   但**一个回调都不触发** —— 看起来像"帧解析了却没回调"。
            //   排查时先看对端有没有发 SETTINGS，别怀疑自己的回调注册。
            fail_protocol(rv);
            return;
        }

        if (nghttp2_session_want_read(sess_) == 0 && nghttp2_session_want_write(sess_) == 0) {
            close_conn();
            return;
        }

        flush();
        do_read();
    }

    void on_write(std::error_code ec, netmech::ConstBuf /*buf*/, std::size_t /*n*/) override
    {
        if (ec) {
            return;
        }
        // 上一批可能因为队列满而剩下几段：继续泵
        flush();
    }

    /// 由 `Server::stop()` 调用：发 GOAWAY 后关闭（尽力而为）。
    ///
    /// @note 这里不等待在途流收尾：`stop()` 的契约是"尽快停"，
    ///       想优雅排空请在业务层先拒绝新请求、等自己的计数归零再 stop()。
    void shutdown_gracefully()
    {
        if (sess_ != nullptr && conn_ && conn_->is_open()) {
            // last_stream_id = 0：告诉对端"我这边没有已处理完的流"，
            // 对端据此不会再发新流（已在途的由 RST_STREAM/超时收场）。
            nghttp2_submit_goaway(sess_, NGHTTP2_FLAG_NONE, 0, NGHTTP2_NO_ERROR, nullptr, 0);
            flush();
        }
        close_conn();
    }

    void on_close() override
    {
        // ★ 必须在这里断开对 Conn 的强引用：否则 Conn::Impl 与本 handler 互持成环
        conn_.reset();
        streams_.clear();
        pending_out_.clear();
        pending_bytes_       = 0;
        resp_buffered_total_ = 0;
        if (handler_) {
            handler_->on_close();
        }
    }

    // -------------------------------------------------------------------------
    //  Responder 的落点
    // -------------------------------------------------------------------------

    bool resp_send_headers(std::int32_t id, int status, const HeaderList& headers)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->responded) {
            return false;
        }
        st->status       = status;
        st->resp_headers = headers;
        return true;
    }

    bool resp_send_data(std::int32_t id, std::string_view data)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->responded) {
            return false;
        }
        // 两级上限：单条流（kMaxResponseBody）+ 整条连接（配置项）。
        // 后者是防"128 条流各攒 4 MiB"那种按并发数放大的内存占用。
        if (st->resp_body.size() + data.size() > kMaxResponseBody) {
            return false;
        }
        if (resp_buffered_total_ + data.size() > cfg_.max_buffered_response_bytes) {
            return false;
        }
        st->resp_body.append(data.data(), data.size());   // 必须拷贝：调用方的内存随时可能失效
        st->buffered += data.size();
        resp_buffered_total_ += data.size();
        return true;
    }

    bool resp_finish(std::int32_t id, const HeaderList& trailers)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->responded) {
            return false;
        }
        st->trailers  = trailers;
        st->responded = true;

        const HeaderList nv = build_response_headers(
            ResponseHead{st->status, st->resp_headers}, st->resp_body.size(), true);

        std::vector<nghttp2_nv> raw;
        raw.reserve(nv.size());
        for (const Header& h : nv) {
            raw.push_back(make_nv(h.name, h.value));
        }

        nghttp2_data_provider provider{};
        provider.source.ptr = st;
        provider.read_callback = &Session::data_read_cb;

        const int rv = nghttp2_submit_response(sess_, id, raw.data(), raw.size(), &provider);
        if (rv != 0) {
            fail_protocol(static_cast<ssize_t>(rv));
            return false;
        }
        flush();
        return true;
    }

    bool resp_abort(std::int32_t id, Errc code)
    {
        StreamState* st = find(id);
        if (st == nullptr) {
            return false;
        }
        st->responded = true;
        nghttp2_submit_rst_stream(sess_, NGHTTP2_FLAG_NONE, id, to_nghttp2_error(code));
        flush();
        return true;
    }

private:
    // -------------------------------------------------------------------------
    //  nghttp2 初始化与回调
    // -------------------------------------------------------------------------

    bool init_session()
    {
        nghttp2_session_callbacks* cbs = nullptr;
        if (nghttp2_session_callbacks_new(&cbs) != 0) {
            return false;
        }
        nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, &Session::on_begin_headers_cb);
        nghttp2_session_callbacks_set_on_header_callback(cbs, &Session::on_header_cb);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, &Session::on_data_chunk_cb);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, &Session::on_frame_recv_cb);
        nghttp2_session_callbacks_set_on_stream_close_callback(cbs, &Session::on_stream_close_cb);

        const int rv = nghttp2_session_server_new(&sess_, cbs, this);
        // nghttp2 在 session_new 里做的是 `session->callbacks = *callbacks`
        // （按值拷贝），所以这里可以立刻释放。
        cbs_ = cbs;
        if (rv != 0) {
            sess_ = nullptr;
            return false;
        }

        // 通告并发流上限（对端据此决定开多少流）
        std::array<nghttp2_settings_entry, 1> iv{{
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, cfg_.max_concurrent_streams},
        }};
        if (nghttp2_submit_settings(sess_, NGHTTP2_FLAG_NONE, iv.data(), iv.size()) != 0) {
            return false;
        }
        flush();
        return true;
    }

    static nghttp2_nv make_nv(const std::string& name, const std::string& value)
    {
        nghttp2_nv nv{};
        nv.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
        nv.value    = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
        nv.namelen  = name.size();
        nv.valuelen = value.size();
        nv.flags    = NGHTTP2_NV_FLAG_NONE;   // 让 nghttp2 复制，我们不必背生命周期
        return nv;
    }

    static int on_begin_headers_cb(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud)
    {
        auto* self = static_cast<Session*>(ud);
        if (frame->hd.type != NGHTTP2_HEADERS) {
            return 0;   // 目前只关心 HEADERS
        }
        if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            StreamState st;
            self->streams_.emplace(frame->hd.stream_id, std::move(st));
        }
        return 0;
    }

    static int on_header_cb(nghttp2_session* /*s*/, const nghttp2_frame* frame,
                            const uint8_t* name, std::size_t namelen,
                            const uint8_t* value, std::size_t valuelen,
                            uint8_t /*flags*/, void* ud)
    {
        auto* self = static_cast<Session*>(ud);
        StreamState* st = self->find(frame->hd.stream_id);
        if (st == nullptr) {
            return 0;
        }
        if (st->raw_headers.size() >= self->cfg_.max_headers) {
            st->body_overflow = true;   // 复用这个旗标表示"头太多"
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        st->raw_headers.push_back(Header{std::string(sv(name, namelen)),
                                        std::string(sv(value, valuelen))});
        return 0;
    }

    static int on_data_chunk_cb(nghttp2_session* /*s*/, uint8_t /*flags*/, std::int32_t id,
                                const uint8_t* data, std::size_t len, void* ud)
    {
        auto* self = static_cast<Session*>(ud);
        StreamState* st = self->find(id);
        if (st == nullptr) {
            return 0;
        }
        // 流式：**不缓冲**，直接交付给业务（bidi 的地基）。
        // 上限靠 nghttp2 的流控窗口 + netmech 的接收节奏，不在这里截断。
        if (self->stream_mode_) {
            if (self->stream_handler_) {
                StreamResponder out{id, self};
                self->stream_handler_->on_data(sv(data, len), false, out);
            }
            return 0;
        }

        if (st->req.body.size() + len > self->cfg_.max_body_bytes) {
            st->body_overflow = true;
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        st->req.body.append(reinterpret_cast<const char*>(data), len);   // 拷贝：回调期外失效
        return 0;
    }

    static int on_frame_recv_cb(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud)
    {
        auto* self = static_cast<Session*>(ud);

        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            StreamState* st = self->find(frame->hd.stream_id);
            if (st != nullptr) {
                st->headers_done = true;
                if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
                    st->end_stream = true;
                }
            }
        } else if (frame->hd.type == NGHTTP2_DATA) {
            StreamState* st = self->find(frame->hd.stream_id);
            if (st != nullptr && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
                st->end_stream = true;
            }
        }

        const bool end_stream =
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0
            && (frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA);

        // 流式会话：HEADERS 到达就 on_open（不等正文），END_STREAM 再补一次
        // on_data(end=true) —— 这就是"边收边发"的全部机制。
        if (self->stream_mode_) {
            if (frame->hd.type == NGHTTP2_HEADERS
                && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
                self->stream_open(frame->hd.stream_id);
            }
            if (end_stream) {
                self->stream_end(frame->hd.stream_id);
            }
            return 0;
        }

        if (end_stream) {
            self->maybe_dispatch(frame->hd.stream_id);
        }
        return 0;
    }

    static int on_stream_close_cb(nghttp2_session* /*s*/, std::int32_t id, uint32_t /*ec*/,
                                  void* ud)
    {
        auto* self = static_cast<Session*>(ud);
        // 回收这条流占用的"待发正文"配额：否则连接级上限只增不减，
        // 跑一段时间后所有新响应都会被误判成超限。
        auto it = self->streams_.find(id);
        if (it != self->streams_.end()) {
            self->resp_buffered_total_ -= it->second.buffered;
            self->streams_.erase(it);
        }
        return 0;
    }

    static ssize_t data_read_cb(nghttp2_session* s, std::int32_t id, uint8_t* buf,
                                std::size_t length, uint32_t* flags,
                                nghttp2_data_source* source, void* ud)
    {
        auto* st = static_cast<StreamState*>(source->ptr);
        if (st == nullptr) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }

        // ---- 流式：从 out_queue 里取；空了就 DEFERRED，等业务 write() 唤醒 ----
        if (static_cast<Session*>(ud)->stream_mode_) {
            while (!st->out_queue.empty()) {
                std::string&      front = st->out_queue.front();
                const std::size_t avail = front.size() - st->resp_offset;
                if (avail == 0) {
                    st->out_queue.pop_front();
                    st->resp_offset = 0;
                    continue;
                }
                const std::size_t n = std::min(avail, length);
                std::memcpy(buf, front.data() + st->resp_offset, n);
                st->resp_offset += n;
                static_cast<Session*>(ud)->release_buffered(st, n);
                if (st->resp_offset == front.size()) {
                    st->out_queue.pop_front();
                    st->resp_offset = 0;
                }
                if (st->out_queue.empty() && st->writer_eof) {
                    *flags |= NGHTTP2_DATA_FLAG_EOF;
                    if (!st->trailers.empty()) {
                        *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
                        submit_trailers(s, id, st->trailers);
                    }
                }
                return static_cast<ssize_t>(n);
            }

            if (!st->writer_eof) {
                // 业务暂时没有新数据：让 nghttp2 把这条流挂起，
                // 等 write() 里 resume_data() 再继续 —— 这就是流式反压。
                return NGHTTP2_ERR_DEFERRED;
            }
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            if (!st->trailers.empty()) {
                *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
                submit_trailers(s, id, st->trailers);
            }
            return 0;
        }

        const std::size_t left = st->resp_body.size() - st->resp_offset;
        if (left > 0) {
            const std::size_t n = std::min(left, length);
            std::memcpy(buf, st->resp_body.data() + st->resp_offset, n);
            st->resp_offset += n;
            if (st->resp_offset < st->resp_body.size()) {
                return static_cast<ssize_t>(n);
            }
            *flags |= NGHTTP2_DATA_FLAG_EOF;
            if (!st->trailers.empty()) {
                // 有 trailers：DATA 不带 END_STREAM，随后单独发 HEADERS(END_STREAM)
                *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
                submit_trailers(s, id, st->trailers);
            }
            return static_cast<ssize_t>(n);
        }

        *flags |= NGHTTP2_DATA_FLAG_EOF;
        if (!st->trailers.empty()) {
            *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
            submit_trailers(s, id, st->trailers);
        }
        return 0;
    }

    static void submit_trailers(nghttp2_session* s, std::int32_t id, const HeaderList& trailers)
    {
        std::vector<nghttp2_nv> raw;
        raw.reserve(trailers.size());
        for (const Header& h : trailers) {
            raw.push_back(make_nv(h.name, h.value));
        }
        nghttp2_submit_trailer(s, id, raw.data(), raw.size());
    }

    // -------------------------------------------------------------------------
    //  分发与收尾
    // -------------------------------------------------------------------------

public:
    // -------------------------------------------------------------------------
    //  流式：请求侧（StreamResponder / StreamHandler 的落点）
    // -------------------------------------------------------------------------

    /// 请求头到齐 → 回调 `StreamHandler::on_open`（**不等正文**）。
    void stream_open(std::int32_t id)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->handler_opened || !stream_handler_) {
            return;
        }
        if (st->body_overflow) {          // 头太多（复用这个旗标）
            abort_stream(id, Errc::enhance_your_calm);
            return;
        }

        RequestHeadResult parsed = build_request_head(st->raw_headers, cfg_.max_headers);
        if (!parsed.ok()) {
            abort_stream(id, map_headers_status(parsed.status));
            return;
        }

        StreamRequest head;
        head.method    = std::move(parsed.head.method);
        head.url       = std::move(parsed.head.url);
        head.query     = std::move(parsed.head.query);
        head.authority = std::move(parsed.head.authority);
        head.scheme    = std::move(parsed.head.scheme);
        head.headers   = std::move(parsed.head.headers);

        st->handler_opened = true;
        StreamResponder out{id, this};
        stream_handler_->on_open(head, out);
    }

    /// 对端半关闭（END_STREAM）→ 通知业务"不再有请求数据了"。
    void stream_end(std::int32_t id)
    {
        StreamState* st = find(id);
        if (st == nullptr || !st->handler_opened || !stream_handler_) {
            return;
        }
        StreamResponder out{id, this};
        stream_handler_->on_data(std::string_view{}, true, out);
    }

    // -------------------------------------------------------------------------
    //  流式：响应侧
    // -------------------------------------------------------------------------

    bool stream_send_headers(std::int32_t id, int status, const HeaderList& headers)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->resp_submitted || st->writer_eof) {
            return false;
        }
        st->status       = status;
        st->resp_headers = headers;
        return submit_streaming(id, st);
    }

    bool stream_write(std::int32_t id, std::string_view data)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->writer_eof) {
            return false;
        }
        if (!data.empty()) {
            // 连接级内存上限：超出时返回 false，让业务自己决定（等/放弃）。
            if (resp_buffered_total_ + data.size() > cfg_.max_buffered_response_bytes) {
                return false;
            }
            st->buffered += data.size();
            resp_buffered_total_ += data.size();
            st->out_queue.emplace_back(data);
        }
        if (!st->resp_submitted && !submit_streaming(id, st)) {
            return false;
        }
        // 有数据可发了：把之前因 DEFERRED 暂停的流唤醒
        nghttp2_session_resume_data(sess_, id);
        flush();
        return true;
    }

    bool stream_finish(std::int32_t id, const HeaderList& trailers)
    {
        StreamState* st = find(id);
        if (st == nullptr || st->writer_eof) {
            return false;
        }
        st->writer_eof = true;
        st->trailers   = trailers;

        if (!st->resp_submitted) {
            // 从没发过正文/头 ⇒ HEADERS 直接 END_STREAM（Trailers-Only 的落点）
            for (const auto& h : st->trailers) {
                st->resp_headers.push_back(h);
            }
            st->trailers.clear();
            return submit_streaming(id, st);
        }
        nghttp2_session_resume_data(sess_, id);   // 让 provider 看到 EOF
        flush();
        return true;
    }

    /// 提交流式响应头（带 data provider；若此刻已 writer_eof 且无待发正文，
    /// 直接 END_STREAM 结束本次流）。
    bool submit_streaming(std::int32_t id, StreamState* st)
    {
        const bool end_now = st->writer_eof && st->out_queue.empty();

        // 流式响应**不发 content-length**（长度未知；发了反而会要求我们先算总长）
        const HeaderList nv = build_response_headers(
            ResponseHead{st->status == 0 ? 200 : st->status, st->resp_headers}, 0, false);

        std::vector<nghttp2_nv> raw;
        raw.reserve(nv.size());
        for (const Header& h : nv) {
            raw.push_back(make_nv(h.name, h.value));
        }

        int rv = 0;
        if (end_now) {
            rv = nghttp2_submit_response(sess_, id, raw.data(), raw.size(), nullptr);
        } else {
            nghttp2_data_provider provider{};
            provider.source.ptr    = st;
            provider.read_callback = &Session::data_read_cb;
            rv = nghttp2_submit_response(sess_, id, raw.data(), raw.size(), &provider);
        }
        if (rv != 0) {
            fail_protocol(static_cast<ssize_t>(rv));
            return false;
        }
        st->resp_submitted = true;
        flush();
        return true;
    }

private:
    /// 归还一段"待发正文"配额（流式响应在 provider 里消费后调用）。
    void release_buffered(StreamState* st, std::size_t n) noexcept
    {
        if (st->buffered >= n) {
            st->buffered -= n;
        } else {
            st->buffered = 0;
        }
        if (resp_buffered_total_ >= n) {
            resp_buffered_total_ -= n;
        } else {
            resp_buffered_total_ = 0;
        }
    }

    void maybe_dispatch(std::int32_t id)
    {
        StreamState* st = find(id);
        if (st == nullptr || !st->headers_done || !st->end_stream || st->dispatched) {
            return;
        }
        st->dispatched = true;

        if (st->body_overflow) {
            abort_stream(id, Errc::enhance_your_calm);
            return;
        }

        RequestHeadResult parsed = build_request_head(st->raw_headers, cfg_.max_headers);
        if (!parsed.ok()) {
            abort_stream(id, map_headers_status(parsed.status));
            return;
        }

        st->req.method    = std::move(parsed.head.method);
        st->req.url       = std::move(parsed.head.url);
        st->req.query     = std::move(parsed.head.query);
        st->req.authority = std::move(parsed.head.authority);
        st->req.scheme    = std::move(parsed.head.scheme);
        st->req.headers   = std::move(parsed.head.headers);

        if (!handler_) {
            abort_stream(id, Errc::internal_error);
            return;
        }

        Responder out{id, this};
        handler_->on_request(st->req, out);

        // 业务忘了收尾：兜底结束，避免流一直挂着（与 httpmech 的 500 兜底同思路）
        if (!st->responded) {
            StreamState* again = find(id);
            if (again != nullptr) {
                again->status       = 500;
                again->resp_headers = HeaderList{{"content-type", "text/plain"}};
                again->resp_body    = "handler did not respond";
                resp_finish(id, HeaderList{});
            }
        }
    }

    void abort_stream(std::int32_t id, Errc code)
    {
        StreamState* st = find(id);
        if (st != nullptr) {
            st->responded = true;
        }
        nghttp2_submit_rst_stream(sess_, NGHTTP2_FLAG_NONE, id, to_nghttp2_error(code));
        flush();
    }

    static Errc map_headers_status(HeadersStatus s) noexcept
    {
        switch (s) {
        case HeadersStatus::ok:
            return Errc::internal_error;
        case HeadersStatus::too_many_headers:
            return Errc::enhance_your_calm;
        default:
            return Errc::protocol_error;
        }
    }

    static uint32_t to_nghttp2_error(Errc e) noexcept
    {
        switch (e) {
        case Errc::protocol_error:          return NGHTTP2_PROTOCOL_ERROR;
        case Errc::internal_error:          return NGHTTP2_INTERNAL_ERROR;
        case Errc::flow_control_error:      return NGHTTP2_FLOW_CONTROL_ERROR;
        case Errc::stream_closed:           return NGHTTP2_STREAM_CLOSED;
        case Errc::frame_size_error:        return NGHTTP2_FRAME_SIZE_ERROR;
        case Errc::refused_stream:          return NGHTTP2_REFUSED_STREAM;
        case Errc::cancel:                  return NGHTTP2_CANCEL;
        case Errc::compression_error:       return NGHTTP2_COMPRESSION_ERROR;
        case Errc::connect_error:           return NGHTTP2_CONNECT_ERROR;
        case Errc::enhance_your_calm:       return NGHTTP2_ENHANCE_YOUR_CALM;
        case Errc::inadequate_security:     return NGHTTP2_INADEQUATE_SECURITY;
        case Errc::http_1_1_required:       return NGHTTP2_HTTP_1_1_REQUIRED;
        default:                            return NGHTTP2_INTERNAL_ERROR;
        }
    }

    // -------------------------------------------------------------------------
    //  读写泵
    // -------------------------------------------------------------------------

    /// 把 nghttp2 里待发的字节搬给 Conn（队列满就留着，等 on_write 续泵）。
    ///
    /// 反压分两层：
    ///   · `Conn::write` 返回 false = netmech 的发送队列满 → 我们把这段留在
    ///     `pending_out_`，**同时停止继续从 nghttp2 取**；
    ///   · `pending_out_` 自身也有字节上限（kMaxPendingBytes）—— 只要积压超过
    ///     它就不再 mem_send，剩下的帧留在 nghttp2 内部（它自己会管住内存）。
    ///
    /// 为什么必须有第二层：`Conn::write` 一次只收一段，若对端完全不读，
    /// 我们会在一次 flush 里把 nghttp2 里所有待发帧都搬进自己的队列 ——
    /// 那不是反压，只是把内存从 nghttp2 挪到了我们这儿。
    void flush()
    {
        if (sess_ == nullptr || !conn_) {
            return;
        }
        // 1) 先把上一轮没发出去的吐干净
        while (!pending_out_.empty()) {
            const auto& sp = pending_out_.front();
            if (!conn_->write(netmech::ConstBuf{sp, sp->data(), sp->size()})) {
                return;   // 队列满：留着，等 on_write
            }
            pending_bytes_ -= sp->size();
            pending_out_.pop_front();
        }

        // 2) 再从 nghttp2 取，直到积压到上限
        while (pending_bytes_ < kMaxPendingBytes) {
            const uint8_t* data = nullptr;
            const ssize_t  n    = nghttp2_session_mem_send(sess_, &data);
            if (n <= 0) {
                break;
            }
            // ★ 必须拷贝：data 的指针只在下次 mem_send 之前有效
            auto sp = std::make_shared<std::string>(reinterpret_cast<const char*>(data),
                                                    static_cast<std::size_t>(n));
            if (!conn_->write(netmech::ConstBuf{sp, sp->data(), sp->size()})) {
                pending_bytes_ += sp->size();
                pending_out_.push_back(std::move(sp));
                return;
            }
        }
    }

    bool do_read()
    {
        if (!conn_) {
            return false;
        }
        return conn_->read(netmech::Buf{nullptr, rx_.data(), rx_.size()});
    }

    void fail_protocol(ssize_t rv)
    {
        if (handler_) {
            handler_->on_protocol_error(make_error_code(Errc::protocol_error));
        }
        (void)rv;
        close_conn();
    }

    void close_conn()
    {
        if (conn_) {
            conn_->close();
        }
    }

    StreamState* find(std::int32_t id)
    {
        auto it = streams_.find(id);
        return it == streams_.end() ? nullptr : &it->second;
    }

    std::shared_ptr<Handler>       handler_;
    std::shared_ptr<StreamHandler> stream_handler_;
    bool                           stream_mode_ = false;
    Server::Config             cfg_;
    std::shared_ptr<netmech::Conn> conn_;
    nghttp2_session*           sess_ = nullptr;
    nghttp2_session_callbacks* cbs_  = nullptr;   ///< 必须活到 session 之后（见 init_session）

    std::array<uint8_t, kReadChunk>                   rx_{};
    std::unordered_map<std::int32_t, StreamState>     streams_;
    std::deque<std::shared_ptr<std::string>>          pending_out_;
    std::size_t                                       pending_bytes_ = 0;  ///< pending_out_ 的总字节
    std::size_t                                       resp_buffered_total_ = 0;  ///< 所有流的待发正文合计
};

} // namespace

// =============================================================================
//  Responder → Session
// =============================================================================
bool Responder::send_headers(int status, const HeaderList& headers)
{
    return static_cast<Session*>(state_)->resp_send_headers(stream_id_, status, headers);
}

bool Responder::send_data(std::string_view data)
{
    return static_cast<Session*>(state_)->resp_send_data(stream_id_, data);
}

bool Responder::finish(const HeaderList& trailers)
{
    return static_cast<Session*>(state_)->resp_finish(stream_id_, trailers);
}

bool Responder::abort(Errc code)
{
    return static_cast<Session*>(state_)->resp_abort(stream_id_, code);
}

// =============================================================================
//  StreamResponder → Session
// =============================================================================
bool StreamResponder::send_headers(int status, const HeaderList& headers)
{
    return static_cast<Session*>(state_)->stream_send_headers(stream_id_, status, headers);
}

bool StreamResponder::write(std::string_view data)
{
    return static_cast<Session*>(state_)->stream_write(stream_id_, data);
}

bool StreamResponder::finish(const HeaderList& trailers)
{
    return static_cast<Session*>(state_)->stream_finish(stream_id_, trailers);
}

bool StreamResponder::abort(Errc code)
{
    return static_cast<Session*>(state_)->resp_abort(stream_id_, code);
}

// =============================================================================
//  Server
// =============================================================================
namespace {

class ServerLinkable final : public netmech::Linkable {
public:
    explicit ServerLinkable(std::weak_ptr<Server::Impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<netmech::ConnHandler> on_accept(netmech::Conn& c) override;

    void on_accept_failed(const netmech::Address&, std::error_code) override {}

private:
    std::weak_ptr<Server::Impl> impl_;
};

} // namespace

struct Server::Impl : public std::enable_shared_from_this<Server::Impl> {
    std::shared_ptr<netmech::Mechanism> mech;
    Config                              cfg;
    std::shared_ptr<Handler>            handler;
    std::shared_ptr<StreamHandler>      stream_handler;   ///< 与 handler 二选一
    std::shared_ptr<ServerLinkable>     linkable;
    std::atomic<bool>                   running{false};
    netmech::Address                    bound{};
    mutable std::mutex                  mu;
    /// 已建立的会话（弱引用：连接的生死由 netmech 决定）。
    /// stop() 用它给每条连接发 GOAWAY 并关闭，否则 stop 之后连接还在跑。
    std::vector<std::weak_ptr<Session>> sessions;
};

std::shared_ptr<netmech::ConnHandler> ServerLinkable::on_accept(netmech::Conn& c)
{
    auto impl = impl_.lock();
    if (!impl || !impl->running.load()) {
        return nullptr;
    }

    // TLS 场景：ALPN 必须协商出 h2（否则对端会按 h1 说话，这里没有解析器）
    if (impl->cfg.tls && c.alpn_protocol() != "h2") {
        c.close();
        return nullptr;
    }

    auto session = impl->stream_handler
                       ? std::make_shared<Session>(impl->stream_handler, impl->cfg)
                       : std::make_shared<Session>(impl->handler, impl->cfg);
    {
        std::lock_guard<std::mutex> lk(impl->mu);
        // 顺手剪掉已失效的弱引用：否则长跑进程的 sessions 会随着连接数
        // 只增不减（每条已关闭连接留下一个 16 字节的空壳）。
        if (impl->sessions.size() > 64) {
            impl->sessions.erase(
                std::remove_if(impl->sessions.begin(), impl->sessions.end(),
                               [](const std::weak_ptr<Session>& w) { return w.expired(); }),
                impl->sessions.end());
        }
        impl->sessions.push_back(session);
    }
    return session;
}

Server::Server() : m_impl(std::make_shared<Impl>()) {}

Server::~Server() = default;

std::shared_ptr<Server> Server::create(Config cfg, std::shared_ptr<Handler> handler,
                                       std::shared_ptr<netmech::Runtime> runtime)
{
    if (!handler) {
        return nullptr;
    }
    auto mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, std::move(runtime));
    if (!mech) {
        return nullptr;
    }
    return create(std::move(mech), std::move(cfg), std::move(handler));
}

std::shared_ptr<Server> Server::create(std::shared_ptr<netmech::Mechanism> mech,
                                       Config cfg, std::shared_ptr<Handler> handler)
{
    if (!mech || !handler) {
        return nullptr;
    }
    auto srv      = std::shared_ptr<Server>(new Server());
    srv->m_impl->mech    = std::move(mech);
    srv->m_impl->cfg     = std::move(cfg);
    srv->m_impl->handler = std::move(handler);
    return srv;
}

std::shared_ptr<Server> Server::create(Config cfg, std::shared_ptr<StreamHandler> handler,
                                       std::shared_ptr<netmech::Runtime> runtime)
{
    if (!handler) {
        return nullptr;
    }
    auto mech = netmech::Mechanism::create(netmech::Mechanism::Config{}, std::move(runtime));
    if (!mech) {
        return nullptr;
    }
    return create(std::move(mech), std::move(cfg), std::move(handler));
}

std::shared_ptr<Server> Server::create(std::shared_ptr<netmech::Mechanism> mech, Config cfg,
                                       std::shared_ptr<StreamHandler> handler)
{
    if (!mech || !handler) {
        return nullptr;
    }
    auto srv                  = std::shared_ptr<Server>(new Server());
    srv->m_impl->mech         = std::move(mech);
    srv->m_impl->cfg          = std::move(cfg);
    srv->m_impl->stream_handler = std::move(handler);
    return srv;
}

std::error_code Server::start()
{
    if (m_impl->running.load()) {
        return make_error_code(Errc::already_started);
    }

    m_impl->linkable = std::make_shared<ServerLinkable>(m_impl);
    m_impl->running.store(true);

    netmech::ListenOptions opts;
    opts.backlog = m_impl->cfg.backlog;
    if (m_impl->cfg.tls) {
        netmech::TlsConfig tls = *m_impl->cfg.tls;
        tls.alpn               = {"h2"};   // h2 over TLS：只接受 h2
        opts.tls               = std::move(tls);
    }

    const auto ec = m_impl->mech->listen(m_impl->cfg.local, m_impl->linkable, opts);
    if (ec) {
        m_impl->running.store(false);
        m_impl->linkable.reset();
        return ec;
    }

    if (auto actual = m_impl->mech->listening_address(m_impl->cfg.local)) {
        m_impl->bound = *actual;
    } else {
        m_impl->bound = m_impl->cfg.local;
    }
    return std::error_code{};
}

void Server::stop() noexcept
{
    if (!m_impl->running.exchange(false)) {
        return;
    }
    if (m_impl->mech) {
        const netmech::Address addr =
            (m_impl->bound.port() != 0) ? m_impl->bound : m_impl->cfg.local;
        m_impl->mech->unlisten(addr);
    }
    m_impl->linkable.reset();

    // 关掉已经建立的连接：`unlisten` 只停 accept，不动既有连接。
    // 不发这一步的话 stop() 之后这些 h2 连接还会继续收发 —— 与"停服"的
    // 语义不符，也让"重启后端口变了但旧连接还在"这种状态变得无法解释。
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        for (auto& weak : m_impl->sessions) {
            if (auto s = weak.lock()) {
                s->shutdown_gracefully();   // 先发 GOAWAY，再关闭
            }
        }
        m_impl->sessions.clear();
    }

    // 清掉实际绑定地址：否则 `local()` 在 stop 之后还会报上一个端口的旧值，
    // 而端口 0 的监听器重启后拿到的是**新**端口 —— 那是个很难查的"地址说谎"。
    m_impl->bound = netmech::Address{};
}

netmech::Address Server::local() const
{
    return m_impl->bound.port() != 0 ? m_impl->bound : m_impl->cfg.local;
}

bool Server::running() const noexcept
{
    return m_impl->running.load();
}

} // namespace h2
} // namespace h2mech

#endif // H2MECH_HAS_NGHTTP2
