// Malookys_Soundboard.cpp  --  NO Unity / IL2CPP / Photon calls.   (v2: hardened + local monitor)
// Build: see README.txt (CMake + Android NDK, arm64-v8a, API 26+)
//
//  * INPUT : hooks OpenXR to read right B (hold = menu) and a hand pose to "poke" buttons.
//  * MENU  : appends its own quad layer in xrEndFrame (GLES swapchain, scissor-clear drawing, built-in font).
//  * MIC   : hooks AAudio mic capture and mixes the sound into it -> game voice chat (Photon Voice) sends it to the lobby.
//  * LOCAL : opens its own AAudio output stream so YOU hear the sound too.
//
// Safety design: every hook calls the original first/always, never blocks the game thread for long,
// never throws, and switches the menu OFF permanently (kill switch) the moment anything looks wrong,
// so the game keeps running and keeps connecting to servers normally.
//
// Assumes OpenXR + OpenGL ES + AAudio mic. UNTESTED. Log: adb logcat -s MalookySB

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <dobby.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MalookySB", __VA_ARGS__)

// ================= CONFIG =================
static const char* SOUND_DIR_OVERRIDE = "";   // empty = auto: /storage/emulated/0/Android/data/<game package>/files/Malookys/
// (fallback folder tried too: /data/local/tmp/Malookys/ )
static const char* FILES[5]  = { "VineBoom.wav", "Bruh.wav", "Running.wav", "Slap.wav", "Fart.wav" };
static const char* NAMES[5]  = { "Vine Boom", "Bruh", "Running", "Slap", "Fart" };
static const char* MENU_HAND  = "/user/hand/right"; // B button hand
static const char* POKE_HAND  = "/user/hand/left";  // hand that presses buttons
static const float GAIN       = 1.0f;               // volume mixed into the mic (what others hear)
static const bool  LOCAL_MONITOR = true;            // you hear the sound too
static const float LOCAL_GAIN = 0.8f;               // volume you hear
static const bool  FLIP_Y = false;                  // true if the menu shows upside down
// ==========================================

static const int W = 400, H = 560;
static const int BX = 55, BW = 275, BH = 68;
static const int BY[5] = { 82, 179, 279, 371, 465 };

static double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// =====================================================================
//                              AUDIO
// =====================================================================
static std::vector<int16_t> g_pcm[5];          // 48kHz mono, loaded once, read-only afterwards
static std::atomic<bool> g_loaded{ false };
static std::atomic<int>  g_cur{ -1 };          // mic-mix state
static std::atomic<float> g_pos{ 0 };
static std::atomic<int>  g_lcur{ -1 };         // local monitor state
static std::atomic<int>  g_lpos{ 0 };

static std::string g_dir;
static void ResolveDir() {
    if (SOUND_DIR_OVERRIDE[0]) { g_dir = SOUND_DIR_OVERRIDE; return; }
    char pkg[256] = {}; FILE* c = fopen("/proc/self/cmdline", "rb");
    if (c) { size_t n = fread(pkg, 1, sizeof(pkg) - 1, c); pkg[n] = 0; fclose(c); }
    g_dir = std::string("/storage/emulated/0/Android/data/") + pkg + "/files/Malookys/";
    LOG("sound folder: %s", g_dir.c_str());
}
static bool LoadWav(const char* file, std::vector<int16_t>& out) {
    std::string p = g_dir + file;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { p = std::string("/data/local/tmp/Malookys/") + file; f = fopen(p.c_str(), "rb"); }
    if (!f) { LOG("missing %s%s", g_dir.c_str(), file); return false; }
    char hdr[12]; bool ok = false;
    if (fread(hdr, 1, 12, f) == 12) {
        while (!ok) {
            char id[4]; uint32_t sz;
            if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
            if (!memcmp(id, "data", 4)) {
                if (sz > 32u * 1024 * 1024) break;
                out.resize(sz / 2); size_t got = fread(out.data(), 2, sz / 2, f); out.resize(got); ok = got > 0;
            } else fseek(f, (long)sz + (sz & 1), SEEK_CUR);
        }
    }
    fclose(f); return ok; // expects 16-bit mono 48k PCM (the provided wavs)
}

