// ============================================================================
// VLR 测试工具 - 几何生成函数
// ============================================================================

#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>

namespace vlr_test {

static const float PI = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;

/// UV sphere: center (cx,cy,cz), radius, segments (longitude), rings (latitude)
static void createSphere(std::vector<float>& vertices, std::vector<uint32_t>& indices,
                        float cx, float cy, float cz, float radius,
                        int segments = 64, int rings = 48,
                        std::vector<float>* outNormals = nullptr) {
    vertices.clear();
    indices.clear();
    if (outNormals) outNormals->clear();

    for (int lat = 0; lat <= rings; ++lat) {
        float phi = PI * float(lat) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int lon = 0; lon <= segments; ++lon) {
            float theta = TWO_PI * float(lon) / float(segments);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            float nx = sinPhi * cosTheta;
            float ny = cosPhi;
            float nz = sinPhi * sinTheta;

            vertices.push_back(cx + radius * nx);
            vertices.push_back(cy + radius * ny);
            vertices.push_back(cz + radius * nz);

            if (outNormals) {
                outNormals->push_back(nx);
                outNormals->push_back(ny);
                outNormals->push_back(nz);
            }
        }
    }

    for (int lat = 0; lat < rings; ++lat) {
        for (int lon = 0; lon < segments; ++lon) {
            uint32_t first = lat * (segments + 1) + lon;
            uint32_t second = first + segments + 1;

            if (lat != 0) {
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second);
            }
            if (lat != rings - 1) {
                indices.push_back(first + 1);
                indices.push_back(second + 1);
                indices.push_back(second);
            }
        }
    }
}

/// Axis-aligned box at origin, rotated around Y axis, then translated to (cx,cy,cz)
static void createRotatedBox(std::vector<float>& vertices, std::vector<uint32_t>& indices,
                             float cx, float cy, float cz, float size, float rotationY) {
    float halfSize = size * 0.5f;
    float cosY = std::cos(rotationY);
    float sinY = std::sin(rotationY);

    float verts[] = {
        -halfSize, -halfSize, halfSize,   halfSize, -halfSize, halfSize,
        halfSize, halfSize, halfSize,     -halfSize, halfSize, halfSize,
        halfSize, -halfSize, -halfSize,   -halfSize, -halfSize, -halfSize,
        -halfSize, halfSize, -halfSize,    halfSize, halfSize, -halfSize,
        -halfSize, -halfSize, -halfSize,   -halfSize, -halfSize, halfSize,
        -halfSize, halfSize, halfSize,    -halfSize, halfSize, -halfSize,
        halfSize, -halfSize, halfSize,     halfSize, -halfSize, -halfSize,
        halfSize, halfSize, -halfSize,    halfSize, halfSize, halfSize,
        -halfSize, halfSize, halfSize,    halfSize, halfSize, halfSize,
        halfSize, halfSize, -halfSize,    -halfSize, halfSize, -halfSize,
        -halfSize, -halfSize, -halfSize,   halfSize, -halfSize, -halfSize,
        halfSize, -halfSize, halfSize,    -halfSize, -halfSize, halfSize
    };

    uint32_t faceInds[] = {
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23
    };

    vertices.clear();
    indices.clear();

    for (int i = 0; i < 24; ++i) {
        float x = verts[i * 3 + 0];
        float y = verts[i * 3 + 1];
        float z = verts[i * 3 + 2];
        float newX = cosY * x - sinY * z + cx;
        float newZ = sinY * x + cosY * z + cz;
        vertices.push_back(newX);
        vertices.push_back(y + cy);
        vertices.push_back(newZ);
    }

    for (int i = 0; i < 36; ++i)
        indices.push_back(faceInds[i]);
}

/// Load OBJ file (simple: vertices + triangle faces only)
static bool loadOBJ(const char* filename, std::vector<float>& vertices, std::vector<uint32_t>& indices,
                   float scale = 1.0f, float tx = 0.0f, float ty = 0.0f, float tz = 0.0f) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "[Error] Cannot open OBJ file: %s\n", filename);
        return false;
    }
    
    vertices.clear();
    indices.clear();
    
    std::vector<float> tempVerts;
    std::string line;
    
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;
        
        if (prefix == "v") {
            float x, y, z;
            iss >> x >> y >> z;
            tempVerts.push_back(x * scale + tx);
            tempVerts.push_back(y * scale + ty);
            tempVerts.push_back(z * scale + tz);
        }
        else if (prefix == "f") {
            std::string v1, v2, v3;
            iss >> v1 >> v2 >> v3;
            
            auto parseIndex = [](const std::string& s) -> uint32_t {
                size_t slash = s.find('/');
                std::string indexStr = (slash != std::string::npos) ? s.substr(0, slash) : s;
                return (uint32_t)(std::stoi(indexStr) - 1);
            };
            
            indices.push_back(parseIndex(v1));
            indices.push_back(parseIndex(v2));
            indices.push_back(parseIndex(v3));
        }
    }
    
    vertices = tempVerts;
    file.close();
    
    printf("[Info] Loaded OBJ: %s (%zu vertices, %zu triangles)\n", 
           filename, vertices.size() / 3, indices.size() / 3);
    return true;
}

} // namespace vlr_test
