// ============================================================================
// VLR 性能统计宏
//
// VLR_PROFILE_BEGIN(var)          — 记录起始时间点到局部变量 var
// VLR_PROFILE_END(var, outMs)     — 将自 var 以来的耗时(毫秒) 赋值到已有 double 变量 outMs
// VLR_PROFILE_END_NEW(var, newMs) — 声明 double newMs 并赋值耗时，适合只用一次的场景
//
// 需要 cmake -DVLR_PROFILE_SCENE_PREPARE=ON 启用；
// 未启用时所有宏展开为空操作，零开销。
// ============================================================================

#pragma once

#include <chrono>
#include <cstdio>

#ifdef VLR_PROFILE_SCENE_PREPARE

    #define VLR_PROFILE_BEGIN(var) \
        auto var = std::chrono::high_resolution_clock::now()

    #define VLR_PROFILE_END(var, outMs) \
        (outMs) = std::chrono::duration<double, std::milli>( \
            std::chrono::high_resolution_clock::now() - (var)).count()

    #define VLR_PROFILE_END_NEW(var, newMs) \
        double newMs = std::chrono::duration<double, std::milli>( \
            std::chrono::high_resolution_clock::now() - (var)).count()

#else

    #define VLR_PROFILE_BEGIN(var)           ((void)0)
    #define VLR_PROFILE_END(var, outMs)      ((void)0)
    #define VLR_PROFILE_END_NEW(var, newMs)  ((void)0)

#endif