// ---- local monitor (own AAudio output stream, opened lazily on first press) ----
static aaudio_data_callback_result_t OutCb(AAudioStream*, void*, void* data, int32_t frames) {
    int16_t* o = (int16_t*)data; int c = g_lcur.load(); int pos = g_lpos.load();
    const std::vector<int16_t>* pcm = (c >= 0 && c < 5) ? &g_pcm[c] : nullptr;
    for (int32_t f = 0; f < frames; f++) {
        if (pcm && pos < (int)pcm->size()) o[f] = (int16_t)((*pcm)[pos++] * LOCAL_GAIN);
        else { o[f] = 0; if (pcm) { g_lcur = -1; pcm = nullptr; } }
    }
    g_lpos = pos; return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void (*orig_setCb)(AAudioStreamBuilder*, AAudioStream_dataCallback, void*) = nullptr;
static std::atomic<bool> g_outStarted{ false };
static void* OpenOut(void*) {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return nullptr;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    if (orig_setCb) orig_setCb(b, OutCb, nullptr); else AAudioStreamBuilder_setDataCallback(b, OutCb, nullptr); // bypass our own hook
    AAudioStream* st = nullptr;
    if (AAudioStreamBuilder_openStream(b, &st) == AAUDIO_OK && st) { AAudioStream_requestStart(st); LOG("local monitor stream started"); }
    else LOG("local monitor stream failed");
    AAudioStreamBuilder_delete(b); return nullptr;
}

static void Play(int i) {
    if (i < 0 || i >= 5 || !g_loaded || g_pcm[i].empty()) return;
    g_pos = 0; g_cur = i;                                  // everyone else hears it (via mic)
    if (LOCAL_MONITOR) {
        g_lpos = 0; g_lcur = i;                            // you hear it
        if (!g_outStarted.exchange(true)) { pthread_t t; if (pthread_create(&t, nullptr, OpenOut, nullptr) == 0) pthread_detach(t); }
    }
}

// real-time safe: no allocation, no locks, no logging
static void Mix(void* data, int32_t frames, int32_t ch, aaudio_format_t fmt, int32_t rate) {
    int c = g_cur.load(); if (c < 0 || c >= 5 || !data || frames <= 0 || ch <= 0 || ch > 8 || rate <= 0) return;
    if (fmt != AAUDIO_FORMAT_PCM_I16 && fmt != AAUDIO_FORMAT_PCM_FLOAT) return;
    const auto& pcm = g_pcm[c]; float pos = g_pos.load(), step = 48000.f / rate;
    for (int32_t f = 0; f < frames; f++, pos += step) {
        size_t idx = (size_t)pos; if (idx >= pcm.size()) { g_cur = -1; break; }
        float s = pcm[idx] / 32768.f * GAIN;
        for (int32_t k = 0; k < ch; k++) {
            if (fmt == AAUDIO_FORMAT_PCM_I16) {
                int16_t* d = (int16_t*)data + (size_t)f * ch + k; int v = *d + (int)(s * 32767.f);
                *d = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            } else {
                float* d = (float*)data + (size_t)f * ch + k; float v = *d + s; *d = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
            }
        }
    }
    g_pos = pos;
}
static void MixStream(AAudioStream* st, void* data, int32_t frames) {
    if (!st || AAudioStream_getDirection(st) != AAUDIO_DIRECTION_INPUT) return;
    Mix(data, frames, AAudioStream_getChannelCount(st), AAudioStream_getFormat(st), AAudioStream_getSampleRate(st));
}

static int32_t (*orig_read)(AAudioStream*, void*, int32_t, int64_t) = nullptr;
static int32_t hk_read(AAudioStream* s, void* buf, int32_t frames, int64_t timeout) {
    int32_t r = orig_read(s, buf, frames, timeout);
    if (r > 0) MixStream(s, buf, r);
    return r;
}
struct CbCtx { AAudioStream_dataCallback cb; void* user; };
static aaudio_data_callback_result_t Tramp(AAudioStream* s, void* user, void* data, int32_t frames) {
    CbCtx* c = (CbCtx*)user; MixStream(s, data, frames); // mic data is already filled; mix BEFORE the game reads it
    return c->cb(s, c->user, data, frames);
}
static void hk_setCb(AAudioStreamBuilder* b, AAudioStream_dataCallback cb, void* user) {
    if (!cb) { orig_setCb(b, cb, user); return; }
    orig_setCb(b, Tramp, new CbCtx{ cb, user });
}

// =====================================================================
//                              OPENXR
// =====================================================================
static PFN_xrGetInstanceProcAddr orig_gipa = nullptr;
static PFN_xrCreateSession orig_xrCreateSession = nullptr;
static PFN_xrDestroySession orig_xrDestroySession = nullptr;
static PFN_xrCreateAction orig_xrCreateAction = nullptr;
static PFN_xrCreateActionSpace orig_xrCreateActionSpace = nullptr;
static PFN_xrDestroySpace orig_xrDestroySpace = nullptr;
static PFN_xrGetActionStateBoolean orig_xrGetActionStateBoolean = nullptr;
static PFN_xrEndFrame orig_xrEndFrame = nullptr;
static PFN_xrSuggestInteractionProfileBindings orig_xrSuggest = nullptr;

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSession g_sess = XR_NULL_HANDLE;
static bool g_gles = false;
static std::atomic<bool> g_dead{ false }; // kill switch: menu permanently off, game untouched
static std::mutex g_mx;
struct ActInfo { XrAction a; std::string name, paths; };
static std::vector<ActInfo> g_acts;
static XrAction g_bAction = XR_NULL_HANDLE;
static XrSpace g_hand = XR_NULL_HANDLE, g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
static std::atomic<bool> g_bHeld{ false };
// Role discovery from the game's own input bindings (works for any OpenXR game, no action-name guessing):
// role 0 = menu button (B), roles 1..5 = fallback sound keys, used when the menu can't be drawn.
static XrAction g_roleAct[6][4]; static int g_roleN[6]; static bool g_prevKey[6];
static XrAction g_poseAct[4]; static int g_poseN = 0;
static bool NoMenu();

template <class T> static T XR(const char* n) { T f = nullptr; if (orig_gipa && g_inst) orig_gipa(g_inst, n, (PFN_xrVoidFunction*)&f); return f; }
struct Fns {
    PFN_xrLocateSpace locate; PFN_xrCreateReferenceSpace mkRef; PFN_xrEnumerateSwapchainFormats enumFmt;
    PFN_xrCreateSwapchain create; PFN_xrEnumerateSwapchainImages enumImg; PFN_xrAcquireSwapchainImage acq;
    PFN_xrWaitSwapchainImage wait; PFN_xrReleaseSwapchainImage rel; PFN_xrPathToString pathToStr;
};
static Fns F{}; static XrInstance g_fnsInst = XR_NULL_HANDLE;
static bool LoadFns() {
    if (g_fnsInst == g_inst && F.locate) return true;
    if (!g_inst || !orig_gipa) return false;
    F.locate = XR<PFN_xrLocateSpace>("xrLocateSpace"); F.mkRef = XR<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
    F.enumFmt = XR<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats"); F.create = XR<PFN_xrCreateSwapchain>("xrCreateSwapchain");
    F.enumImg = XR<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages"); F.acq = XR<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
    F.wait = XR<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage"); F.rel = XR<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
    F.pathToStr = XR<PFN_xrPathToString>("xrPathToString");
    bool ok = F.locate && F.mkRef && F.enumFmt && F.create && F.enumImg && F.acq && F.wait && F.rel && F.pathToStr;
    if (ok) g_fnsInst = g_inst; return ok;
}

static std::string PathStr(XrPath p) {
    char buf[128] = {}; uint32_t n = 0;
    if (p != XR_NULL_PATH && LoadFns()) F.pathToStr(g_inst, p, sizeof(buf), &n, buf);
    return buf;
}
static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower(c); return s; }
static bool Has(const std::string& s, const char* sub) { return Lower(s).find(Lower(sub)) != std::string::npos; }

