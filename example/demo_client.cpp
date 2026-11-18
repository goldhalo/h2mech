// =============================================================================
//  demo_client —— 发现对端（Nacos 或直连），调它的 unary 与双向流接口
// =============================================================================
//  用法：
//    demo_client --peer demo-b --nacos 127.0.0.1:8848        # 通过注册中心发现
//    demo_client --peer 127.0.0.1:50052                     # 直连（不需要 Nacos）
// =============================================================================
#include <h2mech/grpc/client.hpp>

#include "nacos_registry.hpp"

#include <netmech/common.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string peer;    ///< 服务名（配 --nacos）或 host:port（直连）
    std::string nacos;        ///< "host:port"，空 = 直连
    std::string nacos_auth;   ///< "user:pass"，空 = 不鉴权
    std::string message = "hello";
    int         rounds  = 3;   ///< 双向流来回几轮

    bool parse(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&](std::string& out) {
                if (i + 1 >= argc) {
                    return false;
                }
                out = argv[++i];
                return true;
            };
            std::string v;
            if (a == "--peer" && next(v)) {
                peer = v;
            } else if (a == "--nacos" && next(v)) {
                nacos = v;
            } else if (a == "--nacos-auth" && next(v)) {
                nacos_auth = v;
            } else if (a == "--message" && next(v)) {
                message = v;
            } else if (a == "--rounds" && next(v)) {
                rounds = std::atoi(v.c_str());
            } else if (a == "--help" || a == "-h") {
                std::cout << "用法: demo_client --peer <服务名|host:port> [--nacos host:port] "
                             "[--message 文本] [--rounds N]\n";
                return false;
            } else {
                std::cerr << "未知参数: " << a << "\n";
                return false;
            }
        }
        return !peer.empty();
    }
};

/// 解析 "host:port"。
bool split_addr(const std::string& s, std::string& host, std::uint16_t& port)
{
    const auto colon = s.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    host = s.substr(0, colon);
    port = static_cast<std::uint16_t>(std::atoi(s.substr(colon + 1).c_str()));
    return !host.empty() && port != 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!opt.parse(argc, argv)) {
        return 1;
    }

    // ---- 1) 找对端地址 ----
    std::string   host;
    std::uint16_t port = 0;

    if (!opt.nacos.empty()) {
        const auto colon = opt.nacos.find(':');
        demo::NacosRegistry registry(
            opt.nacos.substr(0, colon),
            static_cast<std::uint16_t>(
                std::atoi(opt.nacos.substr(colon == std::string::npos ? 0 : colon + 1).c_str())));

        std::string err;
        if (!opt.nacos_auth.empty()) {
            const auto c = opt.nacos_auth.find(':');
            registry.set_credentials(opt.nacos_auth.substr(0, c),
                                     opt.nacos_auth.substr(c == std::string::npos ? 0 : c + 1));
            if (!registry.login(err)) {
                std::cerr << "Nacos 登录失败: " << err << "\n";
                return 1;
            }
        }
        const auto  instances = registry.discover(opt.peer, err);
        if (instances.empty()) {
            std::cerr << "从 Nacos 发现 [" << opt.peer << "] 失败";
            if (!err.empty()) {
                std::cerr << ": " << err;
            }
            std::cerr << "\n提示：若错误里带 501，说明该 Nacos 关闭了 v1 命名 OpenAPI"
                         "（3.x 默认）；换 2.x 镜像或打开 v1 后再试，"
                         "也可以先用 --peer host:port 直连验证调用链。\n";
            return 1;
        }
        host = instances.front().ip;
        port = instances.front().port;
        std::cout << "从 Nacos 发现 [" << opt.peer << "] → " << host << ":" << port << "\n";
    } else {
        if (!split_addr(opt.peer, host, port)) {
            std::cerr << "--peer 需要 host:port（或用 --nacos + 服务名）\n";
            return 1;
        }
        std::cout << "直连对端 " << host << ":" << port << "\n";
    }

    // ---- 2) unary ----
    h2mech::grpc::ClientConfig ccfg;
    ccfg.remote = {host, port};

    auto cli = h2mech::grpc::Client::create(ccfg);
    if (!cli) {
        std::cerr << "创建 client 失败\n";
        return 1;
    }

    const auto r = cli->call("/demo.Echo/Say", opt.message, {}, std::chrono::seconds(5));
    if (!r.ok()) {
        std::cerr << "unary 调用失败: " << h2mech::grpc::code_name(r.status.code) << " "
                  << r.status.message << "\n";
        return 1;
    }
    std::cout << "unary  → " << r.message << "\n";

    // ---- 3) 双向流：同一连接上来回 N 轮 ----
    auto stream = cli->open_bidi("/demo.Echo/Chat");
    if (!stream) {
        std::cerr << "打开双向流失败\n";
        return 1;
    }

    for (int i = 0; i < opt.rounds; ++i) {
        const std::string msg = opt.message + "-" + std::to_string(i);
        if (!stream->send(msg)) {
            std::cerr << "发第 " << i << " 轮失败\n";
            break;
        }
        // 轮询等这一轮的回显（示例代码：真实业务里应当有超时与背压策略）
        std::vector<std::string> got;
        const auto               deadline = std::chrono::steady_clock::now()
                              + std::chrono::seconds(3);
        while (got.empty() && std::chrono::steady_clock::now() < deadline) {
            for (auto& f : stream->take_messages()) {
                got.push_back(f.payload);
            }
            if (got.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (got.empty()) {
            std::cerr << "第 " << i << " 轮没收到回显（3s 超时）\n";
            break;
        }
        std::cout << "bidi   → " << got.front() << "\n";
    }

    stream->close_writes();
    const auto st = stream->wait_close(std::chrono::seconds(5));
    std::cout << "bidi 收尾: " << h2mech::grpc::code_name(st.code) << "\n";

    cli->close();
    netmech::shutdown();
    return st.ok() ? 0 : 1;
}
