// ============================================================================
// VLR Interactive Progressive Viewer
//
// GLFW + OpenGL + Dear ImGui + CUDA-GL Interop 交互式渐进渲染。
// 每渲染一个 sample 就更新窗口，ImGui 面板提供参数实时调节。
//
// 依赖：GLFW, GLAD, Dear ImGui, CUDA, VLR
// ============================================================================

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <vlr/vlr.h>
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>

// ============================================================================
// 全局状态
// ============================================================================

static struct AppState {
    VLRContext context = nullptr;
    VLRScene scene = nullptr;

    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t accumFrames = 0;
    int maxSamples = 256;
    float exposure = 1.5f;
    float gamma = 2.2f;
    bool paused = false;
    bool needReset = false;
    bool showGui = false;

    // OpenGL
    GLuint texture = 0;
    GLuint pbo = 0;
    cudaGraphicsResource_t cudaPboResource = nullptr;

    // 帧率统计
    float fps = 0.0f;
    float sampleMs = 0.0f;
    int frameCount = 0;
    std::chrono::steady_clock::time_point lastFpsTime;

    // 样本率历史（用于绘图）
    float sampleMsHistory[120] = {};
    int sampleMsHistoryIdx = 0;
} g;

// ============================================================================
// 场景构建（Cornell Box）
// ============================================================================