// ---- GL menu state ----
static XrSwapchain g_sc = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageOpenGLESKHR> g_imgs;
static std::vector<char> g_dirty;
static GLuint g_fbo = 0; static EGLContext g_ctx = EGL_NO_CONTEXT;
static bool g_open = false;
static XrPosef g_pose; static float g_yaw;
static XrCompositionLayerQuad g_quad;
static int g_hover = -1; static double g_pressUntil[5] = {}; static double g_lastPress[5] = {};
static int g_drawnSig = -1;

static bool NoMenu() { return g_dead || !g_gles; }
static void ResetMenuState() { // session went away: forget every handle that belonged to it
    g_sc = XR_NULL_HANDLE; g_imgs.clear(); g_dirty.clear(); g_fbo = 0; g_ctx = EGL_NO_CONTEXT;
    g_local = g_view = g_hand = XR_NULL_HANDLE; g_open = false; g_drawnSig = -1; g_bHeld = false;
}

static XrResult XRAPI_CALL hk_xrCreateSession(XrInstance i, const XrSessionCreateInfo* ci, XrSession* s) {
    XrResult r = orig_xrCreateSession(i, ci, s);
    if (XR_SUCCEEDED(r) && s && ci) {
        ResetMenuState(); g_sess = *s; g_gles = false;
        for (auto* p = (const XrBaseInStructure*)ci->next; p; p = p->next)
            if (p->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR) g_gles = true;
        LOG("session created, GLES=%d (0 = Vulkan: menu disabled, mic mixing still works)", g_gles);
    }
    return r;
}
static XrResult XRAPI_CALL hk_xrSuggest(XrInstance i, const XrInteractionProfileSuggestedBinding* sb) {
    if (sb && sb->suggestedBindings) {
        try {
            std::string mh = MENU_HAND, ph = POKE_HAND;
            const std::string keys[6] = { mh + "/input/b/click", "/user/hand/left/input/x/click", "/user/hand/left/input/y/click",
                                          "/user/hand/right/input/a/click", "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click" };
            const std::string pose = ph + "/input/grip/pose";
            for (uint32_t k = 0; k < sb->countSuggestedBindings; k++) {
                std::string path = Lower(PathStr(sb->suggestedBindings[k].binding)); XrAction a = sb->suggestedBindings[k].action;
                for (int r = 0; r < 6; r++) if (path == keys[r] && g_roleN[r] < 4) { g_roleAct[r][g_roleN[r]++] = a; LOG(">> role %d bound: %s", r, path.c_str()); }
                if (path == pose && g_poseN < 4) { g_poseAct[g_poseN++] = a; LOG(">> poke pose bound: %s", path.c_str()); }
            }
        } catch (...) {}
    }
    return orig_xrSuggest(i, sb);
}
static XrResult XRAPI_CALL hk_xrDestroySession(XrSession s) {
    if (s == g_sess) { ResetMenuState(); g_sess = XR_NULL_HANDLE; }
    return orig_xrDestroySession(s);
}
static XrResult XRAPI_CALL hk_xrCreateAction(XrActionSet set, const XrActionCreateInfo* ci, XrAction* a) {
    XrResult r = orig_xrCreateAction(set, ci, a);
    if (XR_SUCCEEDED(r) && a && ci) {
        try {
            ActInfo ai{ *a, ci->actionName, "" };
            for (uint32_t k = 0; k < ci->countSubactionPaths; k++) ai.paths += PathStr(ci->subactionPaths[k]) + " ";
            LOG("action '%s' type=%d paths=%s", ai.name.c_str(), (int)ci->actionType, ai.paths.c_str());
            std::lock_guard<std::mutex> lk(g_mx); g_acts.push_back(ai);
            std::string hs = Lower(MENU_HAND); hs = hs.substr(hs.rfind('/') + 1); // "right"
            if (ci->actionType == XR_ACTION_TYPE_BOOLEAN_INPUT && Has(ai.name, "secondary") && (Has(ai.name, hs.c_str()) || Has(ai.paths, MENU_HAND)))
                { g_bAction = *a; LOG(">> B action = %s", ai.name.c_str()); }
        } catch (...) {}
    }
    return r;
}
static XrResult XRAPI_CALL hk_xrCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* sp) {
    XrResult r = orig_xrCreateActionSpace(s, ci, sp);
    if (XR_SUCCEEDED(r) && sp && ci && g_hand == XR_NULL_HANDLE) {
        try {
            for (int k = 0; k < g_poseN; k++) if (g_poseAct[k] == ci->action) { g_hand = *sp; LOG(">> poke hand space set (binding)"); }
            if (g_hand != XR_NULL_HANDLE) return r;
            std::string hs = Lower(POKE_HAND); hs = hs.substr(hs.rfind('/') + 1); // "left"
            std::string sub = PathStr(ci->subactionPath), nm, ps;
            { std::lock_guard<std::mutex> lk(g_mx); for (auto& a : g_acts) if (a.a == ci->action) { nm = a.name; ps = a.paths; } }
            LOG("actionspace action='%s' sub='%s'", nm.c_str(), sub.c_str());
            if (sub == POKE_HAND || Has(ps, POKE_HAND) || Has(nm, hs.c_str())) { g_hand = *sp; LOG(">> poke hand space set"); }
        } catch (...) {}
    }
    return r;
}
static XrResult XRAPI_CALL hk_xrDestroySpace(XrSpace sp) {
    if (sp == g_hand) g_hand = XR_NULL_HANDLE;
    return orig_xrDestroySpace(sp);
}
static XrResult XRAPI_CALL hk_xrGetActionStateBoolean(XrSession s, const XrActionStateGetInfo* gi, XrActionStateBoolean* st) {
    XrResult r = orig_xrGetActionStateBoolean(s, gi, st);
    if (XR_SUCCEEDED(r) && gi && st) {
        bool v = st->currentState != 0;
        for (int k = 0; k < 6; k++) for (int j = 0; j < g_roleN[k]; j++) if (g_roleAct[k][j] == gi->action) {
            if (k == 0) g_bHeld = v;
            else if (v && !g_prevKey[k] && g_bHeld && NoMenu()) Play(k - 1); // no menu possible: hold B + X/Y/A/stick-clicks
            g_prevKey[k] = v;
        }
        if (g_bAction != XR_NULL_HANDLE && gi->action == g_bAction) g_bHeld = v; // name-based fallback
    }
    return r;
}

