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
| S5 | gRPC wire 客户端（帧、`grpc-status`、deadline） | ⏳ 下一步 |
| S6 | gRPC 服务端（方法注册、trailers、Trailers-Only） | ⏳ |
| S7 | adapter/httpmech + 同端口 h1/h2c preface 嗅探（可选） | ⏳ |

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
