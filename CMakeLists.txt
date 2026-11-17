# =============================================================================
#  h2mech —— HTTP/2 与 gRPC 协议层（基于 netmech 的 TCP/TLS）
# =============================================================================
#  依赖关系：
#      h2mech  ──►  netmech（TCP / TLS / ALPN）
#         └─────►  nghttp2（帧层 / HPACK / 流控，第三阶段引入）
#
#  与 httpmech 的关系：**互不依赖**。httpmech 冻结在 HTTP/1.1（含 HTTPS/WSS），
#  本库只做 h2 与 gRPC。要把 h2 请求塞进 httpmech 的路由表，用可选的
#  adapter/httpmech target（默认不编）。
#
#  构建（与其它 mech 库一致）：
#      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#      cmake --build build --parallel
#      ctest --test-dir build --output-on-failure
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(h2mech
        VERSION 0.1.0
        DESCRIPTION "HTTP/2 and gRPC protocol layer built on netmech"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------------------
# 1. 选项
# ---------------------------------------------------------------------------
option(H2MECH_BUILD_TESTS        "构建单元测试" ON)
option(H2MECH_BUILD_EXAMPLES     "构建示例" OFF)
option(H2MECH_WARNINGS_AS_ERRORS "把**库与示例**的告警当错误（CI 用；test/ 不受约束）" OFF)
option(H2MECH_PIC                "编译为位置无关代码（静态库要链进 .so 时必须开）" ON)
option(H2MECH_WITH_NGHTTP2       "使用仓内 vendored 的 nghttp2（关掉只能编头字段层）" ON)

# ---------------------------------------------------------------------------
# 2. 定位 netmech（同级目录，或 -DH2MECH_NETMECH_DIR=<路径>）
# ---------------------------------------------------------------------------
set(H2MECH_NETMECH_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../netmech"
    CACHE PATH "netmech 所在目录")

if(NOT EXISTS "${H2MECH_NETMECH_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 netmech：${H2MECH_NETMECH_DIR}\n"
        "   指定：cmake -DH2MECH_NETMECH_DIR=<netmech 路径> ...")
endif()

# 库类型：STATIC（默认）/ SHARED。
#
# ★ 必须在 add_subdirectory(netmech) 之前定义：SHARED 构建要求 netmech 也是
#   共享库（只静态链 netmech 会让注入的 Runtime 跨两份副本使用，netmech 文档
#   明确禁止 —— 实测会段错误）。
set(H2MECH_LIBRARY_TYPE "STATIC" CACHE STRING "库类型：STATIC 或 SHARED")
set_property(CACHE H2MECH_LIBRARY_TYPE PROPERTY STRINGS STATIC SHARED)
if(H2MECH_LIBRARY_TYPE STREQUAL "SHARED")
    set(NETMECH_LIBRARY_TYPE "SHARED" CACHE STRING "netmech 库类型" FORCE)
endif()

set(NETMECH_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(NETMECH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${H2MECH_NETMECH_DIR}" "${CMAKE_BINARY_DIR}/netmech"
                 EXCLUDE_FROM_ALL)

message(STATUS "h2mech: netmech @ ${H2MECH_NETMECH_DIR}")
message(STATUS "   TLS（netmech）: ${NETMECH_TLS}")

# ---------------------------------------------------------------------------
# 3. 编译选项
# ---------------------------------------------------------------------------
add_library(h2mech_compile_options INTERFACE)

# genex 里的多个选项必须用 ';' 分隔，不能靠空格 —— 否则 VS 生成器会把
# "a b" 原样塞进 AdditionalOptions，与 Ninja/Makefile 行为不一致。
target_compile_options(h2mech_compile_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>>
    $<$<COMPILE_LANGUAGE:C>:$<$<C_COMPILER_ID:MSVC>:/utf-8;/W3>>
    $<$<COMPILE_LANGUAGE:C>:$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall;-Wextra>>)

# ---------------------------------------------------------------------------
# 3.5 nghttp2（vendored，只编 lib/）
# ---------------------------------------------------------------------------
#  它只被 src/h2/session.cpp 用到，且**公共头一行都不引**（quality job 守着
#  这条边界）。关掉 H2MECH_WITH_NGHTTP2 时只能编到"头字段语义转换层"，
#  会话相关代码由 H2MECH_HAS_NGHTTP2 宏整段排除。
if(H2MECH_WITH_NGHTTP2)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nghttp2"
                     "${CMAKE_BINARY_DIR}/nghttp2")
    message(STATUS "h2mech: nghttp2 @ thirdparty/nghttp2（vendored，仅 lib/）")
else()
    message(STATUS "h2mech: nghttp2 已关闭（只编头字段语义层）")
endif()

# ---------------------------------------------------------------------------
# 4. 主库
# ---------------------------------------------------------------------------
#  ★ 严格告警（/WX、-Werror）只挂在**库本体与示例**上，不挂在这个 INTERFACE
#    目标上 —— 它是 PUBLIC 依赖，挂上去会顺着依赖传给 test/ 和下游使用者，
#    而测试不应该为第三方头的新版本弃用警告买单（netmech 0.91 定的策略）。
set(H2MECH_STRICT_WARNING_FLAGS "")
if(H2MECH_WARNINGS_AS_ERRORS)
    if(MSVC)
        set(H2MECH_STRICT_WARNING_FLAGS /WX)
    else()
        set(H2MECH_STRICT_WARNING_FLAGS -Werror)
    endif()
endif()

file(GLOB_RECURSE H2MECH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")
file(GLOB H2MECH_PUBLIC_HEADERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/h2mech/*.hpp")

if(H2MECH_SOURCES)
    add_library(h2mech ${H2MECH_LIBRARY_TYPE} ${H2MECH_SOURCES} ${H2MECH_PUBLIC_HEADERS})
    set(H2MECH_IS_INTERFACE OFF)
else()
    message(STATUS "h2mech: src/ 下暂无 .cpp，构建为 INTERFACE（header-only）库")
    add_library(h2mech INTERFACE)
    target_sources(h2mech INTERFACE ${H2MECH_PUBLIC_HEADERS})
    set(H2MECH_IS_INTERFACE ON)
endif()

add_library(h2mech::h2mech ALIAS h2mech)

target_include_directories(h2mech PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# ★ 依赖名必须分成"源码树内"和"安装后"两种：
#   * add_subdirectory(netmech) 引入时，目标就叫 `netmech`（netmech 没有
#     定义 netmech::netmech 的 ALIAS）；
#   * 而装出去的包经 netmechTargets.cmake 导出后，使用者看到的是
#     `netmech::netmech`。
#   直接写死其中一个，另一条路径就会在"用别人的安装包"时炸掉。
target_link_libraries(h2mech PUBLIC
    $<BUILD_INTERFACE:netmech>
    $<INSTALL_INTERFACE:netmech::netmech>)
target_link_libraries(h2mech PUBLIC h2mech_compile_options)

# nghttp2 是**实现细节**：公共头不引它，所以 PRIVATE。
#
# 目前（S1 收尾）还没有源文件用到 nghttp2 —— 会话层在 experiments/h2-session-wip
# 里等一个未解决的接入问题（见该目录 README）。这里**先不链接**，避免给产物
# 带进一个没人用的 300 KB 静态库；等 S2 落地时把下面三行打开即可。
# if(H2MECH_WITH_NGHTTP2)
#     target_compile_definitions(h2mech PRIVATE H2MECH_HAS_NGHTTP2)
#     target_link_libraries(h2mech PRIVATE nghttp2_static)
# endif()

if(H2MECH_LIBRARY_TYPE STREQUAL "SHARED")
    target_compile_definitions(h2mech PUBLIC H2MECH_SHARED)
    target_compile_definitions(h2mech PRIVATE H2MECH_BUILDING_LIBRARY)
    set_target_properties(h2mech PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(MSVC)
        target_compile_options(h2mech PRIVATE /wd4251)
    elseif(UNIX AND NOT APPLE)
        target_link_options(h2mech PRIVATE "-Wl,--exclude-libs,ALL")
    endif()
endif()

if(NOT H2MECH_IS_INTERFACE)
    target_compile_options(h2mech PRIVATE ${H2MECH_STRICT_WARNING_FLAGS})
endif()

set_target_properties(h2mech PROPERTIES
    POSITION_INDEPENDENT_CODE ${H2MECH_PIC}
    FOLDER "h2mech")

# ---------------------------------------------------------------------------
# 5. 单元测试
# ---------------------------------------------------------------------------
if(H2MECH_BUILD_TESTS)
    enable_testing()
    file(GLOB H2MECH_TEST_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")

    if(H2MECH_TEST_SOURCES)
        add_executable(h2mech_ut ${H2MECH_TEST_SOURCES})
        set_target_properties(h2mech_ut PROPERTIES FOLDER "h2mech/test")
        target_include_directories(h2mech_ut PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
        target_link_libraries(h2mech_ut PRIVATE h2mech)

        # 端到端用例直接 include <nghttp2/nghttp2.h>（用它的 client 模式当对端），
        # 因此需要 nghttp2 的**使用需求**（头路径 + Windows 的 ssize_t 定义）。
        # 会话层的端到端用例在 experiments/h2-session-wip（未接入构建）：
        # 等它修好之后再给测试目标加 nghttp2 的头 + 宏（注意**不要**再
        # target_link_libraries 一次 nghttp2_static，那会让同一个静态库在
        # 可执行文件里出现两份符号）。
        add_test(NAME h2mech_ut COMMAND h2mech_ut)

        # 统一输出到 bin/：CI 里"失败时直接跑一遍取详细输出"用的是这个路径，
        # 单配置（Ninja/gcc）与多配置（VS）都要一致，否则那条兜底命令会
        # "找不到文件"，把真正的失败信息盖掉。
        set_target_properties(h2mech_ut PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
endif()

# ---------------------------------------------------------------------------
# 6. 安装与导出
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT H2MECH_IS_INTERFACE)
    # RUNTIME / LIBRARY 两行不能省：SHARED 构建在 Windows 上是 .dll、
    # Linux 上是 .so，只写 ARCHIVE 会装不出库文件本身，使用者一运行就
    # 0xC0000135 / 找不到 .so。
    install(TARGETS h2mech
            EXPORT h2mechTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/h2mech")
else()
    install(TARGETS h2mech EXPORT h2mechTargets)
endif()

install(TARGETS h2mech_compile_options EXPORT h2mechTargets)
install(DIRECTORY include/h2mech
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT h2mechTargets
        FILE h2mechTargets.cmake
        NAMESPACE h2mech::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/h2mech")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/h2mechConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/h2mechConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/h2mech")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/h2mechConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/h2mechConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/h2mechConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/h2mech")

message(STATUS "")
message(STATUS "h2mech ${PROJECT_VERSION}")
message(STATUS "   库类型        : ${H2MECH_LIBRARY_TYPE}")
message(STATUS "   构建测试      : ${H2MECH_BUILD_TESTS}")
message(STATUS "   告警当错误    : ${H2MECH_WARNINGS_AS_ERRORS}（仅库与示例）")
message(STATUS "")
