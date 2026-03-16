// ============================================================================
// PbrtSceneData - PBRT v4 场景中间表示
//
// 存储从 .pbrt 文件解析出的原始数据，作为解析器输出 / 构建器输入。
// 设计原则：
//   - 所有字符串用 std::string（名称引用），大批量数值用 std::vector<float>
//   - 禁用拷贝，强制移动语义
//   - 枚举而非魔法字符串表示材质类型
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <variant>
#include <optional>
#include <memory>

namespace viewer {

// ----------------------------------------------------------------------------
// 基础类型别名
// ----------------------------------------------------------------------------
using Vec3f = std::array<float, 3>;
using Vec2f = std::array<float, 2>;
using Mat4f = std::array<float, 16>;  // 行主序

// ----------------------------------------------------------------------------
// 纹理描述
// ----------------------------------------------------------------------------
enum class PbrtTextureType { Spectrum, Float };

struct PbrtTexture {
    std::string name;
    PbrtTextureType type = PbrtTextureType::Spectrum;
    std::string filename;           // imagemap 类型
    std::string filterMode = "trilinear";

    // 禁用拷贝
    PbrtTexture() = default;
    PbrtTexture(PbrtTexture&&) noexcept = default;
    PbrtTexture& operator=(PbrtTexture&&) noexcept = default;
    PbrtTexture(const PbrtTexture&) = delete;
    PbrtTexture& operator=(const PbrtTexture&) = delete;
};

// ----------------------------------------------------------------------------
// 材质描述（对应 MakeNamedMaterial / Material 指令）
// ----------------------------------------------------------------------------
enum class PbrtMaterialType {
    Diffuse,
    CoatedDiffuse,
    Conductor,
    Dielectric,
    DiffuseTransmission,
    Unknown
};

// 参数值：标量、RGB 或纹理名引用
using PbrtParamValue = std::variant<
    float,
    Vec3f,
    std::string,     // 纹理名称引用
    bool
>;

struct PbrtMaterial {
    std::string name;
    PbrtMaterialType type = PbrtMaterialType::Diffuse;

    // 通用参数，键值对保存原始数据
    std::optional<Vec3f>       reflectance;       // rgb reflectance
    std::optional<Vec3f>       transmittance;      // rgb transmittance
    std::optional<Vec3f>       conductorEta;       // rgb eta（导体）
    std::optional<Vec3f>       conductorK;         // rgb k（导体）
    std::optional<float>       dielectricEta;      // float eta（电介质）
    std::optional<float>       roughness;          // 各向同性
    std::optional<float>       uRoughness;
    std::optional<float>       vRoughness;
    bool                       remapRoughness = true;
    std::string                reflectanceTexture; // 引用纹理名，优先于 reflectance
    std::string                displacementTexture;
    std::string                spectrumEta;        // 具名光谱（如 "metal-Ag-eta"）
    std::string                spectrumK;

    PbrtMaterial() = default;
    PbrtMaterial(PbrtMaterial&&) noexcept = default;
    PbrtMaterial& operator=(PbrtMaterial&&) noexcept = default;
    PbrtMaterial(const PbrtMaterial&) = delete;
    PbrtMaterial& operator=(const PbrtMaterial&) = delete;
};

// ----------------------------------------------------------------------------
// 形状（Shape）
// ----------------------------------------------------------------------------
enum class PbrtShapeType { PlyMesh, TriangleMesh, Unknown };

struct PbrtPlyMesh {
    std::string filename;   // 相对于 .pbrt 文件的路径
};

struct PbrtTriangleMesh {
    std::vector<float>    positions;   // point3 P，交错 x,y,z
    std::vector<float>    normals;     // normal N，交错 x,y,z
    std::vector<float>    uvs;         // point2 uv，交错 u,v
    std::vector<uint32_t> indices;     // integer indices
};

using PbrtShapeGeometry = std::variant<PbrtPlyMesh, PbrtTriangleMesh>;

struct PbrtShape {
    PbrtShapeType     type = PbrtShapeType::Unknown;
    PbrtShapeGeometry geometry;
    std::string       materialName;   // 当前绑定的具名材质（可为空）
    bool              isAreaLight = false;
    Vec3f             areaLightL = {0.f, 0.f, 0.f};

    PbrtShape() = default;
    PbrtShape(PbrtShape&&) noexcept = default;
    PbrtShape& operator=(PbrtShape&&) noexcept = default;
    PbrtShape(const PbrtShape&) = delete;
    PbrtShape& operator=(const PbrtShape&) = delete;
};

// ----------------------------------------------------------------------------
// 相机
// ----------------------------------------------------------------------------
struct PbrtCamera {
    std::string type = "perspective";
    float fov = 45.0f;
    Mat4f cameraToWorld = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
};

// ----------------------------------------------------------------------------
// 渲染器配置（Film / Sampler / Integrator）
// ----------------------------------------------------------------------------
struct PbrtFilm {
    std::string filename = "output.png";
    uint32_t xResolution = 1280;
    uint32_t yResolution = 720;
};

struct PbrtSampler {
    std::string type = "sobol";
    uint32_t pixelSamples = 64;
};

struct PbrtIntegrator {
    std::string type = "path";
    uint32_t maxDepth = 8;
};

// ----------------------------------------------------------------------------
// 整体场景描述
// ----------------------------------------------------------------------------
struct PbrtSceneData {
    PbrtCamera    camera;
    PbrtFilm      film;
    PbrtSampler   sampler;
    PbrtIntegrator integrator;

    // 使用 unordered_map 按名称快速查找
    std::unordered_map<std::string, PbrtTexture>  textures;
    std::unordered_map<std::string, PbrtMaterial> namedMaterials;

    // 形状列表（顺序有意义，按文件中的顺序存储）
    std::vector<PbrtShape> shapes;

    // 根目录（用于解析相对路径）
    std::string baseDir;

    PbrtSceneData() = default;
    PbrtSceneData(PbrtSceneData&&) noexcept = default;
    PbrtSceneData& operator=(PbrtSceneData&&) noexcept = default;
    PbrtSceneData(const PbrtSceneData&) = delete;
    PbrtSceneData& operator=(const PbrtSceneData&) = delete;
};

} // namespace viewer
