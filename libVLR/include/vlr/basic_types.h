// ============================================================================
// VLR 基础类型定义
// 
// 本文件定义了 VLR 中使用的基础数学类型。
// 
// 作者：VLR 开发团队
// 创建日期：2026-03-07
// 环境：CUDA 13.1, OptiX 8.0.0, VS2022
// ============================================================================

#pragma once

#include <cstdint>
#include <cmath>

// 平台检测
#ifdef __CUDACC__
    #define CUDA_DEVICE_FUNCTION __device__
    #define CUDA_HOST_FUNCTION __host__
    #define CUDA_INLINE __forceinline__
    #define CUDA_DEVICE_KERNEL extern "C" __global__
#else
    #define CUDA_DEVICE_FUNCTION
    #define CUDA_HOST_FUNCTION
    #define CUDA_INLINE inline
    #define CUDA_DEVICE_KERNEL
#endif

namespace vlr {

// ============================================================================
// 数学常量
// ============================================================================

constexpr float VLR_M_PI = 3.14159265358979323846f;
constexpr float VLR_M_2PI = 6.28318530717958647692f;
constexpr float VLR_M_INV_PI = 0.31830988618379067154f;
constexpr float VLR_M_INV_2PI = 0.15915494309189533577f;

/// 设备端安全的 max/min（替代 std::max/min，避免 CUDA 设备代码中的链接问题）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE float vlr_max(float a, float b) {
    return (a > b) ? a : b;
}
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE float vlr_min(float a, float b) {
    return (a < b) ? a : b;
}


// ============================================================================
// 向量类型
// ============================================================================

