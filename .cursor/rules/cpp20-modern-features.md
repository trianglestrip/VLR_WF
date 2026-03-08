# C++20 现代特性使用规范

本项目使用 C++20 标准，优先使用现代 C++ 特性以提升代码质量、安全性和可维护性。

## 1. 智能指针 (Smart Pointers)

### 1.1 使用 `std::unique_ptr` 管理独占所有权

**✅ 推荐做法**:
```cpp
// 使用 std::unique_ptr 自动管理资源
std::unique_ptr<cudau::Buffer<uint32_t>> buffer;

// 使用 std::make_unique 分配
buffer = std::make_unique<cudau::Buffer<uint32_t>>();

// 使用 reset() 释放
buffer.reset();

// std::array 管理固定大小的智能指针数组
std::array<std::unique_ptr<cudau::Buffer<uint32_t>>, NumCategories> buffers;
```

**❌ 避免做法**:
```cpp
// 不要使用原始指针 + 手动 delete
cudau::Buffer<uint32_t>* buffer = new cudau::Buffer<uint32_t>();
delete buffer;
buffer = nullptr;
```

**优点**:
- RAII 自动管理生命周期
- 异常安全
- 明确所有权语义
- 避免内存泄漏

### 1.2 使用 `std::shared_ptr` 管理共享所有权

```cpp
// 多个对象需要共享资源时使用
std::shared_ptr<Scene> scene = std::make_shared<Scene>();
```

## 2. `std::optional` 替代特殊值

### 2.1 使用 `std::optional` 表示可选值

**✅ 推荐做法**:
```cpp
// 使用 std::optional 表示可能不存在的值
std::optional<uint32_t> envLightInstIndex;

// 检查是否有值
if (envLightInstIndex.has_value()) {
    uint32_t index = envLightInstIndex.value();
    // 或使用 *envLightInstIndex
}

// 提供默认值
uint32_t index = envLightInstIndex.value_or(0xFFFFFFFF);

// 赋值
envLightInstIndex = 42;

// 清空
envLightInstIndex.reset();
```

**❌ 避免做法**:
```cpp
// 不要使用魔术数字表示"无效值"
uint32_t envLightInstIndex = 0xFFFFFFFF;  // 魔术数字
if (envLightInstIndex != 0xFFFFFFFF) {
    // ...
}
```

**优点**:
- 语义清晰，明确表示"可能没有值"
- 类型安全
- 避免魔术数字
- 强制检查是否有值

## 3. `constexpr` 编译期计算

### 3.1 使用 `constexpr` 定义常量

**✅ 推荐做法**:
```cpp
// 编译期常量
constexpr uint32_t NumMaterialCategories = 6;
constexpr uint32_t MaxPathLength = 64;
constexpr float Pi = 3.14159265358979323846f;

// 编译期函数
constexpr float square(float x) {
    return x * x;
}

// 编译期计算
constexpr float area = Pi * square(5.0f);
```

**❌ 避免做法**:
```cpp
// 不要使用宏定义
#define NUM_MATERIAL_CATEGORIES 6  // 无类型检查

// 不要使用运行时常量（如果可以编译期确定）
const uint32_t NumMaterialCategories = 6;  // 运行时常量
```

**优点**:
- 编译期求值，零运行时开销
- 类型安全
- 更好的调试支持
- 可用于模板参数

### 3.2 使用 `consteval` 强制编译期求值 (C++20)

```cpp
// 必须在编译期求值
consteval size_t computeBufferSize(uint32_t width, uint32_t height) {
    return width * height * sizeof(SpectrumStorage);
}
```

## 4. `std::span` 零拷贝视图 (推荐)

### 4.1 使用 `std::span` 替代指针+大小参数

**✅ 推荐做法**:
```cpp
// 内部实现使用 span
void processVertices(std::span<const float> vertices) {
    size_t count = vertices.size();
    for (float v : vertices) {
        // ...
    }
}

// 调用
std::vector<float> verts = {...};
processVertices(verts);  // 自动转换
processVertices({data, size});  // 从指针+大小构造
```

**❌ 避免做法**:
```cpp
// 不要分离指针和大小参数
void processVertices(const float* vertices, size_t numVertices) {
    // 容易出错：忘记检查 numVertices
}
```

**优点**:
- 零拷贝，只传递指针和大小
- 类型安全，自动边界检查
- 统一接口，支持数组、vector、span
- 更清晰的函数签名

**注意**: C API 保持原有接口（ABI 稳定性），内部实现可使用 span。

## 5. `std::array` 替代 C 数组

**✅ 推荐做法**:
```cpp
// 使用 std::array
std::array<float, 3> color = {1.0f, 0.0f, 0.0f};
std::array<std::unique_ptr<Buffer>, 8> buffers;

// 访问
color[0] = 0.5f;
size_t size = color.size();
```

**❌ 避免做法**:
```cpp
// 不要使用 C 数组（除非必须）
float color[3] = {1.0f, 0.0f, 0.0f};
Buffer* buffers[8];  // 需要手动管理
```

## 6. Designated Initializers (有限使用)

### 6.1 仅在纯 C++ 代码中使用

**✅ 可以使用**:
```cpp
// 纯 C++ 代码中可以使用
struct Config {
    bool enableLogging;
    int logLevel;
    const char* outputPath;
};

Config config = {
    .enableLogging = true,
    .logLevel = 4,
    .outputPath = "output.log"
};
```

