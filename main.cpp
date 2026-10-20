// Counter Flame ESP — internal, Unity il2cpp arm64. Offsets from dump.cs (build in the repo); everything else resolved via il2cpp exports.
// Main thread (PlayerFPSCamera.LateUpdate hook): scan + project + touch -> snapshot. Render thread (eglSwapBuffers hook): ImGui only, no Unity calls.
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <vector>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "dobby.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "CFESP", __VA_ARGS__)

// ---- offsets (dump.cs) -------------------------------------------------------------------------
namespace off {
    constexpr size_t Obj_CachedPtr   = 0x10;  // UnityEngine.Object.m_CachedPtr
    constexpr size_t PC_fpsCamera    = 0x28;  // PlayerController._fpsCamera
    constexpr size_t PC_view         = 0x80;  // PlayerController._view (PhotonView)
    constexpr size_t PC_player       = 0x90;  // PlayerController._player (Photon Player)
    constexpr size_t PC_isDeath      = 0xa8;  // PlayerController._isDeath
    constexpr size_t SK_head         = 0xa8;  // PlayerSkeleton._headBone  (Rigidbody)
    constexpr size_t SK_chest        = 0xb0;  // PlayerSkeleton._chestBone
    constexpr size_t SK_pelvis       = 0xb8;  // PlayerSkeleton._pelvisBone
    constexpr size_t Str_len         = 0x10;  // Il2CppString length
    constexpr size_t Str_chars       = 0x14;  // Il2CppString UTF-16 data
    constexpr size_t Arr_len         = 0x18;  // Il2CppArray max_length
    constexpr size_t Arr_data        = 0x20;
}
static const char* TEAM_KEY = "team"; // FILL: Photon Player.CustomProperties key holding "CT"/"TR" — unknown, not in dump.cs. Unknown team => everyone drawn as enemy.
static const char* CFG_PATH = "/data/local/tmp/cf_esp.json";

// ---- il2cpp api --------------------------------------------------------------------------------
static void*  (*il2cpp_domain_get)();
static void** (*il2cpp_domain_get_assemblies)(void*, size_t*);
static void*  (*il2cpp_assembly_get_image)(void*);
static void*  (*il2cpp_class_from_name)(void*, const char*, const char*);
static void*  (*il2cpp_class_get_method_from_name)(void*, const char*, int);
static void*  (*il2cpp_class_get_type)(void*);
static void*  (*il2cpp_type_get_object)(void*);
static void*  (*il2cpp_object_get_class)(void*);
static void*  (*il2cpp_string_new)(const char*);

struct Vec3 { float x, y, z; };
struct Touch { int id; float px, py, rx, ry, dx, dy, dt; int tap, phase, type; float pr, mpr, rad, radv, alt, az; };

static void* klass(const char* ns, const char* name) {
    size_t n = 0; void** asm_ = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) { void* c = il2cpp_class_from_name(il2cpp_assembly_get_image(asm_[i]), ns, name); if (c) return c; }
    return nullptr;
}
static void* method(void* k, const char* name, int argc) { return k ? il2cpp_class_get_method_from_name(k, name, argc) : nullptr; }
static void* mptr(void* mi) { return mi ? *(void**)mi : nullptr; }  // MethodInfo.methodPointer is first field

// managed method table
static void* mi_get_transform, *mi_get_position, *mi_w2s, *mi_find, *mi_getcic, *mi_get_main, *mi_get_health, *mi_isMine, *mi_scrW, *mi_scrH, *mi_getTouch, *mi_touchCount;
static void* t_PlayerController, *t_PlayerSkeleton;
using fn_transform = void* (*)(void*, void*);
using fn_pos       = Vec3  (*)(void*, void*);
using fn_w2s       = Vec3  (*)(void*, Vec3, void*);
using fn_find      = void* (*)(void*, void*);
using fn_getcic    = void* (*)(void*, void*, bool, void*);
using fn_main      = void* (*)(void*);
using fn_int       = int   (*)(void*, void*);
using fn_bool      = bool  (*)(void*, void*);
using fn_sint      = int   (*)(void*);
using fn_touch     = Touch (*)(int, void*);