struct Vector3D {
    float x, y, z;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D() : x(0), y(0), z(0) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D operator+(const Vector3D& v) const {
        return Vector3D(x + v.x, y + v.y, z + v.z);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D operator-(const Vector3D& v) const {
        return Vector3D(x - v.x, y - v.y, z - v.z);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D operator*(float s) const {
        return Vector3D(x * s, y * s, z * s);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D operator/(float s) const {
        float inv = 1.0f / s;
        return Vector3D(x * inv, y * inv, z * inv);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D operator-() const {
        return Vector3D(-x, -y, -z);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    void toPolarYUp(float* theta, float* phi) const {
        *theta = std::acos(y);
        *phi = std::atan2(z, x);
        if (*phi < 0) *phi += VLR_M_2PI;
    }
};

using Point3D = Vector3D;
using Normal3D = Vector3D;


struct Vector2D {
    float x, y;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector2D() : x(0), y(0) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector2D(float x_, float y_) : x(x_), y(y_) {}
};

using TexCoord2D = Vector2D;


// ============================================================================
// CUDA 向量类型（用于兼容性）
// ============================================================================

#ifndef __CUDACC__
struct uint2 {
    uint32_t x, y;
    
    uint2() : x(0), y(0) {}
    uint2(uint32_t x_, uint32_t y_) : x(x_), y(y_) {}
};

inline uint2 make_uint2(uint32_t x, uint32_t y) {
    return uint2(x, y);
}
#endif


// ============================================================================
// 向量运算
// ============================================================================

/// Scalar * Vector (commutative)
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Vector3D operator*(float s, const Vector3D& v) {
    return v * s;
}

CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
float dot(const Vector3D& a, const Vector3D& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
float absDot(const Vector3D& a, const Vector3D& b) {
    return std::abs(dot(a, b));
}

CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Vector3D cross(const Vector3D& a, const Vector3D& b) {
    return Vector3D(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
float length(const Vector3D& v) {
    return std::sqrt(dot(v, v));
}

CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
Vector3D normalize(const Vector3D& v) {
    return v / length(v);
}


// ============================================================================
// 参考坐标系
// ============================================================================

struct ReferenceFrame {
    Vector3D x, y, z;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    ReferenceFrame() : x(1, 0, 0), y(0, 1, 0), z(0, 0, 1) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    ReferenceFrame(const Vector3D& tangent, const Normal3D& normal) {
        z = normal;
        x = normalize(tangent - z * dot(tangent, z));
        y = cross(z, x);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D toLocal(const Vector3D& v) const {
        return Vector3D(dot(v, x), dot(v, y), dot(v, z));
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    Vector3D toWorld(const Vector3D& v) const {
        return x * v.x + y * v.y + z * v.z;
    }
};


// ============================================================================
// 光谱类型
// ============================================================================

constexpr uint32_t NumSpectralSamples = 4;

struct WavelengthSamples;   // 前向声明，用于 toDiscretizedSpectrum
struct DiscretizedSpectrum;  // 前向声明，用于 toDiscretizedSpectrum 返回类型

struct SampledSpectrum {
    float values[NumSpectralSamples];
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    SampledSpectrum() {
        for (int i = 0; i < NumSpectralSamples; ++i)
            values[i] = 0.0f;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    explicit SampledSpectrum(float v) {
        for (int i = 0; i < NumSpectralSamples; ++i)
            values[i] = v;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static SampledSpectrum Zero() {
        return SampledSpectrum(0.0f);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static SampledSpectrum One() {
        return SampledSpectrum(1.0f);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    SampledSpectrum operator+(const SampledSpectrum& s) const {
        SampledSpectrum result;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result.values[i] = values[i] + s.values[i];
        return result;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    SampledSpectrum operator*(const SampledSpectrum& s) const {
        SampledSpectrum result;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result.values[i] = values[i] * s.values[i];
        return result;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    SampledSpectrum operator*(float s) const {
        SampledSpectrum result;
        for (int i = 0; i < NumSpectralSamples; ++i)
            result.values[i] = values[i] * s;
        return result;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    SampledSpectrum operator/(float s) const {
        return (*this) * (1.0f / s);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    void operator+=(const SampledSpectrum& s) {
        for (int i = 0; i < NumSpectralSamples; ++i)
            values[i] += s.values[i];
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    void operator*=(const SampledSpectrum& s) {
        for (int i = 0; i < NumSpectralSamples; ++i)
            values[i] *= s.values[i];
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    void operator/=(float s) {
        float inv = 1.0f / s;
        for (int i = 0; i < NumSpectralSamples; ++i)
            values[i] *= inv;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool operator==(const SampledSpectrum& s) const {
        for (int i = 0; i < NumSpectralSamples; ++i)
            if (values[i] != s.values[i])
                return false;
        return true;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool hasNonZero() const {
        for (int i = 0; i < NumSpectralSamples; ++i)
            if (values[i] != 0.0f)
                return true;
        return false;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    float importance(uint32_t selectedLambdaIndex) const {
        return values[selectedLambdaIndex];
    }

    /// 检查所有光谱分量是否为有限值（非 NaN、非 Inf）
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool allFinite() const {
        for (int i = 0; i < NumSpectralSamples; ++i) {
#ifdef __CUDACC__
            float v = values[i];
            if (__isnanf(v) || __isinf(v))
#else
            if (!std::isfinite(values[i]))
#endif
                return false;
        }
        return true;
    }

    /// 将采样光谱转换为 RGB（DiscretizedSpectrum），定义见 WavelengthSamples 之后
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    DiscretizedSpectrum toDiscretizedSpectrum(const WavelengthSamples& wls) const;
};

struct DiscretizedSpectrum {
    float r, g, b;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    DiscretizedSpectrum() : r(0), g(0), b(0) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    DiscretizedSpectrum(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
};

using SpectrumStorage = DiscretizedSpectrum;


struct WavelengthSamples {
    float lambdas[NumSpectralSamples];  // 16 字节
    uint32_t selectedLambda;            // 4 字节
    uint32_t _padding;                  // 4 字节（对齐到 24 字节）
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    WavelengthSamples() : selectedLambda(0), _padding(0) {
        for (int i = 0; i < NumSpectralSamples; ++i)
            lambdas[i] = 550.0f;
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    uint32_t selectedLambdaIndex() const {
        return selectedLambda;
    }
    
    /// 使用等间隔偏移创建波长采样（4 个光谱采样）
    /// u1: 波长偏移 [0,1)，u2: 选择索引 [0,1)，selectWLPDF 输出波长选择 PDF
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static WavelengthSamples createWithEqualOffsets(float u1, float u2, float* selectWLPDF) {
        constexpr float lambdaMin = 360.0f;
        constexpr float lambdaMax = 830.0f;
        constexpr float lambdaSpan = lambdaMax - lambdaMin;
        
        WavelengthSamples wls;
        for (int i = 0; i < NumSpectralSamples; ++i) {
            wls.lambdas[i] = lambdaMin + (i + u1) * (lambdaSpan / NumSpectralSamples);
        }
        wls.selectedLambda = static_cast<uint32_t>(u2 * NumSpectralSamples) % NumSpectralSamples;
        wls._padding = 0;
        *selectWLPDF = 1.0f / NumSpectralSamples;
        return wls;
    }
};

static_assert(sizeof(WavelengthSamples) == 24, "WavelengthSamples 必须为 24 字节");

// toDiscretizedSpectrum 实现在此（需要 WavelengthSamples 完整定义）
CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
DiscretizedSpectrum SampledSpectrum::toDiscretizedSpectrum(const WavelengthSamples& wls) const {
    float luminance = values[wls.selectedLambdaIndex() % NumSpectralSamples];
    return DiscretizedSpectrum(luminance, luminance, luminance);
}


// ============================================================================
// 方向类型
// ============================================================================

struct DirectionType {
    uint32_t data;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    DirectionType() : data(0) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    explicit DirectionType(uint32_t d) : data(d) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static DirectionType All() { return DirectionType(0xFFFFFFFF); }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static DirectionType Reflection() { return DirectionType(0x1); }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static DirectionType Transmission() { return DirectionType(0x2); }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static DirectionType Delta0D() { return DirectionType(0x4); }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    static DirectionType HighFreq() { return DirectionType(0x8); }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    DirectionType operator|(const DirectionType& other) const {
        return DirectionType(data | other.data);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    bool isDelta() const {
        return (data & 0x4) != 0;
    }
};


// ============================================================================
// 随机数生成器
// ============================================================================

struct KernelRNG {
    uint64_t state;
    uint64_t inc;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    KernelRNG() : state(0x853c49e6748fea9bULL), inc(0xda3e39cb94b95bdbULL) {}
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    uint32_t next() {
        uint64_t oldstate = state;
        state = oldstate * 6364136223846793005ULL + inc;
        uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
        uint32_t rot = oldstate >> 59u;
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    float getFloat0cTo1o() {
        return (next() >> 8) * 0x1.0p-24f;
    }
};

static_assert(sizeof(KernelRNG) == 16, "KernelRNG 必须为 16 字节");


// ============================================================================
// 表面点
// ============================================================================

struct SurfacePoint {
    Point3D position;
    Normal3D geometricNormal;
    ReferenceFrame shadingFrame;
    TexCoord2D texCoord;
    bool atInfinity;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    SurfacePoint() : atInfinity(false) {}
};


// ============================================================================
// 相机类型与描述符
// ============================================================================

/// 相机类型枚举（与 VLRCameraType 对应）
enum CameraType : uint32_t {
    CameraType_Perspective = 0,
    CameraType_Equirectangular,
    NumCameraTypes
};

struct CameraDescriptor {
    Point3D position;
    ReferenceFrame orientation;
    float fovY;
    float aspect;
    float lensRadius;
    float focusDistance;
    uint32_t cameraType;     ///< CameraType 枚举值
    int32_t progEvaluateIDF;
    
    CUDA_DEVICE_FUNCTION CUDA_HOST_FUNCTION CUDA_INLINE
    CameraDescriptor() 
        : position(0, 0, 0)
        , fovY(45.0f * VLR_M_PI / 180.0f)
        , aspect(16.0f / 9.0f)
        , lensRadius(0.0f)
        , focusDistance(1.0f)
        , cameraType(CameraType_Perspective)
        , progEvaluateIDF(-1)
    {}
};


// ============================================================================
// 几何类型
// ============================================================================

enum GeometryType : uint32_t {
    GeometryType_TriangleMesh = 0,
    GeometryType_InfiniteSphere,
    GeometryType_Point,
    NumGeometryTypes
};


struct Triangle {
    uint32_t indices[3];
    float area;
};


struct GeometryInstance {
    GeometryType geomType;
    uint32_t instIndex;
    uint32_t materialIndex;
    float importance;
    int32_t progDecodeHitPoint;
    int32_t progSampleLightPosition;
    int32_t nodeNormal;
    int32_t nodeTangent;
    
    union {
        struct {
            const Triangle* triangleBuffer;
        } asTriMesh;
        
        struct {
            uint32_t importanceMap;
        } asInfSphere;
    };
};


struct Instance {
    uint32_t* geomInstIndices;
    uint32_t numGeomInsts;
    ReferenceFrame transform;
    float rotationPhi;
    uint32_t lightGeomInstDistribution;
};


// ============================================================================
// 材质描述符（占位符）
// ============================================================================

struct NodeProcedureSet {
    int32_t progs[8];
};

struct SmallNodeDescriptor {
    uint32_t data[4];
};

struct MediumNodeDescriptor {
    uint32_t data[8];
};

struct LargeNodeDescriptor {
    uint32_t data[16];
};

struct BSDFProcedureSet {
    int32_t progGetBaseColor;
    int32_t progMatches;
    int32_t progSample;
    int32_t progEvaluate;
    int32_t progEvaluatePDF;
};

struct EDFProcedureSet {
    int32_t progEvaluateEmittance;
    int32_t progEvaluate;
};

struct IDFProcedureSet {
    int32_t progEvaluate;
};

struct SurfaceMaterialDescriptor {
    uint32_t bsdfProcedureSetIndex;
    uint32_t edfProcedureSetIndex;
    uint32_t data[16];
};


// ============================================================================
// 传输模式
// ============================================================================

enum class TransportMode : uint32_t {
    Radiance = 0,
    Importance
};


// ============================================================================
// BSDF/EDF 查询类型（占位符）
// ============================================================================

template <TransportMode mode>
struct BSDF {
    const SurfaceMaterialDescriptor* matDesc;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    bool matches(DirectionType type) const {
        return true;  // 占位符
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    SampledSpectrum evaluate(const struct BSDFQuery& query) const {
        return SampledSpectrum::Zero();  // 占位符
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    float evaluatePDF(const struct BSDFQuery& query) const {
        return 0.0f;  // 占位符
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    SampledSpectrum sample(const struct BSDFQuery& query, float u0, float u1, struct BSDFSample* sample) const {
        return SampledSpectrum::Zero();  // 占位符
    }
};

struct BSDFQuery {
    Vector3D dirIn;
    Vector3D dirOut;
    WavelengthSamples wls;
    DirectionType dirTypeFilter;
    TransportMode transportMode;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    BSDFQuery(const Vector3D& dirIn_, const Vector3D& dirOut_, 
              const WavelengthSamples& wls_, DirectionType filter, TransportMode mode)
        : dirIn(dirIn_), dirOut(dirOut_), wls(wls_), dirTypeFilter(filter), transportMode(mode) {}
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    BSDFQuery(const Vector3D& dirIn_, const WavelengthSamples& wls_, 
              DirectionType filter, TransportMode mode)
        : dirIn(dirIn_), wls(wls_), dirTypeFilter(filter), transportMode(mode) {}
};

struct BSDFSample {
    Vector3D dirLocal;
    float dirPDF;
    DirectionType dirType;
};

struct EDF {
    const SurfaceMaterialDescriptor* matDesc;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    EDF(const SurfaceMaterialDescriptor& desc, const SurfacePoint& sp, const WavelengthSamples& wls) 
        : matDesc(&desc) {}
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    SampledSpectrum evaluateEmittance() const {
        return SampledSpectrum::Zero();  // 占位符
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    SampledSpectrum evaluate(const struct EDFQuery& query, const Vector3D& dir) const {
        return SampledSpectrum::Zero();  // 占位符
    }
};

struct EDFQuery {
    DirectionType dirTypeFilter;
    WavelengthSamples wls;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    EDFQuery(DirectionType filter, const WavelengthSamples& wls_)
        : dirTypeFilter(filter), wls(wls_) {}
};


// ============================================================================
// 采样结构
// ============================================================================

struct LensPosSample {
    Point3D position;
    float areaPDF;
};

struct IDFSample {
    Vector3D dirLocal;
    Point3D positionLocal;
    float dirPDF;
};

struct LightPosSample {
    SurfacePoint surfPt;
    float areaPDF;
};


// ============================================================================
// 分布类型（占位符）
// ============================================================================

struct DiscreteDistribution1D {
    float* weights;
    uint32_t numValues;
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    uint32_t sample(float u, float* prob) const {
        *prob = 1.0f / numValues;
        return static_cast<uint32_t>(u * numValues);
    }
    
    CUDA_DEVICE_FUNCTION CUDA_INLINE
    float integral() const {
        return 1.0f;
    }
};


// ============================================================================
// 场景边界
// ============================================================================

struct SceneBounds {
    Point3D minPoint;
    Point3D maxPoint;
};


// ============================================================================
// OptiX 工具类型（占位符）
// ============================================================================

namespace optixu {
    template <typename T>
    struct PayloadSignature {
        using Type = T;
    };
    
    template <typename T>
    struct NativeBlockBuffer2D {
        T* data;
    };
    
    template <typename T, int N>
    struct BlockBuffer2D {
        T* data;
    };
}


// ============================================================================
// CMF 类型（占位符）
// ============================================================================

struct DiscretizedSpectrumAlwaysSpectral {
    struct CMF {
        float values[32];
    };
};


// ============================================================================
// 程序签名（占位符）
// ============================================================================

#define ProgSigDecodeHitPoint int32_t
#define ProgSigSampleLensPosition int32_t
#define ProgSigEvaluateIDF int32_t
#define ProgSigSampleLightPosition int32_t


// ============================================================================
// 工具函数
// ============================================================================

CUDA_DEVICE_FUNCTION CUDA_INLINE
void applyBumpMapping(const Normal3D& localNormal, SurfacePoint* surfPt) {
    // 占位符
}

CUDA_DEVICE_FUNCTION CUDA_INLINE
void modifyTangent(const Vector3D& newTangent, SurfacePoint* surfPt) {
    // 占位符
}

template <typename T>
CUDA_DEVICE_FUNCTION CUDA_INLINE
T calcNode(int32_t nodeIndex, const T& defaultValue, const SurfacePoint& surfPt, const WavelengthSamples& wls) {
    return defaultValue;  // 占位符
}

#ifdef __CUDACC__
    #define vlrprintf printf
    #define atomicAdd ::atomicAdd
    #define atomicSub ::atomicSub
#else
    #define vlrprintf printf
    inline void atomicAdd(float* addr, float val) { *addr += val; }
    inline void atomicAdd(uint32_t* addr, uint32_t val) { *addr += val; }
    inline uint32_t atomicSub(uint32_t* addr, uint32_t val) { 
        uint32_t old = *addr; 
        *addr -= val; 
        return old; 
    }
#endif

} // namespace vlr