// ---------------- drawing (5x7 font, scissor clears, no shaders) ----------------
static const char GL_CHARS[] = " MalokysSundbrVieBmhRgpFt";
static const uint8_t GLYPH[25][7] = {
  {0,0,0,0,0,0,0},{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0,0,0x0E,0x01,0x0F,0x11,0x0F},{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
  {0,0,0x0E,0x11,0x11,0x11,0x0E},{0x10,0x10,0x12,0x14,0x18,0x14,0x12},{0,0,0x11,0x11,0x0F,0x01,0x0E},{0,0,0x0F,0x10,0x0E,0x01,0x1E},
  {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0,0,0x11,0x11,0x11,0x13,0x0D},{0,0,0x16,0x19,0x11,0x11,0x11},{0x01,0x01,0x0D,0x13,0x11,0x13,0x0D},
  {0x10,0x10,0x16,0x19,0x11,0x19,0x16},{0,0,0x16,0x19,0x10,0x10,0x10},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x04,0,0x0C,0x04,0x04,0x04,0x0E},
  {0,0,0x0E,0x11,0x1F,0x10,0x0E},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0,0,0x1A,0x15,0x15,0x15,0x15},{0x10,0x10,0x16,0x19,0x11,0x11,0x11},
  {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},{0,0x0F,0x11,0x11,0x0F,0x01,0x0E},{0,0,0x1E,0x11,0x11,0x1E,0x10},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
  {0x08,0x08,0x1C,0x08,0x08,0x09,0x06} };