static inline bool alive(void* o) { return o && *(void**)((char*)o + off::Obj_CachedPtr); }
static bool vpos(void* comp, Vec3& out) {
    if (!alive(comp)) return false;
    void* tr = ((fn_transform)mptr(mi_get_transform))(comp, mi_get_transform);
    if (!alive(tr)) return false;
    out = ((fn_pos)mptr(mi_get_position))(tr, mi_get_position);
    return true;
}

// ---- config ------------------------------------------------------------------------------------
struct Cfg {
    bool  esp = false, box = true, bones = true, health = true, dist = true, snap = false, team = true;
    float maxDist = 300.f, scale = 2.5f, col[3] = {1.f, 0.25f, 0.25f};
    bool  menu = true;
} cfg;

static void cfgSave() {
    FILE* f = fopen(CFG_PATH, "w"); if (!f) return;
    fprintf(f, "{\n\"esp\":%d,\"box\":%d,\"bones\":%d,\"health\":%d,\"dist\":%d,\"snap\":%d,\"team\":%d,\n\"maxDist\":%.1f,\"scale\":%.2f,\"r\":%.3f,\"g\":%.3f,\"b\":%.3f\n}\n",
            cfg.esp, cfg.box, cfg.bones, cfg.health, cfg.dist, cfg.snap, cfg.team, cfg.maxDist, cfg.scale, cfg.col[0], cfg.col[1], cfg.col[2]);
    fclose(f);
}
static bool jget(const char* s, const char* key, double& v) {
    char k[32]; snprintf(k, sizeof k, "\"%s\":", key);
    const char* p = strstr(s, k); if (!p) return false;
    return sscanf(p + strlen(k), "%lf", &v) == 1;
}
static void cfgLoad() {
    FILE* f = fopen(CFG_PATH, "r"); if (!f) return;
    char buf[1024] = {0}; fread(buf, 1, sizeof buf - 1, f); fclose(f);
    double v;
    if (jget(buf, "esp", v)) cfg.esp = v != 0;       if (jget(buf, "box", v)) cfg.box = v != 0;
    if (jget(buf, "bones", v)) cfg.bones = v != 0;   if (jget(buf, "health", v)) cfg.health = v != 0;
    if (jget(buf, "dist", v)) cfg.dist = v != 0;     if (jget(buf, "snap", v)) cfg.snap = v != 0;
    if (jget(buf, "team", v)) cfg.team = v != 0;     if (jget(buf, "maxDist", v)) cfg.maxDist = (float)v;
    if (jget(buf, "scale", v)) cfg.scale = (float)v;
    if (jget(buf, "r", v)) cfg.col[0] = (float)v;    if (jget(buf, "g", v)) cfg.col[1] = (float)v;    if (jget(buf, "b", v)) cfg.col[2] = (float)v;
}

// ---- snapshot (main thread writes, render thread reads) ----------------------------------------
struct P2 { float x, y; bool ok; };
struct Ent { P2 feet, head, hb, chest, pelvis; float hp, dist; bool enemy, bones; };
static std::mutex g_mx;
static std::vector<Ent> g_ents;
static int   g_w = 0, g_h = 0;
static bool  g_down = false; static float g_tx = 0, g_ty = 0;
static double g_stamp = 0;

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ---- main-thread scan --------------------------------------------------------------------------
struct Cached { void* pc; void* skel; bool local; int team; };
static std::vector<Cached> g_cache;
static double g_lastScan = 0;

static int readTeam(void* pc) {
    void* player = *(void**)((char*)pc + off::PC_player); if (!player) return 0;
    void* pk = il2cpp_object_get_class(player);
    void* mi = method(pk, "get_CustomProperties", 0); if (!mi) return 0;
    void* ht = ((fn_find)mptr(mi))(player, mi); if (!ht) return 0;
    void* hk = il2cpp_object_get_class(ht);
    void* gi = method(hk, "get_Item", 1); if (!gi) return 0;
    void* key = il2cpp_string_new(TEAM_KEY);
    using fn_item = void* (*)(void*, void*, void*);
    void* val = ((fn_item)mptr(gi))(ht, key, gi); if (!val) return 0;
    // expect Il2CppString "CT"/"TR"; anything else => unknown
    int len = *(int*)((char*)val + off::Str_len); if (len != 2) return 0;
    uint16_t* c = (uint16_t*)((char*)val + off::Str_chars);
    if (c[0] == 'C' && c[1] == 'T') return 1;
    if (c[0] == 'T' && c[1] == 'R') return 2;
    return 0;
}