static bool buildCornellBoxScene() {
    VLRResult res;

    float white[]  = { 0.5225f, 0.5225f, 0.5225f };
    float red[]    = { 0.5225f, 0.0509f, 0.0509f };
    float blue[]   = { 0.0509f, 0.0509f, 0.5225f };
    float gold_eta[]   = { 0.12481f, 0.46823f, 1.44476f };
    float gold_kappa[] = { 3.32107f, 2.23761f, 1.69196f };
    float lightEmission[] = { 30.0f, 30.0f, 30.0f };

    VLRMaterial whiteMat = nullptr, redMat = nullptr, blueMat = nullptr;
    VLRMaterial goldMat = nullptr, lightMat = nullptr, diamondMat = nullptr;
    VLRMaterial checkerMat = nullptr;

    res = vlrCreateMaterial(g.scene, 0, white, nullptr, &whiteMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(g.scene, 0, red, nullptr, &redMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(g.scene, 0, blue, nullptr, &blueMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterialConductor(g.scene, gold_eta, gold_kappa, 0.01f, &goldMat);
    if (res != VLRResult_Success) return false;
    res = vlrCreateMaterial(g.scene, 0, white, lightEmission, &lightMat);
    if (res != VLRResult_Success) return false;
    float diamondColor[] = { 0.999f, 0.999f, 0.999f };
    res = vlrCreateMaterialEx(g.scene, 6, diamondColor, 0.0f, 0.0f, 2.42f, nullptr, &diamondMat);
    if (res != VLRResult_Success) return false;
    float checkerDark[] = { 0.05f, 0.05f, 0.05f };
    float checkerLight[] = { 0.50f, 0.50f, 0.50f };
    res = vlrCreateMaterialCheckerboard(g.scene, checkerDark, checkerLight, 64, 3.0f, &checkerMat);
    if (res != VLRResult_Success) return false;

    float pos0[] = {0,0,0}, sc1[] = {1,1,1}, ay[] = {0,1,0};
    VLRInstance inst = nullptr;

    // 地板（棋盘格）
    float floorV[] = { -1.5f,0,-1.5f, 1.5f,0,-1.5f, 1.5f,0,1.5f, -1.5f,0,1.5f };
    uint32_t floorI[] = { 0,1,2, 0,2,3 };
    VLRTriangleMesh m = nullptr;
    vlrCreateTriangleMesh(g.scene, floorV, 4, floorI, 2, checkerMat, &m);
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &inst);

    // 天花板
    float ceilV[] = { -1.5f,3,-1.5f, 1.5f,3,-1.5f, 1.5f,3,1.5f, -1.5f,3,1.5f };
    uint32_t ceilI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(g.scene, ceilV, 4, ceilI, 2, whiteMat, &m);
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &inst);

    // 后墙
    float backV[] = { -1.5f,0,-1.5f, 1.5f,0,-1.5f, 1.5f,3,-1.5f, -1.5f,3,-1.5f };
    uint32_t backI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(g.scene, backV, 4, backI, 2, whiteMat, &m);
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &inst);

    // 左墙（红）
    float leftV[] = { -1.5f,0,-1.5f, -1.5f,0,1.5f, -1.5f,3,1.5f, -1.5f,3,-1.5f };
    uint32_t leftI[] = { 0,1,2, 0,2,3 };
    vlrCreateTriangleMesh(g.scene, leftV, 4, leftI, 2, redMat, &m);
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &inst);

    // 右墙（蓝）
    float rightV[] = { 1.5f,0,-1.5f, 1.5f,0,1.5f, 1.5f,3,1.5f, 1.5f,3,-1.5f };
    uint32_t rightI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(g.scene, rightV, 4, rightI, 2, blueMat, &m);
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &inst);

    // 光源 (y=2.9 与参考一致)
    float lightV[] = { -0.5f,2.9f,-0.5f, 0.5f,2.9f,-0.5f, 0.5f,2.9f,0.5f, -0.5f,2.9f,0.5f };
    uint32_t lightI[] = { 0,2,1, 0,3,2 };
    vlrCreateTriangleMesh(g.scene, lightV, 4, lightI, 2, lightMat, &m);
    VLRInstance li = nullptr;
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &li);
    vlrAddAreaLight(g.scene, li);

    // 金属盒子（左侧，接近参考图位置）
    float bx=-0.7f, by=0, bz=-0.25f, bs=0.5f;
    float boxV[] = {
        bx-bs,by,bz-bs, bx+bs,by,bz-bs, bx+bs,by,bz+bs, bx-bs,by,bz+bs,
        bx-bs,by+1,bz-bs, bx+bs,by+1,bz-bs, bx+bs,by+1,bz+bs, bx-bs,by+1,bz+bs,
    };
    uint32_t boxI[] = { 0,1,2,0,2,3, 4,6,5,4,7,6, 0,4,5,0,5,1, 2,6,7,2,7,3, 0,3,7,0,7,4, 1,5,6,1,6,2 };
    vlrCreateTriangleMesh(g.scene, boxV, 8, boxI, 12, goldMat, &m);
    vlrCreateInstance(g.scene, m, pos0, sc1, ay, 0, &inst);

    // 钻石球（右侧，IOR=2.42）
    auto makeSphere = [&](float cx, float cy, float cz, float cr, VLRMaterial mat) {
        constexpr int SL = 64, ST = 64;
        std::vector<float> sv; std::vector<uint32_t> si;
        for (int j = 0; j <= ST; ++j) {
            float t = 3.14159265f * j / ST, sn = sinf(t), cs = cosf(t);
            for (int i = 0; i <= SL; ++i) {
                float p = 6.28318530f * i / SL;
                sv.push_back(cx+cr*sn*cosf(p)); sv.push_back(cy+cr*cs); sv.push_back(cz+cr*sn*sinf(p));
            }
        }
        for (int j = 0; j < ST; ++j)
            for (int i = 0; i < SL; ++i) {
                uint32_t a = j*(SL+1)+i, b = a+SL+1;
                si.push_back(a); si.push_back(a+1); si.push_back(b);
                si.push_back(a+1); si.push_back(b+1); si.push_back(b);
            }
        VLRTriangleMesh sm = nullptr;
        vlrCreateTriangleMesh(g.scene, sv.data(), (uint32_t)(sv.size()/3), si.data(), (uint32_t)(si.size()/3), mat, &sm);
        VLRInstance si2 = nullptr;
        vlrCreateInstance(g.scene, sm, pos0, sc1, ay, 0, &si2);
    };

    makeSphere(0.7f, 0.6f, 0.5f, 0.6f, diamondMat);

    // 相机
    VLRCameraParams cam = {};
    cam.position[0]=0; cam.position[1]=1.5f; cam.position[2]=6.0f;
    cam.direction[0]=0; cam.direction[1]=0; cam.direction[2]=-1;
    cam.up[0]=0; cam.up[1]=1; cam.up[2]=0;
    cam.fovY = 40.0f * 3.14159265f / 180.0f;
    cam.aspect = (float)g.width / g.height;
    vlrSetCamera(g.scene, &cam);
    return true;
}

// ============================================================================
// OpenGL 初始化（渲染纹理 + PBO + Shader）
// ============================================================================

static const char* vsSrc = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 uv;
void main() { gl_Position = vec4(aPos, 0, 1); uv = aUV; }
)";
static const char* fsSrc = R"(
#version 330 core
in vec2 uv;
out vec4 c;
uniform sampler2D tex;
void main() { c = texture(tex, uv); }
)";

