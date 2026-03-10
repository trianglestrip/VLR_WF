# ============================================================================
# Viewer - 3D场景查看器和渲染器前端
#
# 功能：
# 1. 使用Assimp加载各种3D模型格式
# 2. 封装libVLR调用，提供简化的渲染接口
# 3. 供测试程序使用
# ============================================================================

cmake_minimum_required(VERSION 3.18)

# ----------------------------------------------------------------------------
# Assimp 预编译库路径
# ----------------------------------------------------------------------------
set(ASSIMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/assimp")
set(ASSIMP_INCLUDE_DIR "${ASSIMP_ROOT}/include")
set(ASSIMP_LIB_DIR "${ASSIMP_ROOT}/lib")
set(ASSIMP_BIN_DIR "${ASSIMP_ROOT}/bin")

# ----------------------------------------------------------------------------
# Taskflow 头文件库路径（Header-Only，直接在taskflow目录下）
# ----------------------------------------------------------------------------
set(TASKFLOW_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/taskflow")

# 检查Assimp是否存在
if(EXISTS "${ASSIMP_INCLUDE_DIR}/assimp/Importer.hpp")
    set(ASSIMP_FOUND TRUE)
    message(STATUS "[Viewer] 找到Assimp预编译库: ${ASSIMP_ROOT}")
else()
    set(ASSIMP_FOUND FALSE)
    message(WARNING "[Viewer] 未找到Assimp预编译库，请参考 external/assimp/README.md")
    message(WARNING "[Viewer] Viewer库将不会被构建")
    return()
endif()

# ----------------------------------------------------------------------------
# Assimp 库文件（根据构建类型选择）
# ----------------------------------------------------------------------------
# Visual Studio 2022 通常使用 vc143
set(ASSIMP_LIB_RELEASE "${ASSIMP_LIB_DIR}/assimp-vc143-mt.lib")
set(ASSIMP_LIB_DEBUG "${ASSIMP_LIB_DIR}/assimp-vc143-mtd.lib")

# 如果找不到vc143，尝试其他版本
if(NOT EXISTS "${ASSIMP_LIB_RELEASE}")
    file(GLOB ASSIMP_LIBS "${ASSIMP_LIB_DIR}/assimp*.lib")
    if(ASSIMP_LIBS)
        list(GET ASSIMP_LIBS 0 ASSIMP_LIB_RELEASE)
        set(ASSIMP_LIB_DEBUG ${ASSIMP_LIB_RELEASE})
        message(STATUS "[Viewer] 使用Assimp库: ${ASSIMP_LIB_RELEASE}")
    else()
        message(WARNING "[Viewer] 未找到Assimp库文件在 ${ASSIMP_LIB_DIR}")
        return()
    endif()
endif()

# ----------------------------------------------------------------------------
# Viewer 库源文件
# ----------------------------------------------------------------------------
set(VIEWER_SOURCES
    src/VLRRenderer.cpp
    src/SceneLoader.cpp
)

set(VIEWER_HEADERS
    include/VLRRenderer.h
    include/SceneLoader.h
    include/MeshData.h
    include/ZeroCopyMesh.h
)

# ----------------------------------------------------------------------------
# 创建 Viewer 库
# ----------------------------------------------------------------------------
add_library(Viewer STATIC ${VIEWER_SOURCES} ${VIEWER_HEADERS})

# 包含目录
target_include_directories(Viewer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libVLR/include
    ${ASSIMP_INCLUDE_DIR}
    ${TASKFLOW_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/external
)

# 链接库
target_link_libraries(Viewer PUBLIC
    VLR
    $<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>
    $<$<CONFIG:Release>:${ASSIMP_LIB_RELEASE}>
    $<$<CONFIG:RelWithDebInfo>:${ASSIMP_LIB_RELEASE}>
    $<$<CONFIG:MinSizeRel>:${ASSIMP_LIB_RELEASE}>
)

# C++20 标准
target_compile_features(Viewer PUBLIC cxx_std_20)

# 编译选项
target_compile_options(Viewer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

# 设置运行时库（与VLR保持一致）
set_target_properties(Viewer PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# ----------------------------------------------------------------------------
# 复制 Assimp DLL 到输出目录
# ----------------------------------------------------------------------------
set(ASSIMP_DLL_RELEASE "${ASSIMP_BIN_DIR}/assimp-vc143-mt.dll")
set(ASSIMP_DLL_DEBUG "${ASSIMP_BIN_DIR}/assimp-vc143-mtd.dll")

# 如果找不到vc143 DLL，尝试查找其他版本
if(NOT EXISTS "${ASSIMP_DLL_RELEASE}")
    file(GLOB ASSIMP_DLLS "${ASSIMP_BIN_DIR}/assimp*.dll")
    if(ASSIMP_DLLS)
        list(GET ASSIMP_DLLS 0 ASSIMP_DLL_RELEASE)
        set(ASSIMP_DLL_DEBUG ${ASSIMP_DLL_RELEASE})
    endif()
endif()

# 复制DLL到bin目录
if(EXISTS "${ASSIMP_DLL_RELEASE}")
    add_custom_command(TARGET Viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ASSIMP_DLL_RELEASE}"
            "${CMAKE_SOURCE_DIR}/bin/"
        COMMENT "复制Assimp DLL (Release) 到 bin/"
    )
    message(STATUS "[Viewer] 将复制Assimp DLL: ${ASSIMP_DLL_RELEASE} -> bin/")
endif()

if(EXISTS "${ASSIMP_DLL_DEBUG}" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_command(TARGET Viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ASSIMP_DLL_DEBUG}"
            "${CMAKE_SOURCE_DIR}/bin/"
        COMMENT "复制Assimp DLL (Debug) 到 bin/"
    )
endif()

# ----------------------------------------------------------------------------
# 安装规则
# ----------------------------------------------------------------------------
install(TARGETS Viewer
    ARCHIVE DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include/viewer
    FILES_MATCHING PATTERN "*.h"
)
