// ============================================================================
// PbrtParser.cpp - PBRT v4 解析器实现
//
// 解析流程（三阶段流水线）：
//   Phase 1 [单线程]  Lexer  → Token 流（string_view 切片，零拷贝）
//   Phase 2 [单线程]  Parser → PbrtSceneData（填充 Shape/Material/Texture）
//   Phase 3 [并行]    PLY 加载 → 将各 PlyMesh 几何填充为 PbrtTriangleMesh
//
// PLY 并行加载使用 Taskflow：
//   - 每个 plymesh shape 对应一个独立任务
//   - 所有任务无依赖，完全并行
//   - 通过预分配 shapes 槽位避免并发写冲突（无锁）
// ============================================================================

#include "PbrtParser.h"
#include "PbrtSceneData.h"

#include <taskflow.hpp>

#include <fstream>
#include <sstream>
#include <iostream>
#include <charconv>
#include <filesystem>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <span>

namespace fs = std::filesystem;

namespace viewer {

// ============================================================================
// 辅助函数
// ============================================================================

static PbrtMaterialType parseMaterialType(std::string_view sv) {
    if (sv == "diffuse")             return PbrtMaterialType::Diffuse;
    if (sv == "coateddiffuse")       return PbrtMaterialType::CoatedDiffuse;
    if (sv == "conductor")           return PbrtMaterialType::Conductor;
    if (sv == "dielectric")          return PbrtMaterialType::Dielectric;
    if (sv == "diffusetransmission") return PbrtMaterialType::DiffuseTransmission;
    return PbrtMaterialType::Unknown;
}

static float parseFloatSV(std::string_view sv, float fallback = 0.f) {
    float v = fallback;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

static bool parseBoolSV(std::string_view sv) {
    return sv == "true";
}

// ============================================================================
// Lexer — 将整个文件内容切分为 Token（string_view 零拷贝）
// ============================================================================
namespace {

struct Token {
    std::string_view sv;
    bool isString = false;  // 是否来自引号字符串
};

class Lexer {
public:
    explicit Lexer(std::string& src) : m_src(src), m_pos(0) {}

    bool next(Token& tok) {
        skipWhitespaceAndComments();
        if (m_pos >= m_src.size()) return false;

        if (m_src[m_pos] == '"') {
            // 引号字符串
            ++m_pos;
            size_t start = m_pos;
            while (m_pos < m_src.size() && m_src[m_pos] != '"') ++m_pos;
            tok.sv = std::string_view(m_src.data() + start, m_pos - start);
            tok.isString = true;
            if (m_pos < m_src.size()) ++m_pos; // 跳过结尾 "
            return true;
        }

        if (m_src[m_pos] == '[') {
            tok.sv = std::string_view(m_src.data() + m_pos, 1);
            tok.isString = false;
            ++m_pos;
            return true;
        }
        if (m_src[m_pos] == ']') {
            tok.sv = std::string_view(m_src.data() + m_pos, 1);
            tok.isString = false;
            ++m_pos;
            return true;
        }

        // 普通标记（到空白/[]/"为止）
        size_t start = m_pos;
        while (m_pos < m_src.size() &&
               m_src[m_pos] != ' ' && m_src[m_pos] != '\t' &&
               m_src[m_pos] != '\n' && m_src[m_pos] != '\r' &&
               m_src[m_pos] != '"' &&
               m_src[m_pos] != '[' && m_src[m_pos] != ']') {
            ++m_pos;
        }
        tok.sv = std::string_view(m_src.data() + start, m_pos - start);
        tok.isString = false;
        return !tok.sv.empty();
    }

    bool peek(Token& tok) {
        size_t save = m_pos;
        bool ok = next(tok);
        m_pos = save;
        return ok;
    }

private:
    void skipWhitespaceAndComments() {
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else if (c == '#') {
                // 行注释
                while (m_pos < m_src.size() && m_src[m_pos] != '\n') ++m_pos;
            } else {
                break;
            }
        }
    }

    std::string& m_src;
    size_t m_pos;
};

// ============================================================================
// 参数列表解析辅助
// ============================================================================

// 读取 [ v1 v2 ... ] 中的所有浮点数（也支持省略方括号的单值）
static std::vector<float> readFloatList(Lexer& lex) {
    std::vector<float> result;
    Token tok;
    if (!lex.peek(tok)) return result;

    bool hasBracket = (tok.sv == "[");
    if (hasBracket) lex.next(tok); // 消耗 [

    while (lex.peek(tok)) {
        if (tok.sv == "]") { lex.next(tok); break; }
        if (tok.sv == "[") break; // 嵌套错误，安全退出
        // 如果遇到一个新关键字（大写开头），停止
        if (!tok.isString && !tok.sv.empty() &&
            std::isupper(static_cast<unsigned char>(tok.sv[0]))) break;
        lex.next(tok);
        float v = 0.f;
        auto [ptr, ec] = std::from_chars(tok.sv.data(), tok.sv.data() + tok.sv.size(), v);
        if (ec == std::errc{}) {
            result.push_back(v);
        } else {
            // 非数字，退回
            break;
        }
        if (!hasBracket) break; // 无括号时只读一个值
    }
    return result;
}

static std::string readStringValue(Lexer& lex) {
    Token tok;
    if (!lex.peek(tok)) return {};
    bool hasBracket = (tok.sv == "[");
    if (hasBracket) lex.next(tok);
    if (!lex.next(tok)) return {};
    std::string val(tok.sv);
    if (hasBracket) {
        // 消耗 ]
        if (lex.peek(tok) && tok.sv == "]") lex.next(tok);
    }
    return val;
}

static bool readBoolValue(Lexer& lex) {
    Token tok;
    if (!lex.peek(tok)) return false;
    bool hasBracket = (tok.sv == "[");
    if (hasBracket) lex.next(tok);
    if (!lex.next(tok)) return false;
    bool val = parseBoolSV(tok.sv);
    if (hasBracket) {
        if (lex.peek(tok) && tok.sv == "]") lex.next(tok);
    }
    return val;
}

// ============================================================================
// 参数块解析（"type name" [ values ] ...）
// 解析到下一个 PBRT 关键字为止
// ============================================================================

struct ParamBlock {
    // 参数类型前缀
    std::string paramType;   // "string", "float", "rgb", "integer", "bool", etc.
    std::string paramName;   // 参数名
    std::vector<float> floatVals;
    std::string strVal;
    bool boolVal = false;
};

// 将 "rgb reflectance" 拆分为 type="rgb", name="reflectance"
static bool splitTypeAndName(std::string_view s, std::string& type, std::string& name) {
    auto space = s.find(' ');
    if (space == std::string_view::npos) return false;
    type = std::string(s.substr(0, space));
    name = std::string(s.substr(space + 1));
    return true;
}

// PBRT v4 顶层关键字集合
static bool isPbrtKeyword(std::string_view sv) {
    static const char* kws[] = {
        "Integrator","Transform","Sampler","PixelFilter","Film","Camera",
        "WorldBegin","WorldEnd","Texture","MakeNamedMaterial","NamedMaterial",
        "Material","Shape","AttributeBegin","AttributeEnd","AreaLightSource",
        "LightSource","Include","Scale","Rotate","Translate","ConcatTransform",
        nullptr
    };
    for (int i = 0; kws[i]; ++i) {
        if (sv == kws[i]) return true;
    }
    return false;
}

// 读取当前指令后续的所有参数块（直到下一个关键字）
static std::vector<ParamBlock> readParams(Lexer& lex) {
    std::vector<ParamBlock> params;
    Token tok;
    while (lex.peek(tok)) {
        // 停止条件：遇到 PBRT 关键字（非引号）
        if (!tok.isString && isPbrtKeyword(tok.sv)) break;
        // 停止条件：遇到裸 "[" 或 "]"
        if (!tok.isString && (tok.sv == "[" || tok.sv == "]")) break;

        lex.next(tok); // 消耗 "type name" 引号字符串

        if (!tok.isString) {
            // 不是引号字符串，跳过
            continue;
        }

        ParamBlock pb;
        if (!splitTypeAndName(tok.sv, pb.paramType, pb.paramName)) continue;

        if (pb.paramType == "string" || pb.paramType == "spectrum") {
            pb.strVal = readStringValue(lex);
        } else if (pb.paramType == "bool") {
            pb.boolVal = readBoolValue(lex);
        } else {
            pb.floatVals = readFloatList(lex);
        }
        params.push_back(std::move(pb));
    }
    return params;
}

// ============================================================================
// PLY 轻量解析器
// 支持：binary little endian / ascii，element vertex / face
// 零额外依赖（不依赖 happly 等库）
// ============================================================================

struct PlyData {
    std::vector<float>    positions;  // x,y,z interleaved
    std::vector<float>    normals;    // nx,ny,nz interleaved
    std::vector<float>    uvs;        // u,v interleaved
    std::vector<uint32_t> indices;
    bool ok = false;
};

static PlyData loadPly(const std::string& path) {
    PlyData data;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[PbrtParser] 无法打开 PLY: " << path << "\n";
        return data;
    }

    // ---- 读取 header ----
    enum class Format { Ascii, BinaryLE, BinaryBE };
    Format fmt = Format::BinaryLE;

    struct PropDesc { std::string name; std::string type; };
    struct ElemDesc {
        std::string name;
        size_t count = 0;
        std::vector<PropDesc> props;
        bool isList = false;         // face element
        std::string listCountType;
        std::string listElemType;
    };

    std::vector<ElemDesc> elements;
    ElemDesc* curElem = nullptr;

    std::string line;
    while (std::getline(f, line)) {
        // 去掉 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "end_header") break;

        std::istringstream ss(line);
        std::string kw; ss >> kw;

        if (kw == "format") {
            std::string fmtStr; ss >> fmtStr;
            if (fmtStr == "ascii")                  fmt = Format::Ascii;
            else if (fmtStr == "binary_big_endian") fmt = Format::BinaryBE;
            else                                    fmt = Format::BinaryLE;
        } else if (kw == "element") {
            elements.emplace_back();
            curElem = &elements.back();
            ss >> curElem->name >> curElem->count;
        } else if (kw == "property" && curElem) {
            std::string typeOrList; ss >> typeOrList;
            if (typeOrList == "list") {
                curElem->isList = true;
                ss >> curElem->listCountType >> curElem->listElemType;
                PropDesc pd; pd.name = "indices"; pd.type = "list";
                curElem->props.push_back(pd);
            } else {
                PropDesc pd; ss >> pd.name; pd.type = typeOrList;
                curElem->props.push_back(pd);
            }
        }
    }

    // ---- 找到 vertex 和 face element ----
    ElemDesc* vertElem = nullptr;
    ElemDesc* faceElem = nullptr;
    for (auto& e : elements) {
        if (e.name == "vertex") vertElem = &e;
        else if (e.name == "face") faceElem = &e;
    }

    if (!vertElem) return data;

    // ---- 确定各属性在 vertex 中的索引 ----
    int idxX = -1, idxY = -1, idxZ = -1;
    int idxNX = -1, idxNY = -1, idxNZ = -1;
    int idxU = -1, idxV = -1;
    for (int i = 0; i < (int)vertElem->props.size(); ++i) {
        const auto& n = vertElem->props[i].name;
        if      (n == "x")  idxX  = i;
        else if (n == "y")  idxY  = i;
        else if (n == "z")  idxZ  = i;
        else if (n == "nx") idxNX = i;
        else if (n == "ny") idxNY = i;
        else if (n == "nz") idxNZ = i;
        else if (n == "s" || n == "u" || n == "texture_u") idxU = i;
        else if (n == "t" || n == "v" || n == "texture_v") idxV = i;
    }

    size_t vertCount = vertElem->count;
    size_t propCount = vertElem->props.size();
    bool hasNormals = (idxNX >= 0 && idxNY >= 0 && idxNZ >= 0);
    bool hasUVs     = (idxU >= 0 && idxV >= 0);

    data.positions.resize(vertCount * 3);
    if (hasNormals) data.normals.resize(vertCount * 3);
    if (hasUVs)     data.uvs.resize(vertCount * 2);

    // ---- 解析属性尺寸（字节）----
    auto propBytes = [](std::string_view t) -> size_t {
        if (t == "float" || t == "int" || t == "uint") return 4;
        if (t == "double")                              return 8;
        if (t == "uchar" || t == "char")                return 1;
        if (t == "short" || t == "ushort")              return 2;
        return 4;
    };

    // ---- 读取顶点数据 ----
    if (fmt == Format::Ascii) {
        for (size_t vi = 0; vi < vertCount; ++vi) {
            std::getline(f, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ss2(line);
            std::vector<float> vals(propCount);
            for (size_t pi = 0; pi < propCount; ++pi) ss2 >> vals[pi];

            data.positions[vi*3+0] = (idxX >= 0) ? vals[idxX] : 0.f;
            data.positions[vi*3+1] = (idxY >= 0) ? vals[idxY] : 0.f;
            data.positions[vi*3+2] = (idxZ >= 0) ? vals[idxZ] : 0.f;
            if (hasNormals) {
                data.normals[vi*3+0] = vals[idxNX];
                data.normals[vi*3+1] = vals[idxNY];
                data.normals[vi*3+2] = vals[idxNZ];
            }
            if (hasUVs) {
                data.uvs[vi*2+0] = vals[idxU];
                data.uvs[vi*2+1] = vals[idxV];
            }
        }
    } else {
        // Binary little endian（本项目场景文件常见格式）
        // 计算每顶点字节数
        std::vector<size_t> propSizes(propCount);
        size_t vertStride = 0;
        for (size_t pi = 0; pi < propCount; ++pi) {
            propSizes[pi] = propBytes(vertElem->props[pi].type);
            vertStride += propSizes[pi];
        }

        std::vector<char> vertBuf(vertCount * vertStride);
        f.read(vertBuf.data(), static_cast<std::streamsize>(vertBuf.size()));

        for (size_t vi = 0; vi < vertCount; ++vi) {
            const char* row = vertBuf.data() + vi * vertStride;
            size_t offset = 0;
            std::vector<float> vals(propCount);
            for (size_t pi = 0; pi < propCount; ++pi) {
                size_t sz = propSizes[pi];
                if (sz == 4) {
                    std::memcpy(&vals[pi], row + offset, 4);
                } else if (sz == 8) {
                    double d;
                    std::memcpy(&d, row + offset, 8);
                    vals[pi] = static_cast<float>(d);
                } else {
                    vals[pi] = 0.f;
                }
                offset += sz;
            }

            data.positions[vi*3+0] = (idxX >= 0) ? vals[idxX] : 0.f;
            data.positions[vi*3+1] = (idxY >= 0) ? vals[idxY] : 0.f;
            data.positions[vi*3+2] = (idxZ >= 0) ? vals[idxZ] : 0.f;
            if (hasNormals) {
                data.normals[vi*3+0] = vals[idxNX];
                data.normals[vi*3+1] = vals[idxNY];
                data.normals[vi*3+2] = vals[idxNZ];
            }
            if (hasUVs) {
                data.uvs[vi*2+0] = vals[idxU];
                data.uvs[vi*2+1] = vals[idxV];
            }
        }
    }

    // ---- 读取面数据 ----
    if (faceElem) {
        size_t faceCount = faceElem->count;
        data.indices.reserve(faceCount * 3);

        if (fmt == Format::Ascii) {
            for (size_t fi = 0; fi < faceCount; ++fi) {
                std::getline(f, line);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::istringstream ss2(line);
                int cnt; ss2 >> cnt;
                std::vector<uint32_t> poly(cnt);
                for (int k = 0; k < cnt; ++k) ss2 >> poly[k];
                // 扇形三角化
                for (int k = 1; k + 1 < cnt; ++k) {
                    data.indices.push_back(poly[0]);
                    data.indices.push_back(poly[k]);
                    data.indices.push_back(poly[k+1]);
                }
            }
        } else {
            size_t countSz = propBytes(faceElem->listCountType);
            size_t elemSz  = propBytes(faceElem->listElemType);
            for (size_t fi = 0; fi < faceCount; ++fi) {
                uint32_t cnt = 0;
                char cntBuf[8];
                f.read(cntBuf, static_cast<std::streamsize>(countSz));
                if (countSz == 1) cnt = static_cast<uint8_t>(cntBuf[0]);
                else if (countSz == 2) { uint16_t v; std::memcpy(&v, cntBuf, 2); cnt = v; }
                else { std::memcpy(&cnt, cntBuf, 4); }

                std::vector<uint32_t> poly(cnt);
                for (uint32_t k = 0; k < cnt; ++k) {
                    char eBuf[8];
                    f.read(eBuf, static_cast<std::streamsize>(elemSz));
                    if (elemSz == 4) std::memcpy(&poly[k], eBuf, 4);
                    else if (elemSz == 2) { uint16_t v; std::memcpy(&v, eBuf, 2); poly[k] = v; }
                    else poly[k] = static_cast<uint8_t>(eBuf[0]);
                }
                // 扇形三角化
                for (uint32_t k = 1; k + 1 < cnt; ++k) {
                    data.indices.push_back(poly[0]);
                    data.indices.push_back(poly[k]);
                    data.indices.push_back(poly[k+1]);
                }
            }
        }
    }

    data.ok = true;
    return data;
}

// ============================================================================
// 主解析器状态机
// ============================================================================

class ParserImpl {
public:
    ParserImpl(std::string& src, const std::string& baseDir, bool verbose)
        : m_lex(src), m_baseDir(baseDir), m_verbose(verbose) {}

    std::optional<PbrtSceneData> parse() {
        m_scene.baseDir = m_baseDir;

        Token tok;
        while (m_lex.next(tok)) {
            if (tok.sv == "Integrator")       parseIntegrator();
            else if (tok.sv == "Transform")   parseTransform(m_scene.camera.cameraToWorld);
            else if (tok.sv == "Sampler")     parseSampler();
            else if (tok.sv == "PixelFilter") skipParams();
            else if (tok.sv == "Film")        parseFilm();
            else if (tok.sv == "Camera")      parseCamera();
            else if (tok.sv == "WorldBegin")  { /* 进入世界块 */ }
            else if (tok.sv == "WorldEnd")    break;
            else if (tok.sv == "Texture")     parseTexture();
            else if (tok.sv == "MakeNamedMaterial") parseMakeNamedMaterial();
            else if (tok.sv == "NamedMaterial") {
                Token nameTok;
                if (m_lex.next(nameTok)) m_currentMaterial = std::string(nameTok.sv);
            }
            else if (tok.sv == "Material")    parseInlineMaterial();
            else if (tok.sv == "Shape")       parseShape();
            else if (tok.sv == "AttributeBegin") parseAttributeBlock();
            else if (tok.sv == "AttributeEnd")   { /* 嵌套情况已在 parseAttributeBlock 处理 */ }
            else if (tok.sv == "AreaLightSource") parseAreaLightSource();
            else if (tok.sv == "LightSource") skipParams();
            else if (tok.sv == "Include") {
                Token incTok;
                if (m_lex.next(incTok)) {
                    if (m_verbose)
                        std::cout << "[PbrtParser] Include 暂不支持: " << incTok.sv << "\n";
                }
            }
            else if (tok.sv == "Scale" || tok.sv == "Rotate" ||
                     tok.sv == "Translate" || tok.sv == "ConcatTransform") {
                // 变换指令：读取参数但暂不处理（场景已有预设相机变换）
                skipParams();
            }
        }

        return std::move(m_scene);
    }

    // 当前面积光状态
    bool m_hasAreaLight = false;
    Vec3f m_areaLightL = {0,0,0};
    std::string m_currentMaterial;

private:
    Lexer m_lex;
    std::string m_baseDir;
    bool m_verbose;
    PbrtSceneData m_scene;

    // ---- 各指令解析 ----

    void parseIntegrator() {
        Token tok;
        if (m_lex.next(tok)) m_scene.integrator.type = std::string(tok.sv);
        auto params = readParams(m_lex);
        for (auto& p : params) {
            if (p.paramName == "maxdepth" && !p.floatVals.empty())
                m_scene.integrator.maxDepth = static_cast<uint32_t>(p.floatVals[0]);
        }
    }

    void parseTransform(Mat4f& mat) {
        // Transform [ m00 m01 ... m15 ]
        auto vals = readFloatList(m_lex);
        if (vals.size() == 16) {
            for (int i = 0; i < 16; ++i) mat[i] = vals[i];
        }
    }

    void parseSampler() {
        Token tok;
        if (m_lex.next(tok)) m_scene.sampler.type = std::string(tok.sv);
        auto params = readParams(m_lex);
        for (auto& p : params) {
            if (p.paramName == "pixelsamples" && !p.floatVals.empty())
                m_scene.sampler.pixelSamples = static_cast<uint32_t>(p.floatVals[0]);
        }
    }

    void parseFilm() {
        Token tok;
        if (m_lex.next(tok)) {} // "rgb" 等类型名，忽略
        auto params = readParams(m_lex);
        for (auto& p : params) {
            if (p.paramName == "filename")    m_scene.film.filename = p.strVal;
            else if (p.paramName == "xresolution" && !p.floatVals.empty())
                m_scene.film.xResolution = static_cast<uint32_t>(p.floatVals[0]);
            else if (p.paramName == "yresolution" && !p.floatVals.empty())
                m_scene.film.yResolution = static_cast<uint32_t>(p.floatVals[0]);
        }
    }

    void parseCamera() {
        Token tok;
        if (m_lex.next(tok)) m_scene.camera.type = std::string(tok.sv);
        auto params = readParams(m_lex);
        for (auto& p : params) {
            if (p.paramName == "fov" && !p.floatVals.empty())
                m_scene.camera.fov = p.floatVals[0];
        }
    }

    void parseTexture() {
        Token nameTok, typeTok, classTok;
        if (!m_lex.next(nameTok)) return;
        if (!m_lex.next(typeTok)) return;
        if (!m_lex.next(classTok)) return;

        PbrtTexture tex;
        tex.name = std::string(nameTok.sv);
        tex.type = (typeTok.sv == "float") ? PbrtTextureType::Float : PbrtTextureType::Spectrum;

        auto params = readParams(m_lex);
        for (auto& p : params) {
            if (p.paramName == "filename") tex.filename = p.strVal;
            if (p.paramName == "filter")   tex.filterMode = p.strVal;
        }

        if (m_verbose)
            std::cout << "[PbrtParser] Texture: " << tex.name << " -> " << tex.filename << "\n";

        m_scene.textures.emplace(tex.name, std::move(tex));
    }

    void fillMaterialParams(PbrtMaterial& mat, const std::vector<ParamBlock>& params) {
        for (const auto& p : params) {
            if (p.paramName == "type" && !p.strVal.empty()) {
                mat.type = parseMaterialType(p.strVal);
            } else if (p.paramName == "reflectance") {
                if (!p.strVal.empty()) {
                    mat.reflectanceTexture = p.strVal; // texture reference
                } else if (p.floatVals.size() >= 3) {
                    mat.reflectance = Vec3f{p.floatVals[0], p.floatVals[1], p.floatVals[2]};
                }
            } else if (p.paramName == "transmittance" && p.floatVals.size() >= 3) {
                mat.transmittance = Vec3f{p.floatVals[0], p.floatVals[1], p.floatVals[2]};
            } else if (p.paramName == "eta") {
                if (!p.strVal.empty()) {
                    mat.spectrumEta = p.strVal;
                } else if (!p.floatVals.empty()) {
                    mat.dielectricEta = p.floatVals[0];
                } else if (p.floatVals.size() >= 3) {
                    mat.conductorEta = Vec3f{p.floatVals[0], p.floatVals[1], p.floatVals[2]};
                }
            } else if (p.paramName == "k") {
                if (!p.strVal.empty()) {
                    mat.spectrumK = p.strVal;
                } else if (p.floatVals.size() >= 3) {
                    mat.conductorK = Vec3f{p.floatVals[0], p.floatVals[1], p.floatVals[2]};
                }
            } else if (p.paramName == "roughness" && !p.floatVals.empty()) {
                mat.roughness = p.floatVals[0];
            } else if (p.paramName == "uroughness" && !p.floatVals.empty()) {
                mat.uRoughness = p.floatVals[0];
            } else if (p.paramName == "vroughness" && !p.floatVals.empty()) {
                mat.vRoughness = p.floatVals[0];
            } else if (p.paramName == "remaproughness") {
                mat.remapRoughness = p.boolVal;
            } else if (p.paramName == "displacement" && !p.strVal.empty()) {
                mat.displacementTexture = p.strVal;
            }
        }
    }

    void parseMakeNamedMaterial() {
        Token nameTok;
        if (!m_lex.next(nameTok)) return;
        PbrtMaterial mat;
        mat.name = std::string(nameTok.sv);
        auto params = readParams(m_lex);
        fillMaterialParams(mat, params);
        if (m_verbose)
            std::cout << "[PbrtParser] MakeNamedMaterial: " << mat.name << "\n";
        m_scene.namedMaterials.emplace(mat.name, std::move(mat));
    }

    void parseInlineMaterial() {
        // Material "diffuse" "rgb reflectance" [...]
        Token typeTok;
        if (!m_lex.next(typeTok)) return;
        // 创建匿名内联材质
        PbrtMaterial mat;
        mat.name = "__inline__";
        mat.type = parseMaterialType(typeTok.sv);
        auto params = readParams(m_lex);
        fillMaterialParams(mat, params);
        m_currentMaterial = mat.name;
        m_scene.namedMaterials[mat.name] = std::move(mat);
    }

    void parseShape() {
        Token typeTok;
        if (!m_lex.next(typeTok)) return;

        PbrtShape shape;
        shape.materialName = m_currentMaterial;
        shape.isAreaLight  = m_hasAreaLight;
        shape.areaLightL   = m_areaLightL;

        auto params = readParams(m_lex);

        if (typeTok.sv == "plymesh") {
            shape.type = PbrtShapeType::PlyMesh;
            PbrtPlyMesh pm;
            for (auto& p : params) {
                if (p.paramName == "filename") pm.filename = p.strVal;
            }
            if (m_verbose)
                std::cout << "[PbrtParser] Shape plymesh: " << pm.filename << "\n";
            shape.geometry = std::move(pm);
        } else if (typeTok.sv == "trianglemesh") {
            shape.type = PbrtShapeType::TriangleMesh;
            PbrtTriangleMesh tm;
            for (auto& p : params) {
                if (p.paramName == "P" || p.paramName == "p") {
                    tm.positions = p.floatVals;
                } else if (p.paramName == "N" || p.paramName == "n") {
                    tm.normals = p.floatVals;
                } else if (p.paramName == "uv" || p.paramName == "st") {
                    tm.uvs = p.floatVals;
                } else if (p.paramName == "indices") {
                    tm.indices.reserve(p.floatVals.size());
                    for (float v : p.floatVals)
                        tm.indices.push_back(static_cast<uint32_t>(v));
                }
            }
            shape.geometry = std::move(tm);
        } else {
            // 未知形状，跳过
            return;
        }

        m_scene.shapes.push_back(std::move(shape));
    }

    void parseAreaLightSource() {
        Token typeTok;
        if (!m_lex.next(typeTok)) return;
        auto params = readParams(m_lex);
        m_hasAreaLight = true;
        m_areaLightL = {1.f, 1.f, 1.f};
        for (auto& p : params) {
            if (p.paramName == "L" && p.floatVals.size() >= 3) {
                m_areaLightL = {p.floatVals[0], p.floatVals[1], p.floatVals[2]};
            }
        }
    }

    // AttributeBegin ... AttributeEnd 块（递归处理嵌套）
    void parseAttributeBlock() {
        // 保存状态
        bool savedHasLight   = m_hasAreaLight;
        Vec3f savedLightL    = m_areaLightL;
        std::string savedMat = m_currentMaterial;
        m_hasAreaLight = false;

        Token tok;
        while (m_lex.next(tok)) {
            if (tok.sv == "AttributeEnd")   break;
            if (tok.sv == "AttributeBegin") parseAttributeBlock(); // 嵌套
            else if (tok.sv == "AreaLightSource") parseAreaLightSource();
            else if (tok.sv == "NamedMaterial") {
                Token nameTok;
                if (m_lex.next(nameTok)) m_currentMaterial = std::string(nameTok.sv);
            }
            else if (tok.sv == "Material")  parseInlineMaterial();
            else if (tok.sv == "Shape")     parseShape();
            else if (tok.sv == "LightSource") skipParams();
            else if (tok.sv == "Scale" || tok.sv == "Rotate" ||
                     tok.sv == "Translate" || tok.sv == "ConcatTransform" ||
                     tok.sv == "Transform")  skipParams();
        }

        // 恢复状态
        m_hasAreaLight   = savedHasLight;
        m_areaLightL     = savedLightL;
        m_currentMaterial = savedMat;
    }

    // 跳过当前指令的所有参数（遇到下一个关键字停止）
    void skipParams() {
        Token tok;
        while (m_lex.peek(tok)) {
            if (!tok.isString && isPbrtKeyword(tok.sv)) break;
            m_lex.next(tok);
        }
    }
};

} // anonymous namespace

// ============================================================================
// PbrtParser::parseFile 实现
// ============================================================================

std::optional<PbrtSceneData> PbrtParser::parseFile(
    const std::string& filepath,
    const ParseOptions& options
) {
    // ---- 读取整个文件到 string（避免逐行 getline 开销）----
    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "[PbrtParser] 无法打开文件: " << filepath << "\n";
        return std::nullopt;
    }
    auto sz = f.tellg();
    f.seekg(0);
    std::string src(static_cast<size_t>(sz), '\0');
    f.read(src.data(), sz);
    f.close();