static void rescan() {
    g_cache.clear();
    void* tobj = il2cpp_type_get_object(il2cpp_class_get_type(t_PlayerController));
    void* arr = ((fn_find)mptr(mi_find))(tobj, mi_find);
    if (!arr) return;
    int n = *(int*)((char*)arr + off::Arr_len);
    void** el = (void**)((char*)arr + off::Arr_data);
    void* sktype = t_PlayerSkeleton ? il2cpp_type_get_object(il2cpp_class_get_type(t_PlayerSkeleton)) : nullptr;
    for (int i = 0; i < n; i++) {
        void* pc = el[i]; if (!alive(pc)) continue;
        Cached c{pc, nullptr, false, 0};
        void* view = *(void**)((char*)pc + off::PC_view);
        if (view) c.local = ((fn_bool)mptr(mi_isMine))(view, mi_isMine);
        c.team = readTeam(pc);
        if (sktype && mi_getcic) c.skel = ((fn_getcic)mptr(mi_getcic))(pc, sktype, true, mi_getcic);
        g_cache.push_back(c);
    }
}

static bool w2s(void* cam, Vec3 p, P2& o) {
    Vec3 s = ((fn_w2s)mptr(mi_w2s))(cam, p, mi_w2s);
    o.ok = s.z > 0.01f; o.x = s.x; o.y = g_h - s.y;  // Unity screen origin bottom-left -> flip Y for ImGui
    return o.ok;
}

static void frame() {
    int W = ((fn_sint)mptr(mi_scrW))(mi_scrW), H = ((fn_sint)mptr(mi_scrH))(mi_scrH);
    bool down = false; float tx = 0, ty = 0;
    if (((fn_sint)mptr(mi_touchCount))(mi_touchCount) > 0) {
        Touch t = ((fn_touch)mptr(mi_getTouch))(0, mi_getTouch);
        down = t.phase != 3 && t.phase != 4; tx = t.px; ty = H - t.py;
    }
    std::vector<Ent> out;
    if (cfg.esp) {
        double t = now();
        if (t - g_lastScan > 0.25) { rescan(); g_lastScan = t; }
        void* cam = nullptr; Vec3 me{}; int myTeam = 0; bool haveLocal = false;
        for (auto& c : g_cache) if (c.local && alive(c.pc)) {
            haveLocal = true; myTeam = c.team;
            cam = *(void**)((char*)c.pc + off::PC_fpsCamera);
            vpos(c.pc, me);
        }
        if (!alive(cam)) cam = ((fn_main)mptr(mi_get_main))(mi_get_main);
        if (haveLocal && alive(cam)) {
            g_h = H;
            for (auto& c : g_cache) {
                if (c.local || !alive(c.pc) || *(bool*)((char*)c.pc + off::PC_isDeath)) continue;
                Vec3 p; if (!vpos(c.pc, p)) continue;
                float d = sqrtf((p.x-me.x)*(p.x-me.x) + (p.y-me.y)*(p.y-me.y) + (p.z-me.z)*(p.z-me.z));
                if (d > cfg.maxDist) continue;
                bool enemy = !(cfg.team && myTeam && c.team && myTeam == c.team);
                if (!enemy) continue;
                Ent e{}; e.dist = d; e.enemy = enemy;
                e.hp = (float)((fn_int)mptr(mi_get_health))(c.pc, mi_get_health);
                if (!w2s(cam, p, e.feet) || !w2s(cam, {p.x, p.y + 1.8f, p.z}, e.head)) continue;
                if (alive(c.skel)) {
                    void* hb = *(void**)((char*)c.skel + off::SK_head);
                    void* cb = *(void**)((char*)c.skel + off::SK_chest);
                    void* pb = *(void**)((char*)c.skel + off::SK_pelvis);
                    Vec3 a, b2, c2;
                    if (vpos(hb, a) && vpos(cb, b2) && vpos(pb, c2))
                        e.bones = w2s(cam, a, e.hb) & w2s(cam, b2, e.chest) & w2s(cam, c2, e.pelvis);
                }
                out.push_back(e);
            }
        }
    }
    std::lock_guard<std::mutex> l(g_mx);
    g_ents.swap(out); g_w = W; g_h = H; g_down = down; g_tx = tx; g_ty = ty; g_stamp = now();
}

