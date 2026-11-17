// =============================================================================
//  h2mech 单元测试入口
// =============================================================================
//  用例散在 test/test_*.cpp 里，用 `H2MECH_TEST(name)` 自动注册（见
//  test_framework.hpp）。这里只负责解析过滤参数并汇总结果。
// =============================================================================
#include "test_framework.hpp"

#include <netmech/common.hpp>   // netmech::shutdown()

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("用法: h2mech_ut [名字子串]\n");
            return 0;
        }
        if (filter.empty()) {
            filter = argv[i];   // 裸参数即过滤子串
        }
    }
    const int rc = h2mech_test::run_all(filter);

    // 退出前停掉 netmech 的进程级 io 池。
    //
    // TLS 用例会真建连接、真跑 io 线程；不停的话进程退出时可能还有线程在跑，
    // 与 OpenSSL 的 atexit 清理相撞 —— 表现为 sanitizer 在 exit() 阶段报一条
    // libcrypto 的 data race（netmech 仓里踩过同样的问题）。
    netmech::shutdown();
    return rc;
}
