// 最小化测试程序 - 用于定位崩溃问题
#include <cstdio>
#include <cstdlib>

#define VLR_EXPORTS
#include "../libVLR/include/vlr/vlr.h"

int main(int argc, char** argv) {
    printf("=== VLR Minimal Test ===\n");
    fflush(stdout);

    printf("[Step 1] Creating Context...\n");
    fflush(stdout);

    VLRContext context = nullptr;
    VLRResult res = vlrCreateContext(nullptr, 1, &context);
    
    printf("[Step 1] vlrCreateContext returned: %d\n", res);
    fflush(stdout);

    if (res != VLRResult_Success) {
        fprintf(stderr, "[Error] Failed to create Context: %d\n", res);
        return 1;
    }

    printf("[Step 2] Context created successfully\n");
    fflush(stdout);

    printf("[Step 3] Destroying Context...\n");
    fflush(stdout);

    vlrDestroyContext(context);

    printf("[Done] Test passed\n");
    return 0;
}
