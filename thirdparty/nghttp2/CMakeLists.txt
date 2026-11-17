# =============================================================================
#  nghttp2（vendored，只编 lib/）
# =============================================================================
#  为什么不用上游的顶层 CMake：
#    · 它会探测一堆我们根本用不到的东西（libev / c-ares / zlib / systemd /
#      jansson / libxml2 / jemalloc…），任何一个探测行为变化都可能让 CI 变红，
#      而我们只要 libnghttp2 一个静态库；
#    · 上游还会 add_subdirectory(src/examples/doc/tests/third-party)，那要
#      把 8 MB 的整棵树搬进仓库。
#  这里的做法：只抄 lib/*.c 与 includes/nghttp2/*.h，自己给出平台宏。
#  代价是**升级时要重新抄一遍**（步骤见 README.md）。
#
#  编译宏的来历（全部来自上游 cmakeconfig.h.in / nghttp2_net.h / nghttp2_time.c）：
#    BUILDING_NGHTTP2      → 导出宏走 dllexport 分支，并强制启用 ssize_t 回调
#    NGHTTP2_STATICLIB     → 消费者侧不做 dllimport（必须 PUBLIC）
#    HAVE_ARPA_INET_H      → POSIX 的 htonl/ntohl（nghttp2_net.h）
#    HAVE_NETINET_IN_H     → 同上（部分平台只有 netinet/in.h）
#    HAVE_CLOCK_GETTIME    → 单调时钟走 clock_gettime
#    HAVE_DECL_CLOCK_MONOTONIC=1
#    HAVE_WINDOWS_H        → Windows 下取 GetTickCount64
#    HAVE_GETTICKCOUNT64
#    ssize_t=SSIZE_T       → MSVC 没有 ssize_t，上游也是这么补的（cmakedefine）
# =============================================================================
file(GLOB NGHTTP2_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.c")

add_library(nghttp2_static STATIC ${NGHTTP2_SOURCES})
add_library(nghttp2::nghttp2 ALIAS nghttp2_static)

target_include_directories(nghttp2_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib")

target_compile_definitions(nghttp2_static PRIVATE BUILDING_NGHTTP2)
target_compile_definitions(nghttp2_static PUBLIC  NGHTTP2_STATICLIB)

# 关掉 MSVC 对 C 代码的安全函数告警（上游代码大量用 memcpy/strcpy 这类，
# 我们不改上游源码；只影响这个静态库自己）。
if(MSVC)
    # ssize_t：MSVC 没有这个类型，而上游在构建库时强制启用基于 ssize_t 的回调
    # （nghttp2.h 里 `#ifdef BUILDING_NGHTTP2 #undef NGHTTP2_NO_SSIZE_T`）。
    # 上游 cmakeconfig 用的是 SSIZE_T，但它来自 <BaseTsd.h>，在 nghttp2.h 之前
    # 并不保证可见（直接 -Dssize_t=SSIZE_T 会让声明解析失败，报一串 C2059）。
    # 用 stddef.h 里的 ptrdiff_t：nghttp2.h 第 54 行就 include 了它，且语义一致
    # （有符号的 size_t 对应物；上游自己也是 `typedef ptrdiff_t nghttp2_ssize`）。
    target_compile_definitions(nghttp2_static PRIVATE
        WIN32
        HAVE_WINDOWS_H
        HAVE_GETTICKCOUNT64)
    # ★ ssize_t 必须是 **PUBLIC**：nghttp2.h 里的回调签名（send/recv/data_read）
    #   在 Windows 上同样需要 ssize_t，而 MSVC 没有这个类型。只给静态库自己
    #   定义的话，使用者（src/h2/server.cpp）一 include nghttp2.h 就炸。
    target_compile_definitions(nghttp2_static PUBLIC ssize_t=ptrdiff_t)
    target_compile_options(nghttp2_static PRIVATE /W3 /wd4267 /wd4244)
    target_link_libraries(nghttp2_static PUBLIC ws2_32)
else()
    target_compile_definitions(nghttp2_static PRIVATE
        HAVE_ARPA_INET_H
        HAVE_NETINET_IN_H
        HAVE_CLOCK_GETTIME
        HAVE_DECL_CLOCK_MONOTONIC=1)
endif()

set_target_properties(nghttp2_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "thirdparty")

# 跟着 h2mech 一起装出去：h2mech 是**静态**库时，使用方最终链接需要
# libnghttp2.a；导出集里没有它，install(EXPORT) 会直接报错。
install(TARGETS nghttp2_static
        EXPORT h2mechTargets
        ARCHIVE DESTINATION "lib")
install(DIRECTORY includes/nghttp2 DESTINATION "include")
