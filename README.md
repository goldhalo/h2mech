# h2mech

**HTTP/2 与 gRPC 的协议层**，基于 [netmech](../netmech) 的 TCP / TLS / ALPN。

与 [httpmech](../httpmech) 的分工：**httpmech 冻结在 HTTP/1.1**（含 HTTPS/WSS），
本库只做 h2 与 gRPC。两个库互不依赖 —— 想让 h2 请求复用 httpmech 的路由表，
用可选编译的 `adapter/httpmech`（默认不编，不产生依赖）。

## 依赖边界

| 依赖 | 用途 | 说明 |
|---|---|---|
| **netmech** | TCP / UDP / TLS / ALPN | 唯一的传输层；h2mech 不碰 socket 与 OpenSSL |
| **nghttp2** | 帧层 / HPACK / 流控 / 状态机 | MIT，C；随仓库以源码形式提供（第三阶段引入） |
| ~~Boost / beast~~ | — | **不引** |
| ~~protobuf~~ | — | **不引**：gRPC 只做 wire 层，proto 由应用侧生成 |

## 与 netmech / httpmech 的一致性约定

- 公共头**不引第三方**（asio / OpenSSL / nghttp2 一律不出现在 `include/`）；
- `-Werror` / `/WX` **只作用于库与示例**，`test/` 不受约束（用例能过即可）；
- TLS 由 netmech 提供，OpenSSL 由构建方自备（`-DNETMECH_TLS=ON`）；
- 1 io_context = 1 线程，回调天然串行，**不需要 strand**。

## 阶段与当前状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| S0 | 仓库骨架 + 错误码 + 最小 CI | ✅ 完成 |
| S1 | h2 语义转换层（`h2/headers.hpp`：伪头/逐跳头/path/authority/content-length） | ✅ 完成 |
| S2 | h2c 服务端闭环（nghttp2 驱动 + 读写泵 + 正文回环 + trailers + stop 关连接） | ✅ 完成（24 用例 / 177 断言，gcc+clang+MSVC，零告警） |
| S3 | 多流并发与生命周期（并发流隔离、RST、连接级内存配额、写反压） | ✅ 完成（27 用例 / 237 断言） |
| S4 | h2 over TLS + ALPN（服务端强制 `h2`、非 h2 直接拒） | ✅ 完成（29 用例 / 255 断言，含真握手） |
| S5 | gRPC wire 层 + unary 客户端（帧、`grpc-status`/trailers、`grpc-timeout`） | ✅ 完成（42 用例 / 783 断言） |
| S6 | gRPC 服务端（方法注册、trailers、Trailers-Only） | ✅ 完成（48 用例 / 811 断言） |
| S7 | 同端口 h1/h2c preface 嗅探 | ✅ 完成（纯函数 + 6 用例） |
| S7b | `adapter/httpmech`（h2/gRPC 挂到 httpmech 路由） | 🔶 未做（可选 target，等你评审完再定） |

## 构建

```bash
# 依赖同级目录的 ../netmech（或 -DH2MECH_NETMECH_DIR=<路径>）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

启用 TLS（h2 over TLS 需要）：

```bash
cmake -S . -B build -G Ninja -DNETMECH_TLS=ON -DOPENSSL_ROOT_DIR=<OpenSSL 前缀>
```

## 目录

```
include/h2mech/
  common.hpp        错误码（库 / RFC 9113 §7 / gRPC 三段）
  h2/headers.hpp    h1 ⇄ h2 头字段语义转换（纯函数，可单测）
  h2/server.hpp     HTTP/2 服务端（Server / Handler / Responder / Request）
src/h2/headers.cpp  头字段规则的实现
src/h2/server.cpp   会话层：nghttp2 驱动、流表、读写泵、trailers
test/               自带极简框架（H2MECH_TEST 自动注册）
thirdparty/         nghttp2 源码（v1.71.0，只编 lib/）
adapter/httpmech/   可选：h2 请求 → httpmech::HttpRequest（默认不编）
```

## h2 over TLS（S4）

TLS 由 netmech 提供，h2mech 只做两件事：把 `alpn` 固定成 `h2`，以及在
`on_accept` 里拒绝没协商出 `h2` 的连接（否则会把 h1 的字节喂给 h2 状态机）。

```cpp
#include <netmech/tls.hpp>