static GLuint shaderProg = 0, quadVAO = 0, quadVBO = 0;

static GLuint compileShader(GLenum t, const char* s) {
    GLuint sh = glCreateShader(t);
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(sh, 512, nullptr, log); fprintf(stderr, "[GL] %s\n", log); }
    return sh;
}

static void initGL() {
    float q[] = { -1,-1,0,1, 1,-1,1,1, 1,1,1,0, -1,-1,0,1, 1,1,1,0, -1,1,0,0 };
    glGenVertexArrays(1, &quadVAO); glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO); glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8); glEnableVertexAttribArray(1);

    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    shaderProg = glCreateProgram();
    glAttachShader(shaderProg, vs); glAttachShader(shaderProg, fs);
    glLinkProgram(shaderProg); glDeleteShader(vs); glDeleteShader(fs);

    glGenTextures(1, &g.texture); glBindTexture(GL_TEXTURE_2D, g.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.width, g.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenBuffers(1, &g.pbo); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g.pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, g.width * g.height * 4, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    cudaGraphicsGLRegisterBuffer(&g.cudaPboResource, g.pbo, cudaGraphicsMapFlagsWriteDiscard);
}

// ============================================================================
// 渲染 + Tonemap + PBO 更新
// ============================================================================

static void renderOneSampleAndTonemap() {
    if (g.paused && !g.needReset) return;
    if (g.needReset) {
        vlrBeginProgressive(g.context, g.scene, g.width, g.height);
        g.accumFrames = 0;
        g.needReset = false;
    }
    if (g.accumFrames >= (uint32_t)g.maxSamples) return;

    auto t0 = std::chrono::steady_clock::now();
    vlrRenderOneSample(g.context, &g.accumFrames);
    auto t1 = std::chrono::steady_clock::now();
    g.sampleMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    g.sampleMsHistory[g.sampleMsHistoryIdx % 120] = g.sampleMs;
    g.sampleMsHistoryIdx++;

    size_t sz = 0; uint8_t* ptr = nullptr;
    cudaGraphicsMapResources(1, &g.cudaPboResource, 0);
    cudaGraphicsResourceGetMappedPointer((void**)&ptr, &sz, g.cudaPboResource);
    vlrTonemapToRGBA8(g.context, ptr, g.exposure, g.gamma);
    cudaGraphicsUnmapResources(1, &g.cudaPboResource, 0);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g.pbo);
    glBindTexture(GL_TEXTURE_2D, g.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g.width, g.height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

// ============================================================================
// ImGui GUI 面板
// ============================================================================

static void drawImGui() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (g.showGui) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);

        ImGui::Begin("VLR Control Panel", &g.showGui);

        // ---- 渲染状态 ----
        if (ImGui::CollapsingHeader("Render Status", ImGuiTreeNodeFlags_DefaultOpen)) {
            float progress = (float)g.accumFrames / (float)g.maxSamples;
            ImGui::ProgressBar(progress, ImVec2(-1, 0));
            ImGui::Text("Samples: %u / %d", g.accumFrames, g.maxSamples);
            ImGui::Text("Resolution: %u x %u", g.width, g.height);
            ImGui::Text("FPS: %.1f", g.fps);
            ImGui::Text("Sample time: %.2f ms", g.sampleMs);
            float mps = (g.sampleMs > 0) ? (g.width * g.height) / (g.sampleMs * 1000.0f) : 0;
            ImGui::Text("Throughput: %.2f Msamples/s", mps);

            if (g.accumFrames >= (uint32_t)g.maxSamples)
                ImGui::TextColored(ImVec4(0.2f, 1, 0.2f, 1), "RENDER COMPLETE");
            else if (g.paused)
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "PAUSED");
            else
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "RENDERING...");
        }

        ImGui::Separator();

        // ---- Tonemap 参数 ----
        if (ImGui::CollapsingHeader("Tonemap", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Exposure", &g.exposure, 0.1f, 10.0f, "%.2f");
            ImGui::SliderFloat("Gamma", &g.gamma, 1.0f, 3.0f, "%.2f");
            if (ImGui::Button("Reset Tonemap")) { g.exposure = 1.0f; g.gamma = 2.2f; }
        }

        ImGui::Separator();

        // ---- 渲染控制 ----
        if (ImGui::CollapsingHeader("Render Control", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderInt("Max Samples", &g.maxSamples, 1, 16384);

            if (ImGui::Button(g.paused ? "Resume (Space)" : "Pause (Space)")) {
                g.paused = !g.paused;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset (R)")) {
                g.needReset = true;
            }
        }

        ImGui::Separator();

        // ---- 性能图表 ----
        if (ImGui::CollapsingHeader("Performance")) {
            int count = std::min(g.sampleMsHistoryIdx, 120);
            if (count > 0) {
                int offset = (g.sampleMsHistoryIdx >= 120) ? (g.sampleMsHistoryIdx % 120) : 0;
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.1f ms", g.sampleMs);
                ImGui::PlotLines("ms/sample", g.sampleMsHistory, count, offset,
                                 overlay, 0.0f, FLT_MAX, ImVec2(-1, 60));
            }
        }

        ImGui::Separator();

        // ---- 快捷键提示 ----
        if (ImGui::CollapsingHeader("Shortcuts")) {
            ImGui::BulletText("Space  - Pause / Resume");
            ImGui::BulletText("R      - Reset accumulation");
            ImGui::BulletText("G      - Toggle GUI");
            ImGui::BulletText("ESC    - Quit");
        }

        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ============================================================================
// 键盘回调
// ============================================================================

static void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(window, GLFW_TRUE); break;
    case GLFW_KEY_SPACE: g.paused = !g.paused; break;
    case GLFW_KEY_R: g.needReset = true; break;
    case GLFW_KEY_G: g.showGui = !g.showGui; break;
    case GLFW_KEY_EQUAL: g.exposure *= 1.2f; break;
    case GLFW_KEY_MINUS: g.exposure /= 1.2f; break;
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    printf("=== VLR Interactive Progressive Viewer (with ImGui) ===\n\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i+1 < argc) g.width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i+1 < argc) g.height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) g.maxSamples = atoi(argv[++i]);
    }

    if (!glfwInit()) { fprintf(stderr, "Failed to init GLFW\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    char title[128];
    snprintf(title, sizeof(title), "VLR Progressive Viewer (%ux%u)", g.width, g.height);
    GLFWwindow* window = glfwCreateWindow(g.width, g.height, title, nullptr, nullptr);
    if (!window) { fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSetKeyCallback(window, keyCallback);
    glfwSwapInterval(0);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "Failed to init GLAD\n"); glfwTerminate(); return 1;
    }
    printf("[GL] %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    // ImGui 初始化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.Alpha = 0.95f;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // VLR 初始化
    VLRResult res;
    res = vlrCreateContext(nullptr, 0, &g.context);
    if (res != VLRResult_Success) { fprintf(stderr, "vlrCreateContext failed: %d\n", res); return 1; }
    res = vlrCreateScene(g.context, &g.scene);
    if (res != VLRResult_Success) { fprintf(stderr, "vlrCreateScene failed: %d\n", res); return 1; }

    printf("[VLR] Building Cornell Box scene...\n");
    if (!buildCornellBoxScene()) { fprintf(stderr, "Failed to build scene\n"); return 1; }

    initGL();

    vlrSetDenoiserConfig(g.context, true, false, false, 1.0f);

    printf("[VLR] Starting progressive render (%ux%u, max %d spp, denoiser ON)\n", g.width, g.height, g.maxSamples);
    vlrBeginProgressive(g.context, g.scene, g.width, g.height);

    g.lastFpsTime = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        renderOneSampleAndTonemap();

        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(shaderProg);
        glBindTexture(GL_TEXTURE_2D, g.texture);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        drawImGui();

        glfwSwapBuffers(window);

        // FPS 计算
        g.frameCount++;
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - g.lastFpsTime).count();
        if (elapsed >= 1.0f) {
            g.fps = g.frameCount / elapsed;
            g.frameCount = 0;
            g.lastFpsTime = now;
            snprintf(title, sizeof(title), "VLR Viewer | %u/%d spp | %.1f fps",
                     g.accumFrames, g.maxSamples, g.fps);
            glfwSetWindowTitle(window, title);
        }
    }

    printf("\n[Viewer] Exiting (%u samples rendered)\n", g.accumFrames);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (g.cudaPboResource) cudaGraphicsUnregisterResource(g.cudaPboResource);
    if (g.pbo) glDeleteBuffers(1, &g.pbo);
    if (g.texture) glDeleteTextures(1, &g.texture);
    if (g.scene) vlrDestroyScene(g.scene);
    if (g.context) vlrDestroyContext(g.context);
    glfwTerminate();
    return 0;
}