// ---- hooks -------------------------------------------------------------------------------------
static void (*orig_late)(void*, void*);
static void hk_late(void* self, void* mi) { orig_late(self, mi); frame(); }

static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface);
static bool g_init = false;

static void drawEsp(ImDrawList* dl, int W, int H, const std::vector<Ent>& ents) {
    ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(cfg.col[0], cfg.col[1], cfg.col[2], 1.f)), blk = IM_COL32(0, 0, 0, 200);
    for (auto& e : ents) {
        float h = fabsf(e.feet.y - e.head.y), w = h * 0.45f, cx = e.feet.x, top = e.head.y, bot = e.feet.y;
        if (cfg.box) { dl->AddRect({cx-w/2-1, top-1}, {cx+w/2+1, bot+1}, blk, 0, 0, 3.f); dl->AddRect({cx-w/2, top}, {cx+w/2, bot}, col, 0, 0, 1.5f); }
        if (cfg.health) {
            float f = fminf(fmaxf(e.hp / 100.f, 0.f), 1.f);  // FILL: max health assumed 100
            dl->AddRectFilled({cx-w/2-7, top-1}, {cx-w/2-3, bot+1}, blk);
            dl->AddRectFilled({cx-w/2-6, bot - h*f}, {cx-w/2-4, bot}, IM_COL32((int)(255*(1-f)), (int)(255*f), 40, 255));
        }
        if (cfg.dist) { char b[24]; snprintf(b, sizeof b, "%.0fm", e.dist); dl->AddText({cx - ImGui::CalcTextSize(b).x/2, bot + 2}, IM_COL32_WHITE, b); }
        if (cfg.bones && e.bones) {
            dl->AddLine({e.hb.x, e.hb.y}, {e.chest.x, e.chest.y}, col, 1.5f);
            dl->AddLine({e.chest.x, e.chest.y}, {e.pelvis.x, e.pelvis.y}, col, 1.5f);
            dl->AddCircle({e.hb.x, e.hb.y}, h * 0.07f, col, 12, 1.5f);
        }
        if (cfg.snap) dl->AddLine({W * 0.5f, (float)H}, {cx, bot}, col, 1.f);
    }
}

