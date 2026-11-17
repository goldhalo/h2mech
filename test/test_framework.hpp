// =============================================================================
//  h2mech 测试框架（头文件-only，自动注册）
// =============================================================================
//  为什么不用 httpmech 那种"所有用例塞进一个 test_main.cpp + 一张 cases 表"：
//    h2 与 gRPC 的用例会长到两三千行，一张表迟早变成合并冲突高发区。
//    这里改成 `H2MECH_TEST(name) { ... }` 自动注册，一个文件一个主题。
//
//  用法：
//      H2MECH_TEST(headers_pseudo_order) {
//          CHECK_EQ(build(...).status, h2mech::h2::HeadersStatus::ok);
//      }
//  运行：`h2mech_ut`（全部）或 `h2mech_ut headers`（按名字子串过滤）。
// =============================================================================
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace h2mech_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& cases()
{
    // 函数内 static：跨 TU 的静态初始化顺序不确定，注册必须发生在 main 之后
    static std::vector<Case> v;
    return v;
}

inline int& checks()
{
    static int v = 0;
    return v;
}

inline int& fails()
{
    static int v = 0;
    return v;
}

inline const char*& current_case()
{
    static const char* v = "";
    return v;
}

struct Registrar {
    Registrar(const char* name, void (*fn)())
    {
        cases().push_back(Case{name, fn});
    }
};

/// 跑全部（或按名字子串过滤）。返回进程退出码：0 = 全过。
inline int run_all(const std::string& filter)
{
    int run     = 0;
    int skipped = 0;

    for (const Case& c : cases()) {
        if (!filter.empty() && std::strstr(c.name, filter.c_str()) == nullptr) {
            ++skipped;
            continue;
        }
        ++run;
        const int before = fails();
        current_case()   = c.name;
        std::printf("  [%s]\n", c.name);
        c.fn();
        std::printf("    %s\n", fails() == before ? "ok" : "FAILED");
    }

    std::printf("\n用例: %d 运行", run);
    if (skipped > 0) {
        std::printf("（%d 个被过滤）", skipped);
    }
    std::printf("\n断言: %d 通过 / %d 失败 / %d 总计\n", checks() - fails(), fails(),
                checks());
    std::printf("结果: %s\n", fails() == 0 ? "PASS" : "FAIL");
    return fails() == 0 ? 0 : 1;
}

} // namespace h2mech_test

// =============================================================================
//  断言宏：失败只打印"第一现场"（文件:行 + 表达式），不打印调用栈
// =============================================================================
#define H2MECH_CHECK_IMPL(expr, desc)                                            \
    do {                                                                         \
        ++::h2mech_test::checks();                                               \
        if (!(expr)) {                                                           \
            ++::h2mech_test::fails();                                            \
            std::printf("    FAIL [%s] %s:%d  %s\n", ::h2mech_test::current_case(), \
                        __FILE__, __LINE__, desc);                               \
        }                                                                        \
    } while (0)

#define CHECK(expr) H2MECH_CHECK_IMPL(expr, #expr)

/// 带自定义说明的断言（失败时把上下文一起打出来，省一次复跑）。
///
/// 用 `std::string(msg).c_str()` 而不是直接把 msg 交给 `%s`：
/// 调用方既可能传字面量，也可能传 `std::string`（如 std::to_string(...)），
/// 后者直接进 printf 是 UB（GCC 会报 -Wformat）。
#define CHECK_MSG(expr, msg)                                                     \
    H2MECH_CHECK_IMPL(expr, std::string(msg).c_str())

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++::h2mech_test::checks();                                               \
        if (!((a) == (b))) {                                                     \
            ++::h2mech_test::fails();                                            \
            std::printf("    FAIL [%s] %s:%d  %s != %s\n",                       \
                        ::h2mech_test::current_case(), __FILE__, __LINE__, #a, #b); \
        }                                                                        \
    } while (0)

/// 条件为假时记录失败并**直接返回**（后续代码依赖该前提时用）。
#define REQUIRE(expr)                                                            \
    do {                                                                         \
        ++::h2mech_test::checks();                                               \
        if (!(expr)) {                                                           \
            ++::h2mech_test::fails();                                            \
            std::printf("    FAIL [%s] %s:%d  REQUIRE %s\n",                     \
                        ::h2mech_test::current_case(), __FILE__, __LINE__, #expr); \
            return;                                                              \
        }                                                                        \
    } while (0)

/// 定义一个用例并自动注册：`H2MECH_TEST(name) { ... }`
#define H2MECH_TEST(name)                                                        \
    static void h2mech_test_fn_##name();                                         \
    static ::h2mech_test::Registrar h2mech_test_reg_##name{#name,                \
                                                           &h2mech_test_fn_##name}; \
    static void h2mech_test_fn_##name()