    std::string baseDir = fs::path(filepath).parent_path().string();
    if (baseDir.empty()) baseDir = ".";

    std::cout << "[PbrtParser] 开始解析: " << filepath
              << " (" << src.size() << " 字节)\n";

    // ---- Phase 2: 语法解析（单线程）----
    ParserImpl impl(src, baseDir, options.verbose);
    auto sceneOpt = impl.parse();
    if (!sceneOpt) return std::nullopt;

    PbrtSceneData scene = std::move(*sceneOpt);

    std::cout << "[PbrtParser] 解析完成: "
              << scene.textures.size()      << " 纹理, "
              << scene.namedMaterials.size() << " 材质, "
              << scene.shapes.size()         << " 形状\n";

    if (!options.loadPlyFiles) return scene;

    // ---- Phase 3: 并行加载 PLY 文件 ----
    // 收集所有 plymesh shape 的下标
    std::vector<size_t> plyIndices;
    plyIndices.reserve(scene.shapes.size());
    for (size_t i = 0; i < scene.shapes.size(); ++i) {
        if (scene.shapes[i].type == PbrtShapeType::PlyMesh) {
            plyIndices.push_back(i);
        }
    }

    if (plyIndices.empty()) return scene;

    std::cout << "[PbrtParser] 并行加载 " << plyIndices.size() << " 个 PLY 文件...\n";