auto tls = std::make_shared<netmech::TlsConfig>();
tls->cert_chain_file  = "server.crt";
tls->private_key_file = "server.key";
// 不需要显式配 alpn：Server::start() 会强制成 {"h2"}

h2mech::h2::Server::Config cfg;
cfg.local = {"0.0.0.0", 8443};
cfg.tls   = tls;                       // ← 只加这一行
```

客户端（netmech）：`TlsConfig::pinned_sha256` 或 `ca_file` + `alpn = {"h2"}`。
`test/test_h2_tls.cpp` 里就是**真握手**：内嵌的自签证书 + 指纹 pin，
通过 `Conn::alpn_protocol()` 断言协商结果是 `h2`，并跑通一条请求回环；
另一条用例用只谈 `http/1.1` 的客户端，断言服务端直接关闭、handler 不被调用。

## 线程模型与锁（评审时看这一节）

**服务端：一把锁都没有。** netmech 的约定是"一个连接 = 一个 io 线程，回调严格串行"，
所以：

- `h2::Server` 的每个 `Session` 只被它自己的 io 线程碰：流表、读写泵、
  nghttp2 会话全都不加锁；
- `grpc::Server` 的方法表在 `start()` **之前**注册完，之后只读（读不需要锁）；
- 唯一的跨线程共享是 `h2::Server` 里的"会话弱引用表"（`stop()` 要从调用线程
  逐个发 GOAWAY），它由一把 `std::mutex` 保护，作用域只有"push_back / 遍历"。

**客户端：两把锁，嵌套顺序固定 `io_mu_ → state_mu_`。**

| 锁 | 保护什么 | 谁拿 |
|---|---|---|
| `io_mu_` | nghttp2 会话、发送队列、`Conn` | io 线程（回调）、调用线程（提交请求 / 取消流） |
| `state_mu_` | 连接标志、本次调用结果、`done` | io 线程（回调里写）、调用线程（等待 / 读结果） |

为什么必须两把：`nghttp2` 的回调是在 `mem_recv`（持 `io_mu_`）内部**同步**调用的，
回调里要写结果就得再拿 `state_mu_`（io → state）。而调用线程从不在持
`state_mu_` 时去拿 `io_mu_`（提交/取消都先解锁），所以不会反向嵌套、不会死锁。

**为什么客户端不干脆"全部丢给 io 线程"**：`call()` 要阻塞等结果，就得有一个
让调用线程等待的同步点（条件变量 + `state_mu_`）；如果连 nghttp2 会话也只让
io 线程碰，调用线程就必须"投递任务"给 io 线程，而 `Conn` 没有公开的 post 接口
（也不该为这个加）。两把锁、顺序固定，是目前最省事且可证明不会死锁的写法。

## gRPC unary 客户端（S5）

```cpp
#include <h2mech/grpc/client.hpp>

h2mech::grpc::ClientConfig cfg;
cfg.remote = {"10.0.0.5", 50051};
// cfg.tls = tls;                 // 需要 TLS 就配上（ALPN 自动固定为 h2）

auto cli = h2mech::grpc::Client::create(cfg);

// 请求/响应都是**已序列化**的字节：proto 由应用侧 protoc 生成，库不碰
const auto r = cli->call("/helloworld.Greeter/SayHello", request_bytes);
if (r.ok()) {
    /* r.message 是响应消息字节 */
} else {
    /* r.status.code / r.status.message 是 gRPC 状态（UNAVAILABLE / NOT_FOUND …） */
}
```

三个必须知道的语义：

1. **HTTP 200 ≠ 调用成功**。gRPC 的结果在 trailers 的 `grpc-status` 里；`r.http_status`
   只是观测值。缺 `grpc-status` 时本库判 `UNKNOWN`（**不**判成功）。
2. **串行**：一个 `Client` 同时只跑一个调用（设备对平台的链路足够），
   第二个并发调用立刻返回 `INTERNAL` + 明确说明。
3. **超时**：`call()` 的 timeout 会同时用于本地等待和发给对端的 `grpc-timeout`
   （毫秒精度尽量保留，不做"1500ms 变 2s"的取整）。

wire 层本身（`h2mech/grpc/framer.hpp`）也可单独用：`MessageReader` 是增量解析器，
5 字节前缀被 TCP 切在任何位置都能拼回来，并对长度前缀设了上限（默认 4 MiB），
避免对端用一个 `FF FF FF FF` 让我们预分配 4 GiB。

## gRPC 服务端（S6）

```cpp
#include <h2mech/grpc/server.hpp>

