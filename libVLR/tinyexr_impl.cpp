// tinyexr 实现 - 使用 STB zlib（避免 miniz 编译问题）
// 定义 STB_IMAGE 实现（提供完整的 stb_image，包括 zlib）
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

// 定义 STB_IMAGE_WRITE 实现（提供 zlib 编码）
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ZLIB_COMPRESS stbi_zlib_compress
#include "stb/stb_image_write.h"

// 定义 tinyexr 实现（使用 STB zlib）
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_IMPLEMENTATION

#pragma warning(push)
#pragma warning(disable: 4267 4244 4996 4717) // 忽略递归和转换警告
#include "tinyexr/tinyexr.h"
#pragma warning(pop)
