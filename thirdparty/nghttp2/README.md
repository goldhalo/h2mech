# thirdparty/nghttp2 —— vendored HTTP/2 协议引擎（只保留 lib）

| 项 | 值 |
|---|---|
| 上游 | https://github.com/nghttp2/nghttp2 |
| 版本 | **v1.71.0**（2025 年最新稳定 tag） |
| 许可 | MIT（见 `COPYING`） |
| 保留内容 | `lib/*.c`、`lib/*.h`、`includes/nghttp2/{nghttp2.h,nghttp2ver.h}` |
| 丢弃内容 | 上游 CMake / autotools、apps(`nghttp/nghttpd/h2load`)、examples、tests、doc、third-party |

## 为什么不直接用上游 CMake

上游顶层 `CMakeLists.txt` 会探测 libev / c-ares / zlib / systemd / jansson /
libxml2 / jemalloc 等等（我们一个都不用），还会 `add_subdirectory` 八个目录
（整棵树 8 MB）。我们只要一个 `libnghttp2.a`，因此**自己编译 `lib/*.c`**，
平台宏在 `CMakeLists.txt` 顶部有逐条说明。

## 升级步骤（务必照做，不要只换一部分文件）

```bash
VER=1.72.0                      # 目标版本
curl -LO https://github.com/nghttp2/nghttp2/releases/download/v${VER}/nghttp2-${VER}.tar.gz
tar xzf nghttp2-${VER}.tar.gz
cd nghttp2-${VER}

# 覆盖 lib 与公共头（会连带删掉旧文件，避免残留"已删除源文件"的 .h）
rsync -a --delete lib/            <repo>/thirdparty/nghttp2/lib/
rsync -a --delete lib/includes/nghttp2/ <repo>/thirdparty/nghttp2/includes/nghttp2/
cp COPYING <repo>/thirdparty/nghttp2/COPYING
```

然后：

1. 更新本文件表格里的**版本号**；
2. 在改动说明里写清"为什么升级"（安全修复 / 需要新 API）；
3. 全量跑一遍 `h2mech_ut`（h2 是二进制协议，回归靠测试不靠肉眼）。

## 版本纪律

- **钉死版本**：CI 与发布都基于这个 tag。不要"随手拉 master"——
  h2 的帧层改动会直接影响流控与错误码映射。
- **CVE 响应**：nghttp2 的历史 CVE 多在 HPACK / 帧解析。订阅 upstream 的
  security advisory；有高危时优先升级本目录，并跑一遍 `h2mech_ut` +
  `sanitizers` job。
- 本目录**只放库代码**，不放 apps/tests：那些会把仓库撑大，且我们用不到。

## 编译宏说明

见 `CMakeLists.txt` 顶部注释：`BUILDING_NGHTTP2` / `NGHTTP2_STATICLIB` /
`HAVE_ARPA_INET_H` / `HAVE_CLOCK_GETTIME` / `ssize_t=SSIZE_T`（MSVC）等。
其中 `NGHTTP2_STATICLIB` 必须是 **PUBLIC**：否则使用者看到的是
`__declspec(dllimport)`，静态链接时会解析不到符号。