auto service = std::make_shared<h2mech::grpc::Service>();
service->add_unary("/helloworld.Greeter/SayHello",
                   [](h2mech::grpc::ServerCall& call, const h2mech::grpc::Frame& req) {
                       call.send(do_something(req.payload));      // 响应消息字节
                       call.finish(h2mech::grpc::Status{});       // grpc-status: 0
                   });

h2mech::grpc::ServerConfig cfg;
cfg.http.local = {"0.0.0.0", 50051};
cfg.http.tls   = tls;            // 要 TLS 就配（ALPN 由 h2::Server 固定成 h2）

auto srv = h2mech::grpc::Server::create(cfg, service);
srv->start();
```

三条语义：

1. **Trailers-Only**：处理函数**没有** `send()` 就 `finish(错误状态)` 时，`grpc-status`
   直接进响应头（HEADERS 带 END_STREAM），不发 DATA、也不单发 trailers ——
   这是规范要求的错误答复形态，写成"空 DATA + trailers"会让部分客户端报 UNKNOWN。
2. **未注册的方法**自动回 `UNIMPLEMENTED`；请求不是 POST/`application/grpc` 时
   回 HTTP 405/415（对端根本不是 gRPC 客户端，给它 grpc-status 只会更困惑）。
3. **处理函数抛异常**会被捕获并转成 `INTERNAL`；忘记 `finish()` 由框架兜底
   —— 异常绝不允许穿透到 io 线程（那会 terminate 整个进程）。

## 同端口 h1 / h2c（S7）

`h2/sniff.hpp` 只提供一个**纯函数**：拿连接最前面的字节判协议。

```cpp
const auto hint = h2mech::h2::sniff_protocol(peeked);   // peeked 可以不足 24 字节
switch (hint) {
case h2mech::h2::ProtocolHint::need_more: /* 继续读 */ break;
case h2mech::h2::ProtocolHint::h2c:       h2_session.feed(peeked); break;
case h2mech::h2::ProtocolHint::http1:     h1_parser.feed(peeked);  break;
}
```

三个容易写错的地方：

1. **必须把 `peeked` 还给选中的处理器**（`feed(peeked)`）—— 嗅探读过的字节
   不会再从内核来一次；漏掉就是"h1 正常、h2 一上来就协议错误"。
2. **判定是逐字节最短路径**：`G`（GET 的首字节）一进来就能判 http1，不必为每个
   h1 连接攒满 24 字节。
3. **本库不替你做分流**：netmech 已经把连接交给了某个 handler，谁嗅探谁 re-feed。

`adapter/httpmech`（把 h2 请求直接投进 `httpmech::Router`）**没有做**：它会把两个
库的 CI/发版绑在一起，属于"可选 target"，等评审后按需再加。

### 一个值得记住的坑：客户端必须**自己**发 SETTINGS

nghttp2 的客户端会话只会自动发连接前言里的 magic，**SETTINGS 帧要应用自己
`nghttp2_submit_settings()`**（RFC 9113 §3.4 要求它是 magic 之后的第一帧）。
漏掉它时现象极具误导性：服务端 `nghttp2_session_mem_recv` 照收不误、返回全量
消费，但**一个回调都不触发** —— 看起来像"回调没注册"或"库有 bug"，
实际上是服务端在等对端的 SETTINGS。`test/test_h2_server.cpp` 的
`do_request()` 与 `src/h2/server.cpp` 的 `on_read()` 都有注释标注。

测试按主题分文件，用例用 `H2MECH_TEST(name) { ... }` 自动注册：

```bash
./build/bin/h2mech_ut              # 全部
./build/bin/h2mech_ut headers      # 只跑名字含 headers 的
```
