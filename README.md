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
| S1 | h2 语义转换层（伪头/逐跳头/path/authority/content-length，纯函数） | ⏳ 下一步 |
| S2 | h2c 服务端单流闭环（preface 校验 + nghttp2 驱动 + 读写泵） | ⏳ |
| S3 | 多流与生命周期（流表、RST/GOAWAY/SETTINGS、块池、反压） | ⏳ |
| S4 | TLS + ALPN（`{"h2","http/1.1"}`，自签 + 指纹 pin） | ⏳ |
| S5 | gRPC wire 客户端（帧、`grpc-status`、deadline） | ⏳ |
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
include/h2mech/     公共头（common.hpp 错误码 / export.hpp / 伞头）
src/                实现
test/               自带极简框架的单元测试
thirdparty/         nghttp2 源码（S2 引入）
adapter/httpmech/   可选：h2 请求 → httpmech::HttpRequest（默认不编）
```