static void Fill(int x, int yTop, int w, int h, float r, float g, float b) {
    int y = FLIP_Y ? yTop : (H - (yTop + h));
    glScissor(x, y, w, h); glClearColor(r, g, b, 1); glClear(GL_COLOR_BUFFER_BIT);
}
static void Text(const char* s, int cx, int cy, int sc) {
    int len = (int)strlen(s), x0 = cx - (len * 6 * sc) / 2, y0 = cy - (7 * sc) / 2;
    for (int i = 0; i < len; i++) {
        const char* p = strchr(GL_CHARS, s[i]); int gi = p ? (int)(p - GL_CHARS) : 0;
        for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++)
            if (GLYPH[gi][row] & (1 << (4 - col))) Fill(x0 + (i * 6 + col) * sc, y0 + row * sc, sc, sc, 1, 1, 1);
    }
}
static void DrawMenu(GLuint tex) {
    GLint fbo, sb[4], vp[4]; GLfloat cc[4]; GLboolean sc, cm[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo); glGetIntegerv(GL_SCISSOR_BOX, sb); glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc); glGetBooleanv(GL_COLOR_WRITEMASK, cm); sc = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, W, H); glColorMask(1, 1, 1, 1); glEnable(GL_SCISSOR_TEST);

    Fill(0, 0, W, H, 0, 0, 0);
    Text("Malookys Soundboard", W / 2, 24, 3);
    for (int i = 0; i < 5; i++) {
        bool pressed = NowS() < g_pressUntil[i];
        float inner = pressed ? 0.35f : (i == g_hover ? 0.18f : 0.f);
        Fill(BX, BY[i], BW, BH, 1, 1, 1);
        Fill(BX + 4, BY[i] + 4, BW - 8, BH - 8, inner, inner, inner);
        Text(NAMES[i], BX + BW / 2, BY[i] + BH / 2, 4);
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0); // detach, leave no reference to the swapchain image
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo); glScissor(sb[0], sb[1], sb[2], sb[3]); glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]); glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (!sc) glDisable(GL_SCISSOR_TEST);
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {} // don't leave GL errors behind for the game
}

