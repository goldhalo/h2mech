// =============================================================================
//  example/nacos_registry.hpp —— 极简 Nacos 注册/发现（HTTP OpenAPI）
// =============================================================================
//  为什么用 HTTP OpenAPI 而不是 Nacos 的 gRPC 通道：
//    Nacos 2.x 的 gRPC 通道是**它私有的协议**（自带 protobuf 定义 + BiRequestStream
//    握手），不是标准 gRPC 服务；本库刻意"不做 protobuf"，硬接等于把 Nacos 的
//    proto 抄进来。而注册/发现本身只需要两个 HTTP 调用，Go/Python/C++ 的社区
//    客户端走的都是这条路。
//
//  ★ 这里刻意**不引第三方依赖**（不用 httpmech，也不用 JSON 库）：
//    · 只要三件事：POST 注册、DELETE 注销、GET 查询；
//    · 响应体只认 `"ip":"..."` 和 `"port":...` 两个字段，不做通用 JSON 解析。
//    这是示例代码，宁可短、可读，也不为了"优雅"再拉一个依赖。
//
//  ★ 若你的 Nacos 是 3.x 且关闭了 v1 OpenAPI（会返回 501），请用
//    --peer host:port 直连模式跑示例，或把 Nacos 换成开启 v1 兼容的版本。
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "ws2_32.lib")
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace demo {

/// 一台被发现的实例。
struct Instance {
    std::string   ip;
    std::uint16_t port = 0;
    bool          healthy = true;
};

/// 阻塞式 HTTP/1.1 小客户端（只够调 Nacos 的三个接口）。
class TinyHttp {
public:
    ~TinyHttp() { close(); }

    /// 发一个请求，返回响应体（失败返回空串并把原因写进 `err`）。
    std::string request(const std::string& host, std::uint16_t port, const std::string& method,
                        const std::string& target, const std::string& body, std::string& err)
    {
        if (!connect_to(host, port, err)) {
            return {};
        }

        std::string req = method + " " + target + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        req += "Connection: close\r\n";
        req += "User-Agent: h2mech-demo/1.0\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/x-www-form-urlencoded\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        req += body;

        if (!write_all(req)) {
            err = "写请求失败";
            return {};
        }

        std::string resp;
        char        buf[4096];
        for (;;) {
            const int n = static_cast<int>(::recv(fd_, buf, sizeof(buf), 0));
            if (n <= 0) {
                break;
            }
            resp.append(buf, static_cast<std::size_t>(n));
        }
        close();

        const auto pos = resp.find("\r\n\r\n");
        if (pos == std::string::npos) {
            err = "响应格式错误";
            return {};
        }
        const std::string head = resp.substr(0, pos);
        const std::string b    = resp.substr(pos + 4);

        // 状态码必须从**首行**解析。
        // ★ 不要写成 `head.find(" 2")`：响应头里的 `Date: ... Nov 2026` 就带 " 2"，
        //   于是 501 也会被判成成功 —— 表现是"注册成功"的日志配着实际 501，
        //   比直接失败更难查（这个坑真踩过，`demo_server` 一度谎报注册成功）。
        int code = 0;
        const auto sp = head.find(' ');
        if (sp != std::string::npos) {
            code = std::atoi(head.c_str() + sp + 1);
        }
        if (code < 200 || code >= 300) {
            err = head.substr(0, head.find("\r\n"));
            if (b.size() > 2) {
                err += " | " + b.substr(0, 200);
            }
            return {};
        }
        return b;
    }

private:
    bool connect_to(const std::string& host, std::uint16_t port, std::string& err)
    {
        close();
#if defined(_WIN32)
        static bool once = [] {
            WSADATA d{};
            WSAStartup(MAKEWORD(2, 2), &d);
            return true;
        }();
        (void)once;
#endif
        fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd_ < 0) {
            err = "socket 创建失败";
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        // 用 inet_pton 而不是 inet_addr：后者在 Windows 上是被弃用的 API
        // （示例也受 -Werror / /WX 约束，用一次就编不过）。
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            err = "连接 " + host + ":" + std::to_string(port) + " 失败";
            close();
            return false;
        }
#if defined(_WIN32)
        DWORD tv = 3000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = 3;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        return true;
    }

    bool write_all(const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size()) {
            const int n = static_cast<int>(
                ::send(fd_, s.data() + off, static_cast<int>(s.size() - off), 0));
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    void close()
    {
        if (fd_ >= 0) {
#if defined(_WIN32)
            ::closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }
    }

    int fd_ = -1;
};

/// Nacos 注册中心客户端（v1 OpenAPI）。
class NacosRegistry {
public:
    NacosRegistry(std::string host, std::uint16_t port, std::string group = "DEFAULT_GROUP",
                  std::string ns = "public")
        : host_(std::move(host)), port_(port), group_(std::move(group)), ns_(std::move(ns))
    {
    }

    /// 设置账号密码：开了鉴权的 Nacos 要先登录换 `accessToken`。
    void set_credentials(std::string user, std::string password)
    {
        user_     = std::move(user);
        password_ = std::move(password);
    }

    /// 登录一次，缓存 accessToken（Nacos 默认 30 分钟有效，示例不做过期续期）。
    bool login(std::string& err)
    {
        if (user_.empty()) {
            return true;   // 没配账号 = 不鉴权
        }
        const std::string body =
            "username=" + user_ + "&password=" + password_;
        const std::string resp = http_.request(host_, port_, "POST", "/nacos/v1/auth/login", body, err);
        if (resp.empty()) {
            return false;
        }
        const std::string key = "\"accessToken\":\"";
        const auto        p   = resp.find(key);
        if (p == std::string::npos) {
            err = "登录响应里没有 accessToken";
            return false;
        }
        const auto start = p + key.size();
        const auto end   = resp.find('"', start);
        if (end == std::string::npos) {
            err = "accessToken 解析失败";
            return false;
        }
        token_ = resp.substr(start, end - start);
        return true;
    }

    /// 注册实例（`POST /nacos/v1/ns/instance`）。
    bool register_service(const std::string& service, const std::string& ip, std::uint16_t port,
                          std::string& err)
    {
        const std::string body = "serviceName=" + service + "&groupName=" + group_ +
                                 "&namespaceId=" + ns_ + "&ip=" + ip +
                                 "&port=" + std::to_string(port) + "&weight=1&enabled=true" +
                                 "&healthy=true&ephemeral=true" + auth_query();
        const std::string resp = http_.request(host_, port_, "POST", "/nacos/v1/ns/instance", body, err);
        return resp.find("ok") != std::string::npos || err.empty();
    }

    /// 注销实例。
    bool deregister_service(const std::string& service, const std::string& ip,
                            std::uint16_t port, std::string& err)
    {
        const std::string target = "/nacos/v1/ns/instance?serviceName=" + service +
                                   "&groupName=" + group_ + "&namespaceId=" + ns_ + "&ip=" + ip +
                                   "&port=" + std::to_string(port) + "&ephemeral=true" +
                                   auth_query();
        const std::string resp = http_.request(host_, port_, "DELETE", target, std::string{}, err);
        return resp.find("ok") != std::string::npos || err.empty();
    }

    /// 查健康实例（`GET /nacos/v1/ns/instance/list`）。
    std::vector<Instance> discover(const std::string& service, std::string& err)
    {
        const std::string target = "/nacos/v1/ns/instance/list?serviceName=" + service +
                                   "&groupName=" + group_ + "&namespaceId=" + ns_ +
                                   "&healthyOnly=true" + auth_query();
        const std::string body = http_.request(host_, port_, "GET", target, std::string{}, err);
        return parse_hosts(body);
    }

    /// 从 `{"hosts":[{"ip":"1.2.3.4","port":8080,...}]}` 里抠出 ip/port。
    ///
    /// 只认这两个字段、按 `"ip":"` 切段 —— 不是通用 JSON 解析，够用且没有依赖。
    static std::vector<Instance> parse_hosts(const std::string& body)
    {
        std::vector<Instance> out;
        const std::string     key = "\"ip\":\"";
        std::size_t           pos = 0;
        while ((pos = body.find(key, pos)) != std::string::npos) {
            pos += key.size();
            const std::size_t end = body.find('"', pos);
            if (end == std::string::npos) {
                break;
            }
            Instance inst;
            inst.ip = body.substr(pos, end - pos);

            const std::size_t pkey = body.find("\"port\":", end);
            if (pkey != std::string::npos) {
                std::size_t p = pkey + 7;
                while (p < body.size() && (body[p] == ' ' || body[p] == '"')) {
                    ++p;
                }
                std::uint32_t v = 0;
                while (p < body.size() && body[p] >= '0' && body[p] <= '9') {
                    v = v * 10 + static_cast<std::uint32_t>(body[p] - '0');
                    ++p;
                }
                inst.port = static_cast<std::uint16_t>(v);
            }
            if (!inst.ip.empty() && inst.port != 0) {
                out.push_back(std::move(inst));
            }
            pos = end;
        }
        return out;
    }

private:
    /// 鉴权参数（没登录过就是空串）。
    [[nodiscard]] std::string auth_query() const
    {
        return token_.empty() ? std::string{} : "&accessToken=" + token_;
    }

    std::string    host_;
    std::uint16_t  port_;
    std::string    group_;
    std::string    ns_;
    std::string    user_;
    std::string    password_;
    std::string    token_;
    TinyHttp       http_;
};

} // namespace demo
