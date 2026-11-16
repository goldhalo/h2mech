// =============================================================================
//  h2mech - 共享库导出宏
// =============================================================================
//  静态库下展开为空；SHARED 构建时由 CMake 定义 H2MECH_SHARED /
//  H2MECH_BUILDING_LIBRARY（见 CMakeLists 第 4 节）。
// =============================================================================
#pragma once

#if defined(_WIN32) && defined(H2MECH_SHARED)
#  if defined(H2MECH_BUILDING_LIBRARY)
#    define H2MECH_API __declspec(dllexport)
#  else
#    define H2MECH_API __declspec(dllimport)
#  endif
#else
#  define H2MECH_API
#endif
