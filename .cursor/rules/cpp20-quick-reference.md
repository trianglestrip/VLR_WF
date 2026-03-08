# C++20 现代特性快速参考

> 本项目强制使用 C++20 现代特性，提升代码质量和安全性。

## 🚀 必须使用的特性

### 1. 智能指针 - 自动内存管理

```cpp
// ✅ 使用 unique_ptr
std::unique_ptr<Buffer> buf = std::make_unique<Buffer>();
buf.reset();  // 释放

// ✅ 使用 shared_ptr（共享所有权）
std::shared_ptr<Scene> scene = std::make_shared<Scene>();

// ❌ 禁止手动 new/delete
Buffer* buf = new Buffer();  // 禁止！
delete buf;
```

### 2. std::optional - 可选值

```cpp
// ✅ 表示可能不存在的值
std::optional<uint32_t> index;

if (index.has_value()) {
    uint32_t val = *index;
}

uint32_t val = index.value_or(0);  // 默认值

// ❌ 禁止魔术数字
uint32_t index = 0xFFFFFFFF;  // 禁止！
```

### 3. constexpr - 编译期常量

```cpp
// ✅ 编译期常量
constexpr uint32_t MaxSize = 1024;
constexpr float Pi = 3.14159f;

// ❌ 禁止宏定义
#define MAX_SIZE 1024  // 禁止！
```

### 4. std::array - 类型安全数组

```cpp
// ✅ 使用 std::array
std::array<float, 3> color = {1.0f, 0.0f, 0.0f};
std::array<std::unique_ptr<Buffer>, 8> buffers;

// ❌ 禁止 C 数组
float color[3];  // 尽量避免
```

## 💡 推荐使用的特性

### 5. std::span - 零拷贝视图

```cpp
// ✅ 内部实现使用 span
void process(std::span<const float> data) {
    for (float v : data) { ... }
}

// ❌ 避免分离指针和大小
void process(const float* data, size_t size) { ... }
```

### 6. Concepts - 类型约束

```cpp
// ✅ 定义概念
template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

template<Numeric T>
T add(T a, T b) { return a + b; }
```

### 7. std::ranges - 简化操作

```cpp
// ✅ 使用 ranges
auto result = data 
    | std::views::filter([](auto x) { return x > 0; })
    | std::views::transform([](auto x) { return x * 2; });
```

### 8. 结构化绑定

```cpp
// ✅ 解构返回值
auto [width, height] = getSize();

// ✅ 遍历 map
for (const auto& [key, value] : map) { ... }
```

## ⚠️ CUDA 代码限制

### 可以使用
- `constexpr`
- `std::array`
- 智能指针（主机端）

### 不能使用
- Designated Initializers
- `std::format`
- 复杂的 ranges

### 示例

```cpp
// ✅ 主机端
std::unique_ptr<cudau::Buffer<float>> hostBuf;

// ✅ 设备端
__global__ void kernel(float* devPtr, size_t size) {
    // 使用原始指针
}
```

## 📋 代码审查检查清单

- [ ] 使用智能指针？
- [ ] 使用 `std::optional` 替代魔术数字？
- [ ] 常量使用 `constexpr`？
- [ ] 避免手动 `new`/`delete`？
- [ ] 使用 `std::array` 替代 C 数组？
- [ ] CUDA 代码避免不支持的特性？

## 🔗 详细文档

参见 `cpp20-modern-features.md` 获取完整规范。

---

**记住**: 现代 C++ = 更安全 + 更清晰 + 更高效
