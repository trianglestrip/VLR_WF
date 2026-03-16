#ifdef VLR_ENABLE_CPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

#ifdef VLR_ENABLE_CPU_DEBUG
    #define VLR_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
    #define VLR_DEBUG_PRINTF(...) ((void)0)
#endif

#include "context.h"
#include "image_loader.h"
#include "utils/cuda_util.h"
#include <cstring>
#include <stdexcept>

namespace vlr {

namespace {

shared::TextureFormat apiFormatToInternal(uint32_t format) {
    switch (format) {
        case 0: return shared::TextureFormat_RGBA8;
        case 1: return shared::TextureFormat_RGB32F;
        case 2: return shared::TextureFormat_RGBA32F;
        default: return shared::TextureFormat_RGBA8;
    }
}

size_t bytesPerPixel(shared::TextureFormat fmt) {
    switch (fmt) {
        case shared::TextureFormat_RGBA8: return 4;
        case shared::TextureFormat_RGB32F: return 3 * sizeof(float);
        case shared::TextureFormat_RGBA32F: return 4 * sizeof(float);
        default: return 4;
    }
}

}  // namespace

bool Context::createTexture2D(const char* imagePath, uint32_t* outTextureIndex) {
    if (!imagePath || !outTextureIndex) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2D: invalid arguments (null)\n");
        return false;
    }
    TextureImage img;
    std::string error;
    if (!loadTextureImage(imagePath, img, &error)) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2D: failed to load '%s': %s\n", imagePath, error.c_str());
        return false;
    }
    shared::TextureFormat fmt = (img.format == TextureImageFormat::RGBA8)
        ? shared::TextureFormat_RGBA8
        : shared::TextureFormat_RGBA32F;
    bool ok = createTexture2DFromMemory(img.data, img.width, img.height,
                                        static_cast<uint32_t>(img.format), outTextureIndex);
    freeTextureImage(img);
    if (ok) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2D: loaded '%s' %ux%u -> texture index %u\n",
               imagePath, img.width, img.height, *outTextureIndex);
    }
    return ok;
}

bool Context::createTexture2DFromMemory(const void* data, uint32_t width, uint32_t height,
                                       uint32_t format, uint32_t* outTextureIndex) {
    if (!data || width == 0 || height == 0 || !outTextureIndex) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: invalid arguments\n");
        return false;
    }
    if (format > 2) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: invalid format %u (0=RGBA8, 1=RGB32F, 2=RGBA32F)\n", format);
        return false;
    }
    try {
        shared::TextureFormat fmt = apiFormatToInternal(format);
        size_t bytesPerPixelVal = bytesPerPixel(fmt);
        size_t totalBytes = static_cast<size_t>(width) * height * bytesPerPixelVal;

        TextureRecord rec;
        rec.gpuBuffer = std::make_unique<cudau::Buffer<uint8_t>>();
        rec.gpuBuffer->initialize(m_cudaContext, cudau::BufferType::Device, totalBytes);
        rec.gpuBuffer->copyToDevice(static_cast<const uint8_t*>(data), totalBytes, m_stream);

        rec.descriptor = shared::Texture2DDescriptor(
            rec.gpuBuffer->getDevicePointer(),
            width, height,
            fmt,
            1, 0);
        rec.filterMode = shared::TextureFilter_Linear;
        rec.wrapU = shared::TextureWrap_Repeat;
        rec.wrapV = shared::TextureWrap_Repeat;

        uint32_t idx = static_cast<uint32_t>(m_textures.size());
        m_textures.push_back(std::move(rec));
        m_textureDescriptorBufferDirty = true;
        *outTextureIndex = idx;
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: created texture %u %ux%u format %u\n",
               idx, width, height, format);
        return true;
    } catch (const std::exception& e) {
        VLR_DEBUG_PRINTF("[VLR] createTexture2DFromMemory: exception: %s\n", e.what());
        return false;
    }
}

void Context::destroyTexture(uint32_t textureIndex) {
    if (textureIndex >= m_textures.size()) {
        VLR_DEBUG_PRINTF("[VLR] destroyTexture: invalid index %u (max %zu)\n",
               textureIndex, m_textures.size());
        return;
    }
    m_textures[textureIndex].gpuBuffer.reset();
    m_textures[textureIndex].descriptor = shared::Texture2DDescriptor();
    m_textureDescriptorBufferDirty = true;
    VLR_DEBUG_PRINTF("[VLR] destroyTexture: texture %u destroyed\n", textureIndex);
}

bool Context::setTextureFilterMode(uint32_t textureIndex, uint32_t filterMode) {
    if (textureIndex >= m_textures.size()) {
        VLR_DEBUG_PRINTF("[VLR] setTextureFilterMode: invalid index %u\n", textureIndex);
        return false;
    }
    if (filterMode > 1) {
        VLR_DEBUG_PRINTF("[VLR] setTextureFilterMode: invalid mode %u (0=Nearest, 1=Linear)\n", filterMode);
        return false;
    }
    m_textures[textureIndex].filterMode = static_cast<shared::TextureFilterMode>(filterMode);
    return true;
}

bool Context::setTextureWrapMode(uint32_t textureIndex, uint32_t wrapU, uint32_t wrapV) {
    if (textureIndex >= m_textures.size()) {
        VLR_DEBUG_PRINTF("[VLR] setTextureWrapMode: invalid index %u\n", textureIndex);
        return false;
    }
    if (wrapU > 1 || wrapV > 1) {
        VLR_DEBUG_PRINTF("[VLR] setTextureWrapMode: invalid wrap mode (0=Repeat, 1=Clamp)\n");
        return false;
    }
    m_textures[textureIndex].wrapU = static_cast<shared::TextureWrapMode>(wrapU);
    m_textures[textureIndex].wrapV = static_cast<shared::TextureWrapMode>(wrapV);
    return true;
}

const shared::Texture2DDescriptor* Context::getTextureDescriptor(uint32_t textureIndex) const {
    if (textureIndex >= m_textures.size()) return nullptr;
    const auto& rec = m_textures[textureIndex];
    if (!rec.gpuBuffer || !rec.gpuBuffer->getDevicePointer()) return nullptr;
    return &rec.descriptor;
}

void Context::updateTextureDescriptorBuffer() const {
    if (!m_textureDescriptorBufferDirty || m_textures.empty()) return;
    m_textureDescriptorBufferDirty = false;
    std::vector<shared::Texture2DDescriptor> hostDescriptors(m_textures.size());
    for (size_t i = 0; i < m_textures.size(); ++i) {
        const auto& rec = m_textures[i];
        if (rec.gpuBuffer && rec.gpuBuffer->getDevicePointer())
            hostDescriptors[i] = rec.descriptor;
        else
            hostDescriptors[i] = shared::Texture2DDescriptor();
    }
    if (!m_textureDescriptorBuffer)
        m_textureDescriptorBuffer = std::make_unique<cudau::Buffer<shared::Texture2DDescriptor>>();
    m_textureDescriptorBuffer->initialize(m_cudaContext, cudau::BufferType::Device, hostDescriptors.size());
    m_textureDescriptorBuffer->copyToDevice(hostDescriptors.data(), hostDescriptors.size(), m_stream);
}

const shared::Texture2DDescriptor* Context::getTextureDescriptorBuffer() const {
    updateTextureDescriptorBuffer();
    return (m_textureDescriptorBuffer && m_textureDescriptorBuffer->getDevicePointer())
        ? m_textureDescriptorBuffer->getDevicePointer() : nullptr;
}

} // namespace vlr
