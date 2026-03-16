// ============================================================================
// PbrtParser - PBRT v4 场景文件解析器
//
// 功能：
//   1. 词法分析：将文本流切分为 Token
//   2. 语法解析：识别 PBRT v4 指令，填充 PbrtSceneData
//   3. 并行 PLY 加载：通过 Taskflow 并行读取所有 plymesh 文件
//
// 设计原则：
//   - 解析阶段单线程（顺序依赖），PLY I/O 阶段并行
//   - string_view 避免 token 拷贝
//   - 结果通过移动语义返回（std::optional<PbrtSceneData>）
// ============================================================================

#pragma once

#include "PbrtSceneData.h"
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>

namespace viewer {

class PbrtParser {
public:
    struct ParseOptions {
        bool loadPlyFiles    = true;   // 是否并行预加载 PLY 文件顶点数据
        bool verbose         = false;  // 打印解析进度
        uint32_t numThreads  = 0;      // 0 = 自动
    };

    /**
     * @brief 从文件路径解析 PBRT v4 场景
     * @return 成功时返回完整场景数据，失败返回 nullopt
     */
    [[nodiscard]] static std::optional<PbrtSceneData> parseFile(
        const std::string& filepath,
        const ParseOptions& options = {}
    );

private:
    // 仅供静态函数内部使用的实现类（隐藏在 .cpp 中）
    struct Impl;
};

} // namespace viewer
