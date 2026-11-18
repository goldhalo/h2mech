// =============================================================================
//  demo_server —— 一个 gRPC 服务（unary + 双向流），可选注册到 Nacos
// =============================================================================
//  用法：
//    demo_server --service demo-a --port 50051
//    demo_server --service demo-a --port 50051 --nacos 127.0.0.1:8848
//
//  提供的接口：
//    /demo.Echo/Say   unary   —— 回一句 "<服务名> 收到: <请求字节>"
//    /demo.Echo/Chat  双向流   —— 收到一条回一条（原样回显）
// =============================================================================
#include <h2mech/grpc/server.hpp>

#include "nacos_registry.hpp"

#include <netmech/common.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string service = "demo-a";
    std::string bind_ip = "127.0.0.1";
    std::uint16_t port  = 50051;
    std::string nacos;    ///< "host:port"，空 = 不注册
    int         run_for = 0;   ///< >0 = 跑这么多秒后退出（0 = 等回车）

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
            if (a == "--service" && next(v)) {
                service = v;
            } else if (a == "--port" && next(v)) {
                port = static_cast<std::uint16_t>(std::atoi(v.c_str()));
            } else if (a == "--nacos" && next(v)) {
                nacos = v;
            } else if (a == "--run-for" && next(v)) {
                run_for = std::atoi(v.c_str());
            } else if (a == "--ip" && next(v)) {
                bind_ip = v;
            } else if (a == "--help" || a == "-h") {
                std::cout << "用法: demo_server --service <名> --port <端口> "
                             "[--nacos host:port] [--ip 绑定地址]\n";
                return false;
            } else {
                std::cerr << "未知参数: " << a << "\n";
                return false;
            }
        }
        return true;
    }
};

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!opt.parse(argc, argv)) {
        return 1;
    }

    auto service = std::make_shared<h2mech::grpc::Service>();

    // unary：回一句带服务名的文本
    const std::string name = opt.service;
    service->add_unary("/demo.Echo/Say",
                       [name](h2mech::grpc::ServerCall& call, const h2mech::grpc::Frame& req) {
                           call.send(name + " 收到: " + req.payload);
                           call.finish(h2mech::grpc::Status{});
                       });

    // 双向流：收到一条回一条
    service->add_bidi_stream("/demo.Echo/Chat",
                             [](h2mech::grpc::BidiCall& call, const h2mech::grpc::Frame& req) {
                                 call.send("echo:" + req.payload);
                             });

    h2mech::grpc::ServerConfig cfg;
    cfg.http.local = {opt.bind_ip, opt.port};

    auto srv = h2mech::grpc::Server::create(cfg, service);
    if (!srv) {
        std::cerr << "创建 server 失败\n";
        return 1;
    }
    if (const auto ec = srv->start()) {
        std::cerr << "启动失败: " << ec.message() << "\n";
        return 1;
    }

    const auto bound = srv->local();
    std::cout << "[" << opt.service << "] gRPC 监听 " << bound.to_string() << "\n";
    std::cout << "  方法: /demo.Echo/Say (unary), /demo.Echo/Chat (bidi)\n";

    // ---- 注册到 Nacos（可选）----
    std::unique_ptr<demo::NacosRegistry> registry;
    bool                                 registered = false;
    if (!opt.nacos.empty()) {
        const auto colon = opt.nacos.find(':');
        const auto host  = opt.nacos.substr(0, colon);
        const auto port  = static_cast<std::uint16_t>(
            std::atoi(opt.nacos.substr(colon == std::string::npos ? 0 : colon + 1).c_str()));

        registry = std::make_unique<demo::NacosRegistry>(host, port);
        std::string err;
        registered = registry->register_service(opt.service, opt.bind_ip, bound.port(), err);
        if (registered) {
            std::cout << "  已注册到 Nacos " << opt.nacos << "（服务名 " << opt.service << "）\n";
        } else {
            std::cerr << "  注册 Nacos 失败: " << err << "（继续以直连模式服务）\n";
        }
    }

    // 退出方式：
    //   --run-for N  跑 N 秒后自动退出（便于脚本/CI 里跑，不依赖交互式终端）
    //   否则          等回车
    // ★ 为什么不只用"等回车"：stdin 被重定向/关闭时 getline 立刻返回 EOF，
    //   进程会瞬间退出，看起来像"服务起不来"。
    if (opt.run_for > 0) {
        std::cout << "\n运行 " << opt.run_for << " 秒后自动退出...\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(opt.run_for));
    } else {
        std::cout << "\n回车退出（退出时会注销）...\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
    }

    if (registered && registry) {
        std::string err;
        if (registry->deregister_service(opt.service, opt.bind_ip, bound.port(), err)) {
            std::cout << "已从 Nacos 注销\n";
        } else {
            std::cerr << "注销失败: " << err << "\n";
        }
    }

    srv->stop();
    netmech::shutdown();
    return 0;
}
