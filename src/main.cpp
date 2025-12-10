#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#include <vector>
#include <chrono>
#include <mutex>

static std::vector<double> g_clicktimes;
static std::mutex g_clickmutex;
static const double cps_window = 1.0;

static double gettime() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void registerclick() {
    std::lock_guard<std::mutex> lock(g_clickmutex);
    g_clicktimes.push_back(gettime());
}

static int getcps() {
    std::lock_guard<std::mutex> lock(g_clickmutex);
    double now = gettime();
    while (!g_clicktimes.empty() && (now - g_clicktimes.front()) > cps_window) {
        g_clicktimes.erase(g_clicktimes.begin());
    }
    return (int)g_clicktimes.size();
}

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

static void (*orig_input1)(void*, void*, void*) = nullptr;
static void hook_input1(void* thiz, void* a1, void* a2) {
    if (orig_input1) orig_input1(thiz, a1, a2);
    if (thiz && g_initialized) {
        AInputEvent* event = (AInputEvent*)thiz;
        ImGui_ImplAndroid_HandleInputEvent(event);
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
            int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
            if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                registerclick();
            }
        }
    }
}

static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
        if (AInputEvent_getType(*event) == AINPUT_EVENT_TYPE_MOTION) {
            int32_t action = AMotionEvent_getAction(*event) & AMOTION_EVENT_ACTION_MASK;
            if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                registerclick();
            }
        }
    }
    return result;
}

struct glstate {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.atex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bdst);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void restoregl(const glstate& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.atex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bsrc, s.bdst);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void drawmenu() {
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("CPS", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("CPS: %d", getcps());
    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;
    
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    
    float scale = (float)g_height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;
    
    ImFontConfig cfg;
    cfg.SizePixels = 32.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale);
    
    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;
    
    glstate s;
    savegl(s);
    
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    drawmenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    restoregl(s);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);
    
    if (g_targetcontext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_targetcontext = ctx;
            g_targetsurface = surf;
        }
    }
    
    if (ctx != g_targetcontext || surf != g_targetsurface)
        return orig_eglswapbuffers(dpy, surf);
    
    g_width = w;
    g_height = h;
    setup();
    render();
    
    return orig_eglswapbuffers(dpy, surf);
}

static void hookinput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_input1, (void**)&orig_input1);
        if (h) return;
    }
    
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_input2, (void**)&orig_input2);
        if (h) return;
    }
}

static void* mainthread(void*) {
    sleep(3);
    
    GlossInit(true);
    
    GHandle hegl = GlossOpen("libEGL.so");
    if (!hegl) return nullptr;
    
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;
    
    GHook h = GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
    if (!h) return nullptr;
    
    hookinput();
    
    return nullptr;
}

__attribute__((constructor))
void displaycps_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