    uint32_t numWorkers = options.numThreads == 0
        ? std::thread::hardware_concurrency()
        : options.numThreads;
    tf::Executor executor(numWorkers);
    tf::Taskflow taskflow;

    // 每个 ply shape 独立任务，写不同槽位 → 无锁
    for (size_t idx : plyIndices) {
        taskflow.emplace([&scene, &baseDir, idx]() {
            auto& shape = scene.shapes[idx];
            const auto& pm = std::get<PbrtPlyMesh>(shape.geometry);

            // 构造绝对路径
            fs::path plyPath = fs::path(baseDir) / pm.filename;
            PlyData plyData = loadPly(plyPath.string());

            if (!plyData.ok) {
                std::cerr << "[PbrtParser] PLY 加载失败: " << plyPath.string() << "\n";
                return;
            }

            // 将 PlyMesh 就地替换为 TriangleMesh（移动语义，零拷贝）
            PbrtTriangleMesh tm;
            tm.positions = std::move(plyData.positions);
            tm.normals   = std::move(plyData.normals);
            tm.uvs       = std::move(plyData.uvs);
            tm.indices   = std::move(plyData.indices);
            shape.type     = PbrtShapeType::TriangleMesh;
            shape.geometry = std::move(tm);
        }).name("ply_" + std::to_string(idx));
    }

    // 导出任务图（调试用）
    if (options.verbose) {
        std::ofstream dotFile("pbrt_ply_taskgraph.dot");
        taskflow.dump(dotFile);
        std::cout << "[PbrtParser] PLY 任务图已导出: pbrt_ply_taskgraph.dot\n";
    }

    executor.run(taskflow).wait();

    // 统计成功加载的 PLY 数量
    size_t loaded = 0;
    for (size_t idx : plyIndices) {
        if (scene.shapes[idx].type == PbrtShapeType::TriangleMesh) ++loaded;
    }
    std::cout << "[PbrtParser] PLY 加载完成: " << loaded << "/" << plyIndices.size()
              << " 成功 (线程数: " << numWorkers << ")\n";

    return scene;
}

} // namespace viewer