static bool InitSwapchain() {
    if (g_sc) return true;
    if (!LoadFns()) return false;
    EGLContext ctx = eglGetCurrentContext(); if (ctx == EGL_NO_CONTEXT) return false;
    uint32_t n = 0; F.enumFmt(g_sess, 0, &n, nullptr); if (!n || n > 64) return false;
    std::vector<int64_t> fm(n); F.enumFmt(g_sess, n, &n, fm.data());
    int64_t fmt = fm[0]; for (auto f : fm) if (f == GL_RGBA8) fmt = f;
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; ci.format = fmt; ci.sampleCount = 1;
    ci.width = W; ci.height = H; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrSwapchain sc = XR_NULL_HANDLE;
    if (XR_FAILED(F.create(g_sess, &ci, &sc))) { LOG("swapchain create failed -> menu disabled"); g_dead = true; return false; }
    uint32_t cnt = 0; F.enumImg(sc, 0, &cnt, nullptr); if (!cnt || cnt > 16) { g_dead = true; return false; }
    g_imgs.assign(cnt, XrSwapchainImageOpenGLESKHR{ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
    F.enumImg(sc, cnt, &cnt, (XrSwapchainImageBaseHeader*)g_imgs.data());
    g_dirty.assign(cnt, 1); glGenFramebuffers(1, &g_fbo); g_ctx = ctx; g_sc = sc;
    LOG("menu swapchain ready (%u images)", cnt); return true;
}

static void Rot(const XrQuaternionf& q, float vx, float vy, float vz, float* o) {
    float cx = q.y * vz - q.z * vy + q.w * vx, cy = q.z * vx - q.x * vz + q.w * vy, cz = q.x * vy - q.y * vx + q.w * vz;
    o[0] = vx + 2 * (q.y * cz - q.z * cy); o[1] = vy + 2 * (q.z * cx - q.x * cz); o[2] = vz + 2 * (q.x * cy - q.y * cx);
}

static bool Tick(XrTime t) {
    bool held = g_bHeld;
    if (!held) { g_open = false; return false; }          // nothing to do unless B is held -> zero cost otherwise
    if (!LoadFns() || g_sess == XR_NULL_HANDLE) return false;
    if (!g_local) {
        XrReferenceSpaceCreateInfo rc{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rc.poseInReferenceSpace.orientation.w = 1;
        rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; if (XR_FAILED(F.mkRef(g_sess, &rc, &g_local))) { g_local = XR_NULL_HANDLE; return false; }
        rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  if (XR_FAILED(F.mkRef(g_sess, &rc, &g_view))) { g_view = XR_NULL_HANDLE; return false; }
    }
    if (!InitSwapchain()) return false;
    if (eglGetCurrentContext() != g_ctx) return false;    // never touch GL from a different context

    if (!g_open) { // place 0.8m in front of head, facing the player
        XrSpaceLocation hl{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(F.locate(g_view, g_local, t, &hl)) || !(hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
        float f[3]; Rot(hl.pose.orientation, 0, 0, -1, f); float l = sqrtf(f[0] * f[0] + f[2] * f[2]); if (l < 1e-4f) return false;
        f[0] /= l; f[2] /= l;
        g_yaw = atan2f(-f[0], -f[2]);
        g_pose.position = { hl.pose.position.x + f[0] * 0.8f, hl.pose.position.y - 0.1f, hl.pose.position.z + f[2] * 0.8f };
        g_pose.orientation = { 0, sinf(g_yaw / 2), 0, cosf(g_yaw / 2) };
        g_open = true; std::fill(g_dirty.begin(), g_dirty.end(), 1); g_drawnSig = -1;
    }

    int hover = -1; // poke test
    if (g_hand) {
        XrSpaceLocation hl{ XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(F.locate(g_hand, g_local, t, &hl)) && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            float dx = hl.pose.position.x - g_pose.position.x, dy = hl.pose.position.y - g_pose.position.y, dz = hl.pose.position.z - g_pose.position.z;
            float lx = dx * cosf(g_yaw) - dz * sinf(g_yaw), lz = dx * sinf(g_yaw) + dz * cosf(g_yaw);
            float px = lx * 1000.f + W / 2, py = H / 2 - dy * 1000.f;
            if (fabsf(lz) < 0.06f)
                for (int i = 0; i < 5; i++)
                    if (px >= BX && px <= BX + BW && py >= BY[i] && py <= BY[i] + BH) {
                        hover = i;
                        if (NowS() - g_lastPress[i] > 0.6) { g_lastPress[i] = NowS(); g_pressUntil[i] = NowS() + 0.25; Play(i); }
                    }
        }
    }
    g_hover = hover;
    int sig = hover + 1; for (int i = 0; i < 5; i++) if (NowS() < g_pressUntil[i]) sig |= 1 << (4 + i);
    if (sig != g_drawnSig) { g_drawnSig = sig; std::fill(g_dirty.begin(), g_dirty.end(), 1); }

    uint32_t idx = 0; if (XR_FAILED(F.acq(g_sc, nullptr, &idx)) || idx >= g_dirty.size()) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100 * 1000 * 1000; // 100 ms, never infinite
    if (XR_FAILED(F.wait(g_sc, &wi))) { LOG("swapchain wait failed -> menu disabled"); g_dead = true; return false; }
    if (g_dirty[idx]) { DrawMenu(g_imgs[idx].image); g_dirty[idx] = 0; }
    F.rel(g_sc, nullptr);

    g_quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    g_quad.space = g_local; g_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_quad.subImage.swapchain = g_sc; g_quad.subImage.imageRect = { {0, 0}, {W, H} };
    g_quad.pose = g_pose; g_quad.size = { W / 1000.f, H / 1000.f };
    return true;
}

static XrResult XRAPI_CALL hk_xrEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    if (!g_dead && ei && s == g_sess && g_gles && ei->layerCount < 15) {
        bool add = false;
        try { add = Tick(ei->displayTime); } catch (...) { g_dead = true; }
        if (add) {
            const XrCompositionLayerBaseHeader* arr[16];
            for (uint32_t i = 0; i < ei->layerCount; i++) arr[i] = ei->layers[i];
            arr[ei->layerCount] = (const XrCompositionLayerBaseHeader*)&g_quad;
            XrFrameEndInfo e2 = *ei; e2.layerCount = ei->layerCount + 1; e2.layers = arr;
            return orig_xrEndFrame(s, &e2);
        }
    }
    return orig_xrEndFrame(s, ei);
}

static XrResult XRAPI_CALL hk_gipa(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    XrResult r = orig_gipa(inst, name, fn);
    if (XR_FAILED(r) || !fn || !*fn || !name) return r;
    if (inst != XR_NULL_HANDLE) g_inst = inst;
#define HK(f) if (!strcmp(name, #f)) { orig_##f = (PFN_##f)*fn; *fn = (PFN_xrVoidFunction)hk_##f; }
    HK(xrCreateSession) HK(xrDestroySession) HK(xrCreateAction) HK(xrCreateActionSpace) HK(xrDestroySpace)
    HK(xrGetActionStateBoolean) HK(xrEndFrame)
    if (!strcmp(name, "xrSuggestInteractionProfileBindings")) { orig_xrSuggest = (PFN_xrSuggestInteractionProfileBindings)*fn; *fn = (PFN_xrVoidFunction)hk_xrSuggest; }
    return r;
}

// =====================================================================
static void* Init(void*) {
    ResolveDir();
    for (int i = 0; i < 5; i++) LoadWav(FILES[i], g_pcm[i]);
    g_loaded = true;

    if (void* aa = dlopen("libaaudio.so", RTLD_NOW)) {
        void* a = dlsym(aa, "AAudioStream_read"); void* b = dlsym(aa, "AAudioStreamBuilder_setDataCallback");
        if (b) DobbyHook(b, (void*)hk_setCb, (void**)&orig_setCb);
        if (a) DobbyHook(a, (void*)hk_read, (void**)&orig_read);
        LOG("AAudio hooked");
    }
    void* ld = nullptr;
    for (int i = 0; i < 6000 && !(ld = dlopen("libopenxr_loader.so", RTLD_NOLOAD)); i++) usleep(20000); // give up after 2 min
    if (!ld) { LOG("no OpenXR loader found: menu disabled, mic mixing active"); return nullptr; }
    if (void* g = dlsym(ld, "xrGetInstanceProcAddr")) { DobbyHook(g, (void*)hk_gipa, (void**)&orig_gipa); LOG("OpenXR hooked"); }
    else LOG("xrGetInstanceProcAddr not found");
    return nullptr;
}
__attribute__((constructor)) static void OnLoad() { pthread_t t; if (pthread_create(&t, nullptr, Init, nullptr) == 0) pthread_detach(t); }