**❌ 避免在 CUDA 代码中使用**:
```cpp
// CUDA/PTX 编译器可能不支持
// 避免在 .cu 文件或被 .cu 包含的头文件中使用
OptixPipelineCompileOptions options = {
    .usesMotionBlur = false,  // 可能导致 PTX 编译失败
    .numPayloadValues = 7
};
```

**原因**: CUDA 编译器对 C++20 特性支持有限。

## 7. `std::ranges` 简化容器操作 (推荐)

**✅ 推荐做法**:
```cpp
#include <ranges>
#include <algorithm>

// 使用 ranges 进行转换
auto positions = std::views::iota(0u, numVertices)
    | std::views::transform([&](size_t i) {
        return Point3D{data[i*3], data[i*3+1], data[i*3+2]};
    });

// 过滤
auto validNormals = normals 
    | std::views::filter([](const auto& n) { return length(n) > 1e-8f; })
    | std::views::transform([](const auto& n) { return normalize(n); });

// 复制到容器
std::vector<Point3D> result;
std::ranges::copy(positions, std::back_inserter(result));
```

**优点**:
- 更简洁的代码
- 惰性求值，性能更好
- 易于组合和复用
- 函数式编程风格

## 8. Concepts 类型约束 (推荐)

**✅ 推荐做法**:
```cpp
// 定义概念约束
template<typename T>
concept GPUBuffer = requires(T buf) {
    { buf.getDevicePointer() } -> std::convertible_to<const void*>;
    { buf.initialize() } -> std::same_as<void>;
    { buf.copyToDevice() } -> std::same_as<void>;
};

// 使用概念约束模板
template<GPUBuffer BufferType>
class ResourceManager {
    BufferType buffer;
};

// 或使用 requires 子句
template<typename T>
requires std::integral<T>
T add(T a, T b) {
    return a + b;
}
```

**优点**:
- 编译期类型检查
- 更清晰的错误信息
- 自文档化的模板约束
- 更好的 IDE 支持

## 9. 其他现代特性

### 9.1 结构化绑定

```cpp
// 解构返回值
auto [width, height] = getImageSize();

// 遍历 map
for (const auto& [key, value] : myMap) {
    // ...
}
```

### 9.2 `if constexpr` 编译期分支

```cpp
template<typename T>
void process(T value) {
    if constexpr (std::is_integral_v<T>) {
        // 整数处理
    } else if constexpr (std::is_floating_point_v<T>) {
        // 浮点数处理
    }
}
```

### 9.3 `std::format` 格式化 (C++20)

```cpp
#include <format>

// 类型安全的格式化
std::string msg = std::format("Material[{}]: albedo=({:.3f}, {:.3f}, {:.3f})", 
                               index, r, g, b);
```

### 9.4 三路比较运算符 `<=>` (C++20)

```cpp
struct Point {
    float x, y, z;
    auto operator<=>(const Point&) const = default;
};
```

## 10. 禁止使用的旧式写法

### 10.1 原始指针 + 手动内存管理

```cpp
// ❌ 禁止
T* ptr = new T();
delete ptr;

// ✅ 使用智能指针
auto ptr = std::make_unique<T>();
```

### 10.2 魔术数字

```cpp
// ❌ 禁止
if (index == 0xFFFFFFFF) { ... }

// ✅ 使用 std::optional 或命名常量
constexpr uint32_t InvalidIndex = 0xFFFFFFFF;
if (index == InvalidIndex) { ... }

// 或更好
std::optional<uint32_t> index;
if (index.has_value()) { ... }
```

### 10.3 C 风格数组

```cpp
// ❌ 禁止（除非必须）
float colors[3][4];

// ✅ 使用 std::array 或 std::vector
std::array<std::array<float, 4>, 3> colors;
std::vector<std::array<float, 4>> colors;
```

### 10.4 宏定义常量

```cpp
// ❌ 禁止
#define MAX_SIZE 1024

// ✅ 使用 constexpr
constexpr size_t MaxSize = 1024;
```

## 11. CUDA/OptiX 代码特殊注意事项

### 11.1 CUDA 代码中的限制

- **避免使用**: Designated Initializers、`std::format`、复杂的 ranges
- **可以使用**: `constexpr`、`std::array`、智能指针（主机端）
- **必须使用**: 原始指针（设备端内存）

### 11.2 主机/设备代码分离

```cpp
// 主机端：使用所有现代特性
std::unique_ptr<cudau::Buffer<float>> hostBuffer;

// 设备端：使用原始指针
__global__ void kernel(float* devicePtr, size_t size) {
    // CUDA kernel 代码
}
```

## 12. 代码审查检查清单

在代码审查时，检查以下项目：

- [ ] 是否使用智能指针管理动态内存？
- [ ] 是否使用 `std::optional` 替代魔术数字？
- [ ] 常量是否使用 `constexpr`？
- [ ] 是否避免了原始指针 + 手动 delete？
- [ ] 是否使用 `std::array` 替代 C 数组？
- [ ] 函数参数是否可以用 `std::span` 优化？
- [ ] 是否避免在 CUDA 代码中使用不支持的特性？
- [ ] 是否使用了类型安全的现代特性？

## 13. 参考资源

- [C++20 标准文档](https://en.cppreference.com/w/cpp/20)
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- [CUDA C++ Programming Guide](https://docs.nvidia.com/cuda/cuda-c-programming-guide/)

---

**最后更新**: 2026-03-08  
**适用版本**: C++20, CUDA 13.1, OptiX 8.0