static void drawMenu() {
    static ImVec2 tpos{20, 20};
    ImGui::SetNextWindowPos(tpos, ImGuiCond_FirstUseEver);
    ImGui::Begin("##tog", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("INSERT");
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(0)) {
        float dx = io.MousePos.x - io.MouseClickedPos[0].x, dy = io.MousePos.y - io.MouseClickedPos[0].y;
        if (dx*dx + dy*dy < 100.f) cfg.menu = !cfg.menu;
    }
    ImGui::End();
    if (!cfg.menu) return;
    ImGui::SetNextWindowSize({io.DisplaySize.x * 0.45f, io.DisplaySize.y * 0.6f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Counter Flame", &cfg.menu)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("ESP")) {
                ImGui::Checkbox("Enable", &cfg.esp); ImGui::Checkbox("Box", &cfg.box); ImGui::Checkbox("Skeleton", &cfg.bones);
                ImGui::Checkbox("Health", &cfg.health); ImGui::Checkbox("Distance", &cfg.dist); ImGui::Checkbox("Snaplines", &cfg.snap);
                ImGui::Checkbox("Team check", &cfg.team);
                ImGui::SliderFloat("Max dist", &cfg.maxDist, 20.f, 600.f, "%.0f"); ImGui::ColorEdit3("Color", cfg.col, ImGuiColorEditFlags_NoInputs);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("CONFIG")) {
                if (ImGui::Button("Save")) cfgSave(); ImGui::SameLine(); if (ImGui::Button("Load")) cfgLoad();
                ImGui::SliderFloat("UI scale", &cfg.scale, 1.f, 4.f, "%.1f");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

static EGLBoolean hk_swap(EGLDisplay d, EGLSurface s) {
    EGLint w = 0, h = 0; eglQuerySurface(d, s, EGL_WIDTH, &w); eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 64 || h < 64) return orig_swap(d, s);
    if (!g_init) {
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        cfgLoad(); g_init = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {(float)w, (float)h}; io.FontGlobalScale = cfg.scale; io.DeltaTime = 1.f / 60.f;
    std::vector<Ent> ents; bool down; float tx, ty; double st;
    { std::lock_guard<std::mutex> l(g_mx); ents = g_ents; down = g_down; tx = g_tx; ty = g_ty; st = g_stamp; }
    io.AddMousePosEvent(tx, ty); io.AddMouseButtonEvent(0, down);
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
    if (now() - st < 0.2 && cfg.esp) drawEsp(ImGui::GetBackgroundDrawList(), w, h, ents);
    drawMenu();
    ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return orig_swap(d, s);
}

// ---- bootstrap ---------------------------------------------------------------------------------
template <class T> static bool sym(void* h, const char* n, T& f) { f = (T)dlsym(h, n); return f != nullptr; }

static void* boot(void*) {
    void* h = nullptr;
    while (!(h = dlopen("libil2cpp.so", RTLD_NOLOAD))) sleep(1);
    #define S(n) if (!sym(h, #n, n)) { LOG("missing " #n); return nullptr; }
    S(il2cpp_domain_get) S(il2cpp_domain_get_assemblies) S(il2cpp_assembly_get_image) S(il2cpp_class_from_name)
    S(il2cpp_class_get_method_from_name) S(il2cpp_class_get_type) S(il2cpp_type_get_object) S(il2cpp_object_get_class) S(il2cpp_string_new)
    #undef S
    void *tComp, *tObj, *tCam, *tScr, *tInp, *tView, *tFps;
    do { sleep(2);
        t_PlayerController = klass("", "PlayerController"); t_PlayerSkeleton = klass("", "PlayerSkeleton"); tFps = klass("", "PlayerFPSCamera");
        tComp = klass("UnityEngine", "Component"); tObj = klass("UnityEngine", "Object"); tCam = klass("UnityEngine", "Camera");
        tScr = klass("UnityEngine", "Screen"); tInp = klass("UnityEngine", "Input"); tView = klass("Photon.Pun", "PhotonView");
    } while (!(t_PlayerController && tFps && tComp && tObj && tCam && tScr && tInp && tView));
    mi_get_transform = method(tComp, "get_transform", 0);
    mi_getcic  = method(tComp, "GetComponentInChildren", 2);
    mi_find    = method(tObj, "FindObjectsOfType", 1);
    mi_get_main= method(tCam, "get_main", 0);
    mi_w2s     = method(tCam, "WorldToScreenPoint", 1);
    mi_scrW    = method(tScr, "get_width", 0); mi_scrH = method(tScr, "get_height", 0);
    mi_getTouch= method(tInp, "GetTouch", 1); mi_touchCount = method(tInp, "get_touchCount", 0);
    mi_isMine  = method(tView, "get_IsMine", 0);
    mi_get_health = method(t_PlayerController, "get_Health", 0);
    void* tTr = klass("UnityEngine", "Transform"); mi_get_position = method(tTr, "get_position", 0);
    void* late = method(tFps, "LateUpdate", 0);
    if (!(mi_get_transform && mi_find && mi_get_main && mi_w2s && mi_scrW && mi_scrH && mi_getTouch && mi_touchCount && mi_isMine && mi_get_health && mi_get_position && late)) { LOG("method resolve failed"); return nullptr; }
    DobbyHook(mptr(late), (void*)hk_late, (void**)&orig_late);
    void* swap = DobbySymbolResolver("libEGL.so", "eglSwapBuffers");
    if (swap) DobbyHook(swap, (void*)hk_swap, (void**)&orig_swap);
    LOG("hooked");
    return nullptr;
}

__attribute__((constructor)) static void entry() { pthread_t t; pthread_create(&t, nullptr, boot, nullptr); pthread_detach(t); }
