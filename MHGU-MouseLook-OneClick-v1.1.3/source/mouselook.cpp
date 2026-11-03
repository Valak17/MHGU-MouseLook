/*
 * mouselook.cpp - native mouse-look for the recompiled PC build of MHGU (Forge PC plugin).
 *
 * Camera motion is written directly into the game's own angle fields. Raw mouse deltas are
 * not converted into a virtual right stick. The calibration scans only the object graph
 * reachable from the sFestaCamera singleton and stores pointer paths, so later launches do
 * not repeat a heap-wide scan.
 *
 * SPDX-License-Identifier: MIT
 */
#include "forge_pc.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

FORGE_PLUGIN_DEFINE_API

static constexpr const char* kExpectedExeHash =
    "053CAE60DA1BFF123E391E5582A650C1B25E200A0F09A0DD265E321225F38FE5";
static constexpr int kMaxCamSlots = 12;
static constexpr int kButtonCount = 5;
static constexpr int kKeyTokenMax = 48;
static constexpr double kMinCalibrationAxisDegrees = 18.0;
static constexpr double kMinCalibrationSymmetry = 0.62;

/* ---------------------------------------------------------------- logging */

static void LOG(ForgeLogLevel lvl, const char* fmt, ...) {
    if (!forge_api || !forge_api->log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    forge_api->log(lvl, "MouseLook", buf);
}

#define LOGI(...) LOG(FORGE_LOG_INFO, __VA_ARGS__)
#define LOGW(...) LOG(FORGE_LOG_WARN, __VA_ARGS__)
#define LOGE(...) LOG(FORGE_LOG_ERROR, __VA_ARGS__)

/* ---------------------------------------------------------------- settings */

enum SlotType : int {
    SLOT_F32 = 0,
    SLOT_I32 = 1,
    SLOT_I16_LO = 2,
    SLOT_I16_HI = 3,
};

struct Settings {
    float sens_x = 0.06f;
    float sens_y = 0.06f;
    bool invert_x = false;
    bool invert_y = false;
    bool enabled = true;
    bool auto_switch = true;
    int handback_ms = 700;
    int activate_px = 2;
    int button_hold_ms = 100;
    int wheel_hold_ms = 100;
    bool hide_cursor = true;
    bool clip_cursor = false;
    bool center_cursor = true;
    bool cursor_fallback = true;
    bool beep = true;
    bool debug = false;
    bool camera_vector = true;
    float camera_radius = 500.0f;

    int probe_chunks = 1;
    int probe_bytes = 4096;
    int max_depth = 1;
    char camera_root[64] = "sFestaCamera";
    char stick_keys[kKeyTokenMax] = "IJKL";

    char key_lmb[kKeyTokenMax] = "Z";
    char key_rmb[kKeyTokenMax] = "X";
    char key_mmb[kKeyTokenMax] = "F";
    char key_x1[kKeyTokenMax] = "SPACE";
    char key_x2[kKeyTokenMax] = "E";
    char wheel_hold[kKeyTokenMax] = "";
    char wheel_up[kKeyTokenMax] = "3";
    char wheel_down[kKeyTokenMax] = "4";
};

struct CameraSlot {
    std::vector<uint32_t> hops;
    uint32_t field = 0;
    int type = SLOT_F32;
    int dir = 1;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

static Settings g_cfg;
static ForgeConfig* g_conf = nullptr;
static char g_mod_dir[512] = ".";
static char g_exe_hash[65] = "";
static bool g_exe_hash_ok = false;

/* camera state */
static std::vector<CameraSlot> g_yaw;
static std::vector<CameraSlot> g_pitch;
static float g_units_per_turn = 6.283185307179586f;
static double g_pitch_min = -1.0;
static double g_pitch_max = 1.0;
static bool g_have_camera = false;
static bool g_cache_loaded = false;
static bool g_cache_was_valid = false;
static float g_cache_check_timer = 0.0f;
static float g_bad_path_seconds = 0.0f;
static std::vector<double> g_yaw_targets;
static std::vector<double> g_pitch_targets;
static std::vector<uint8_t> g_yaw_target_ready;
static std::vector<uint8_t> g_pitch_target_ready;
static bool g_targets_valid = false;
static uint32_t g_vector_camera_object = 0;
static double g_vector_yaw_offset = 0.0;
static double g_vector_pitch_offset = 0.0;
static Vec3 g_vector_base_radial;
static bool g_vector_valid = false;
static std::mutex g_vector_mutex;
static void InvalidateMouseTargets();
static bool ApplyCameraVectorNow();

/* input state */
static std::atomic<long long> g_dx{0};
static std::atomic<long long> g_dy{0};
static std::atomic<int> g_wheel_up_count{0};
static std::atomic<int> g_wheel_down_count{0};
static std::atomic<unsigned long long> g_raw_events{0};
static std::atomic<int> g_btn_down[kButtonCount];
static std::atomic<int> g_btn_up[kButtonCount];
static std::atomic<bool> g_raw_running{true};
static std::atomic<HWND> g_raw_window{nullptr};
static std::thread g_raw_thread;
static HWND g_game_window = nullptr;
static int g_button_vks[kButtonCount][4];
static int g_button_vk_count[kButtonCount];
static bool g_button_held[kButtonCount];
static int g_stick_vk[4] = {0, 0, 0, 0};
static uint8_t g_key_refs[256] = {};
static int g_calibration_key = 0;
static int g_wheel_vk[2] = {0, 0};
static int g_wheel_hold_vk = 0;
static float g_button_hold_remaining[kButtonCount] = {};
static bool g_button_release_pending[kButtonCount] = {};
static int g_active_wheel_vk = 0;
static float g_active_wheel_remaining = 0.0f;

/* switching state */
static float g_pad_cooldown = 0.0f;
static bool g_mouse_owns = false;
static bool g_cursor_hidden = false;
static bool g_cursor_clipped = false;
static int g_frames = 0;
static HCURSOR g_blank_cursor = nullptr;
static HCURSOR g_saved_class_cursor = nullptr;
static HWND g_saved_cursor_window = nullptr;
static HWND g_cursor_subclass_window = nullptr;
static WNDPROC g_original_cursor_window_proc = nullptr;
static bool g_saved_class_cursor_valid = false;
static std::atomic<bool> g_hide_cursor_requested{false};
static POINT g_last_cursor{};
static bool g_last_cursor_valid = false;
static unsigned long long g_seen_raw_events = 0;
static std::atomic<uint32_t> g_live_camera_object{0};
static std::atomic<bool> g_live_camera_is_quest{false};
static std::atomic<unsigned long long> g_last_camera_hook_tick{0};
static std::atomic<unsigned long long> g_live_camera_since_tick{0};
static std::atomic<bool> g_camera_probe_logging{false};
static void DumpCameraDti();
static void DumpObjectStringPointers(const char* label, uint32_t address, uint32_t bytes);
static void DumpObjectDtiPointers(uint32_t address, uint32_t bytes);
static bool ApplyMouseLook(long long dx, long long dy);
static std::vector<float> g_camera_snapshot;

static ForgeHook* g_camera_probe_hooks[2] = {nullptr, nullptr};
static std::atomic<int> g_camera_probe_calls{0};
static const char* g_camera_probe_names[2] = {"view-call", "camera-call"};

static ForgeHookAction CameraProbePre(ForgeCpu* cpu, void* user) {
    int index = (int)(intptr_t)user;
    g_last_camera_hook_tick.store(GetTickCount64(), std::memory_order_relaxed);
    int call = g_camera_probe_calls.fetch_add(1);
    uint32_t object = cpu->r[1];
    if (object >= 0x30000000u && object < 0x80000000u &&
        forge_api->mem_isValid(object, 0x90)) {
        uint32_t previous_object =
            g_live_camera_object.load(std::memory_order_relaxed);
        g_live_camera_object.store(object, std::memory_order_relaxed);
        uint32_t dti = forge_api->dti_of ? forge_api->dti_of(object) : 0;
        const char* class_name =
            (dti && forge_api->dti_name) ? forge_api->dti_name(dti) : nullptr;
        bool is_quest = class_name && !strcmp(class_name, "uQuestCamera");
        bool was_quest = g_live_camera_is_quest.load(std::memory_order_relaxed);
        if (is_quest && (!was_quest || object != previous_object))
            g_live_camera_since_tick.store(GetTickCount64(), std::memory_order_relaxed);
        if (!is_quest) g_live_camera_since_tick.store(0, std::memory_order_relaxed);
        g_live_camera_is_quest.store(is_quest, std::memory_order_relaxed);
        if (is_quest)
            ApplyCameraVectorNow();
    }
    if (g_camera_probe_logging.load() && call < 40) {
        LOGI("camera probe pre %s: pc 0x%08X r0 0x%08X r1 0x%08X r2 0x%08X r3 0x%08X sp 0x%08X",
             index >= 0 && index < 2 ? g_camera_probe_names[index] : "?", cpu->r[15],
             cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], cpu->r[13]);
    }
    return FORGE_HOOK_CONTINUE;
}

static void CameraProbePost(ForgeCpu* cpu, const uint32_t args[4], void* user) {
    static std::atomic<int> posts{0};
    int index = (int)(intptr_t)user;
    int call = posts.fetch_add(1);
    if (!g_camera_probe_logging.load() || call >= 40) return;
    LOGI("camera probe post %s: pc 0x%08X args %08X %08X %08X %08X ret %08X %08X",
         index >= 0 && index < 2 ? g_camera_probe_names[index] : "?", cpu->r[15], args[0],
         args[1], args[2], args[3], cpu->r[0], cpu->r[1]);
    {
        float matrix[16]{};
        void* host = forge_api->mem_ptr ? forge_api->mem_ptr(args[0], sizeof(matrix)) : nullptr;
        if (host) {
            memcpy(matrix, host, sizeof(matrix));
            LOGI("  out0 host %p for 0x%08X", host, args[0]);
            LOGI("  out0 0x%08X: %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f",
                 args[0], matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5],
                 matrix[6], matrix[7]);
            LOGI("                    %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f",
                 matrix[8], matrix[9], matrix[10], matrix[11], matrix[12], matrix[13],
                 matrix[14], matrix[15]);
        }
    }
    for (int arg = 1; arg <= 3; ++arg) {
        uint32_t address = args[arg];
        if (!forge_api->mem_isValid(address, 16)) continue;
        float values[4]{};
        if (!forge_api->mem_read(address, values, sizeof(values))) continue;
        LOGI("  arg%d 0x%08X: %.6f %.6f %.6f %.6f", arg, address, values[0], values[1],
             values[2], values[3]);
    }
}

static void InstallCameraProbeHooks(bool logging) {
    if (g_camera_probe_hooks[0]) {
        g_camera_probe_logging.store(logging);
        LOGI("camera probe logging %s", logging ? "on" : "off");
        return;
    }
    uint32_t base = forge_api->mem_getModuleBase ? forge_api->mem_getModuleBase("main") : 0;
    const uint32_t offsets[2] = {0x1C33C, 0x1C358};
    for (int i = 0; i < 2; ++i) {
        g_camera_probe_hooks[i] = forge_api->hook_create(
            base + offsets[i], CameraProbePre, CameraProbePost, (void*)(intptr_t)i);
        LOGI("camera probe hook main+0x%X: %s", offsets[i],
             g_camera_probe_hooks[i] ? "installed" : "not a recompiled block");
    }
    g_camera_probe_calls.store(0);
    g_camera_probe_logging.store(logging);
}

static void ToggleCameraProbeHooks() {
    if (g_camera_probe_hooks[0]) {
        bool enable = !g_camera_probe_logging.load();
        g_camera_probe_logging.store(enable);
        LOGI("camera probe logging %s", enable ? "on" : "off");
        return;
    }
    InstallCameraProbeHooks(true);
}

/* ---------------------------------------------------------------- key names */

struct KeyName {
    const char* name;
    int vk;
};

static const KeyName kKeyNames[] = {
    {"LBUTTON", VK_LBUTTON}, {"RBUTTON", VK_RBUTTON}, {"MBUTTON", VK_MBUTTON},
    {"X1", VK_XBUTTON1},     {"X2", VK_XBUTTON2},     {"BACK", VK_BACK},
    {"TAB", VK_TAB},         {"ENTER", VK_RETURN},    {"RETURN", VK_RETURN},
    {"SHIFT", VK_SHIFT},     {"LSHIFT", VK_LSHIFT},   {"RSHIFT", VK_RSHIFT},
    {"CTRL", VK_CONTROL},    {"LCTRL", VK_LCONTROL},  {"RCTRL", VK_RCONTROL},
    {"ALT", VK_MENU},        {"LALT", VK_LMENU},      {"RALT", VK_RMENU},
    {"SPACE", VK_SPACE},     {"ESC", VK_ESCAPE},      {"ESCAPE", VK_ESCAPE},
    {"LEFT", VK_LEFT},       {"RIGHT", VK_RIGHT},     {"UP", VK_UP},
    {"DOWN", VK_DOWN},       {"INS", VK_INSERT},      {"INSERT", VK_INSERT},
    {"DEL", VK_DELETE},      {"DELETE", VK_DELETE},   {"HOME", VK_HOME},
    {"END", VK_END},         {"PGUP", VK_PRIOR},      {"PGDN", VK_NEXT},
    {"F1", VK_F1},           {"F2", VK_F2},           {"F3", VK_F3},
    {"F4", VK_F4},           {"F5", VK_F5},           {"F6", VK_F6},
    {"F7", VK_F7},           {"F8", VK_F8},           {"F9", VK_F9},
    {"F10", VK_F10},         {"F11", VK_F11},         {"F12", VK_F12},
};

static void UpperString(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

static int VkFromName(const char* raw) {
    if (!raw) return 0;
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (!*raw) return 0;
    char name[kKeyTokenMax];
    UpperString(name, raw, sizeof(name));
    if (!strcmp(name, "NONE") || !strcmp(name, "-") || !strcmp(name, "0")) return 0;
    for (const KeyName& key : kKeyNames)
        if (!strcmp(name, key.name)) return key.vk;
    if (name[0] >= 'A' && name[0] <= 'Z' && !name[1]) return name[0];
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) return name[0];
    if (name[0] == 'F' && name[1]) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return 0;
}

static int ParseVkList(const char* csv, int* out, int max_count) {
    if (!csv) return 0;
    int count = 0;
    const char* p = csv;
    while (*p && count < max_count) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char token[kKeyTokenMax];
        if (len >= sizeof(token)) len = sizeof(token) - 1;
        memcpy(token, p, len);
        token[len] = 0;
        int vk = VkFromName(token);
        if (vk) out[count++] = vk;
        p = comma ? comma + 1 : p + strlen(p);
    }
    return count;
}

static void GetConfigString(const char* section, const char* key, char* out, size_t cap,
                            const char* fallback) {
    if (!g_conf || !forge_api->config_string) {
        snprintf(out, cap, "%s", fallback ? fallback : "");
        return;
    }
    const char* value = forge_api->config_string(g_conf, section, key, fallback);
    snprintf(out, cap, "%s", value ? value : (fallback ? fallback : ""));
}

/* ---------------------------------------------------------------- process hash */

static bool ComputeExecutableSha256(char out[65]) {
    wchar_t path[MAX_PATH * 4];
    DWORD path_len = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (!path_len || path_len >= sizeof(path) / sizeof(path[0])) return false;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            std::vector<BYTE> buffer(1024 * 1024);
            DWORD read = 0;
            ok = true;
            while (ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) &&
                   read != 0) {
                if (!CryptHashData(hash, buffer.data(), read, 0)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                BYTE digest[32];
                DWORD digest_size = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0) &&
                    digest_size == sizeof(digest)) {
                    static const char* hex = "0123456789ABCDEF";
                    for (DWORD i = 0; i < digest_size; ++i) {
                        out[i * 2] = hex[digest[i] >> 4];
                        out[i * 2 + 1] = hex[digest[i] & 15];
                    }
                    out[64] = 0;
                } else {
                    ok = false;
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(provider, 0);
    }
    CloseHandle(file);
    return ok;
}

/* ---------------------------------------------------------------- memory values */

static const char* TypeName(int type) {
    switch (type) {
        case SLOT_F32: return "f32";
        case SLOT_I32: return "i32";
        case SLOT_I16_LO: return "i16.lo";
        case SLOT_I16_HI: return "i16.hi";
        default: return "?";
    }
}

static uint32_t SlotSize(int type) {
    return (type == SLOT_I16_LO || type == SLOT_I16_HI) ? 2u : 4u;
}

static bool ReadValue(uint32_t addr, int type, double* out) {
    if (!addr || !forge_api || !forge_api->mem_read || !out) return false;
    if (!forge_api->mem_isValid(addr, SlotSize(type))) return false;
    if (type == SLOT_I16_LO || type == SLOT_I16_HI) {
        uint16_t raw = 0;
        if (!forge_api->mem_read(addr, &raw, 2)) return false;
        *out = (double)(int16_t)raw;
        return true;
    }
    uint32_t raw = 0;
    if (!forge_api->mem_read(addr, &raw, 4)) return false;
    if (type == SLOT_I32) {
        *out = (double)(int32_t)raw;
        return true;
    }
    float value = 0.0f;
    memcpy(&value, &raw, sizeof(value));
    if (!std::isfinite(value)) return false;
    *out = (double)value;
    return true;
}

static bool WriteValue(uint32_t addr, int type, double value) {
    if (!addr || !forge_api || !forge_api->mem_write) return false;
    if (!forge_api->mem_isValid(addr, SlotSize(type))) return false;
    if (type == SLOT_I16_LO || type == SLOT_I16_HI) {
        int16_t raw = (int16_t)llround(value);
        return forge_api->mem_write(addr, &raw, 2);
    }
    if (type == SLOT_I32) {
        int32_t raw = (int32_t)llround(value);
        return forge_api->mem_write(addr, &raw, 4);
    }
    float raw = (float)value;
    return forge_api->mem_write(addr, &raw, 4);
}

static double WrapDelta(double current, double previous, double units) {
    double delta = current - previous;
    if (units > 0.0) {
        double half = units * 0.5;
        if (delta > half) delta -= units;
        if (delta < -half) delta += units;
    }
    return delta;
}

static double WrapValue(double value, double units) {
    if (units <= 0.0) return value;
    double half = units * 0.5;
    while (value > half) value -= units;
    while (value < -half) value += units;
    return value;
}

/* ---------------------------------------------------------------- physical keys */

static bool IsExtendedVirtualKey(int vk) {
    return vk == VK_RIGHT || vk == VK_LEFT || vk == VK_UP || vk == VK_DOWN ||
           vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
           vk == VK_INSERT || vk == VK_DELETE || vk == VK_RCONTROL || vk == VK_RMENU ||
           vk == VK_DIVIDE;
}

static void SendPhysicalKey(int vk, bool down) {
    if (!vk) return;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) |
                       (IsExtendedVirtualKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &input, sizeof(input));
}

static void RefKey(int vk, bool down) {
    if (vk <= 0 || vk > 255) return;
    if (down) {
        if (g_key_refs[vk]++ == 0) SendPhysicalKey(vk, true);
    } else if (g_key_refs[vk]) {
        if (--g_key_refs[vk] == 0) SendPhysicalKey(vk, false);
    }
}

static void PressList(const int* vks, int count, bool down) {
    if (down) {
        for (int i = 0; i < count; ++i) RefKey(vks[i], true);
    } else {
        for (int i = count - 1; i >= 0; --i) RefKey(vks[i], false);
    }
}

static void ReleaseAllMappedKeys() {
    for (int vk = 1; vk < 256; ++vk) {
        while (g_key_refs[vk]) RefKey(vk, false);
    }
    for (int i = 0; i < kButtonCount; ++i) {
        g_button_held[i] = false;
        g_button_release_pending[i] = false;
        g_button_hold_remaining[i] = 0.0f;
        g_btn_down[i].store(0);
        g_btn_up[i].store(0);
    }
    g_active_wheel_vk = 0;
    g_active_wheel_remaining = 0.0f;
    g_wheel_up_count.store(0);
    g_wheel_down_count.store(0);
}

static void HoldCalibrationKey(int vk) {
    if (g_calibration_key == vk) return;
    if (g_calibration_key) SendPhysicalKey(g_calibration_key, false);
    g_calibration_key = vk;
    if (vk) SendPhysicalKey(vk, true);
}

/* ---------------------------------------------------------------- game window */

static HWND FindGameWindow() {
    struct Context {
        DWORD pid;
        HWND best;
    } context{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            Context* ctx = (Context*)parameter;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid != ctx->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
                return TRUE;
            ctx->best = window;
            return FALSE;
        },
        (LPARAM)&context);
    return context.best;
}

static bool GameIsFocused() {
    if (!g_game_window || !IsWindow(g_game_window)) g_game_window = FindGameWindow();
    if (!g_game_window) return false;
    HWND foreground = GetForegroundWindow();
    return foreground == g_game_window || IsChild(g_game_window, foreground) ||
           GetAncestor(foreground, GA_ROOT) == g_game_window;
}

static HCURSOR CreateBlankCursor() {
    BYTE and_mask[32 * 4];
    BYTE xor_mask[32 * 4];
    memset(and_mask, 0xFF, sizeof(and_mask));
    memset(xor_mask, 0x00, sizeof(xor_mask));
    return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask, xor_mask);
}

static LRESULT CALLBACK CursorWindowProc(HWND window, UINT message, WPARAM wparam,
                                         LPARAM lparam) {
    if (message == WM_SETCURSOR && g_hide_cursor_requested.load(std::memory_order_relaxed)) {
        SetCursor(g_blank_cursor);
        return TRUE;
    }
    if (g_original_cursor_window_proc)
        return CallWindowProcW(g_original_cursor_window_proc, window, message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

static void RemoveCursorSubclass() {
    if (g_cursor_subclass_window && g_original_cursor_window_proc &&
        IsWindow(g_cursor_subclass_window)) {
        SetWindowLongPtrW(g_cursor_subclass_window, GWLP_WNDPROC,
                          (LONG_PTR)g_original_cursor_window_proc);
    }
    g_cursor_subclass_window = nullptr;
    g_original_cursor_window_proc = nullptr;
}

static void EnsureCursorSubclass(HWND window) {
    if (!window || !IsWindow(window) || window == g_cursor_subclass_window) return;
    RemoveCursorSubclass();
    SetLastError(0);
    WNDPROC previous =
        (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)CursorWindowProc);
    if (!previous && GetLastError()) return;
    g_cursor_subclass_window = window;
    g_original_cursor_window_proc = previous;
}

static void SetCursorHidden(bool hidden) {
    if (g_cursor_hidden == hidden) return;
    g_cursor_hidden = hidden;
    if (hidden) {
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

static void UpdateCursorHidden(bool hidden) {
    if (hidden && !g_blank_cursor) g_blank_cursor = CreateBlankCursor();
    g_hide_cursor_requested.store(hidden, std::memory_order_relaxed);
    if (g_game_window && IsWindow(g_game_window) &&
        g_saved_cursor_window != g_game_window && g_saved_class_cursor_valid) {
        if (g_saved_cursor_window && IsWindow(g_saved_cursor_window)) {
            SetClassLongPtrW(g_saved_cursor_window, GCLP_HCURSOR,
                             (LONG_PTR)g_saved_class_cursor);
        }
        g_saved_class_cursor_valid = false;
        g_saved_cursor_window = nullptr;
    }
    if (hidden && g_game_window && IsWindow(g_game_window) && g_blank_cursor) {
        if (!g_saved_class_cursor_valid || g_saved_cursor_window != g_game_window) {
            g_saved_class_cursor =
                (HCURSOR)GetClassLongPtrW(g_game_window, GCLP_HCURSOR);
            g_saved_cursor_window = g_game_window;
            g_saved_class_cursor_valid = true;
        }
        SetClassLongPtrW(g_game_window, GCLP_HCURSOR, (LONG_PTR)g_blank_cursor);
        EnsureCursorSubclass(g_game_window);
    } else if (!hidden && g_saved_class_cursor_valid) {
        if (g_saved_cursor_window && IsWindow(g_saved_cursor_window)) {
            SetClassLongPtrW(g_saved_cursor_window, GCLP_HCURSOR,
                             (LONG_PTR)g_saved_class_cursor);
        }
        g_saved_class_cursor_valid = false;
        g_saved_cursor_window = nullptr;
    }
    if (!hidden) RemoveCursorSubclass();
    bool changed = g_cursor_hidden != hidden;
    SetCursorHidden(hidden);
    if (hidden) {
        SetCursor(g_blank_cursor);
    } else if (changed) {
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
    }
}

static void SetCursorClipped(bool clipped) {
    if (g_cursor_clipped == clipped) return;
    g_cursor_clipped = clipped;
    if (clipped && g_game_window) {
        RECT rect{};
        if (GetClientRect(g_game_window, &rect)) {
            POINT top_left{rect.left, rect.top};
            POINT bottom_right{rect.right, rect.bottom};
            ClientToScreen(g_game_window, &top_left);
            ClientToScreen(g_game_window, &bottom_right);
            RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
            ClipCursor(&screen);
            return;
        }
    }
    ClipCursor(nullptr);
}

/* ---------------------------------------------------------------- xinput / pad */

struct XInputStateX {
    DWORD packet;
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT thumb_lx;
    SHORT thumb_ly;
    SHORT thumb_rx;
    SHORT thumb_ry;
};

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XInputStateX*);
static XInputGetStateFn g_xinput_get_state = nullptr;
static bool g_xinput_loaded = false;

static void LoadXInput() {
    if (g_xinput_loaded) return;
    g_xinput_loaded = true;
    const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* name : names) {
        HMODULE module = LoadLibraryW(name);
        if (!module) continue;
        auto fn = (XInputGetStateFn)GetProcAddress(module, "XInputGetState");
        if (fn) {
            g_xinput_get_state = fn;
            return;
        }
        FreeLibrary(module);
    }
}

static float StickDeadzone(float value) {
    constexpr float deadzone = 0.20f;
    if (value > deadzone) return (value - deadzone) / (1.0f - deadzone);
    if (value < -deadzone) return (value + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

static void SampleRightStick(float* out_x, float* out_y) {
    *out_x = 0.0f;
    *out_y = 0.0f;
    LoadXInput();
    if (g_xinput_get_state) {
        for (DWORD index = 0; index < 4; ++index) {
            XInputStateX state{};
            if (g_xinput_get_state(index, &state) != ERROR_SUCCESS) continue;
            float x = StickDeadzone((float)state.thumb_rx / 32767.0f);
            float y = StickDeadzone((float)state.thumb_ry / 32767.0f);
            if (fabsf(x) > fabsf(*out_x)) *out_x = x;
            if (fabsf(y) > fabsf(*out_y)) *out_y = y;
        }
    }
    if (g_stick_vk[0] && (GetAsyncKeyState(g_stick_vk[0]) & 0x8000)) *out_y = 1.0f;
    if (g_stick_vk[1] && (GetAsyncKeyState(g_stick_vk[1]) & 0x8000)) *out_x = -1.0f;
    if (g_stick_vk[2] && (GetAsyncKeyState(g_stick_vk[2]) & 0x8000)) *out_y = -1.0f;
    if (g_stick_vk[3] && (GetAsyncKeyState(g_stick_vk[3]) & 0x8000)) *out_x = 1.0f;
}

/* ---------------------------------------------------------------- raw input */

static LRESULT CALLBACK RawWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        g_raw_events.fetch_add(1);
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lparam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size > 0 && size < 256 * 1024) {
            std::vector<BYTE> buffer(size);
            if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buffer.data(), &size,
                                sizeof(RAWINPUTHEADER)) == size) {
                RAWINPUT* raw = (RAWINPUT*)buffer.data();
                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    const RAWMOUSE& mouse = raw->data.mouse;
                    if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                        g_dx.fetch_add(mouse.lLastX);
                        g_dy.fetch_add(mouse.lLastY);
                    }
                    struct ButtonEvent {
                        USHORT down;
                        USHORT up;
                        int index;
                    };
                    static const ButtonEvent events[] = {
                        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0},
                        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 1},
                        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2},
                        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3},
                        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4},
                    };
                    for (const ButtonEvent& event : events) {
                        if (mouse.usButtonFlags & event.down) g_btn_down[event.index]++;
                        if (mouse.usButtonFlags & event.up) g_btn_up[event.index]++;
                    }
                    if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                        SHORT delta = (SHORT)mouse.usButtonData;
                        if (delta > 0)
                            g_wheel_up_count.fetch_add(1);
                        else
                            g_wheel_down_count.fetch_add(1);
                    }
                }
            }
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static POINT GameClientCenter() {
    POINT center{0, 0};
    if (!g_game_window || !IsWindow(g_game_window)) return center;
    RECT rect{};
    if (!GetClientRect(g_game_window, &rect)) return center;
    center.x = (rect.right - rect.left) / 2;
    center.y = (rect.bottom - rect.top) / 2;
    ClientToScreen(g_game_window, &center);
    return center;
}

static void SampleMouseMotion(bool cursor_allowed, bool center_cursor, long long* out_x,
                              long long* out_y) {
    *out_x = 0;
    *out_y = 0;
    unsigned long long events = g_raw_events.load(std::memory_order_relaxed);
    bool raw_fresh = events != g_seen_raw_events;
    g_seen_raw_events = events;
    if (raw_fresh) {
        *out_x = g_dx.exchange(0);
        *out_y = g_dy.exchange(0);
    } else {
        g_dx.store(0);
        g_dy.store(0);
    }

    POINT cursor{};
    bool have_cursor = GetCursorPos(&cursor) != 0;
    if (!raw_fresh && g_cfg.cursor_fallback && cursor_allowed && have_cursor &&
        g_last_cursor_valid) {
        *out_x += cursor.x - g_last_cursor.x;
        *out_y += cursor.y - g_last_cursor.y;
    }

    if (cursor_allowed && center_cursor && g_game_window) {
        POINT center = GameClientCenter();
        if (center.x || center.y) {
            SetCursorPos(center.x, center.y);
            cursor = center;
            have_cursor = true;
        }
    }
    g_last_cursor_valid = have_cursor;
    if (have_cursor) g_last_cursor = cursor;
}

static void RawInputThread() {
    const wchar_t* class_name = L"MouseLookRawInput";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = RawWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    RegisterClassW(&window_class);

    HWND window = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  window_class.hInstance, nullptr);
    if (!window) {
        LOGE("raw input window creation failed: %lu", GetLastError());
        return;
    }
    g_raw_window.store(window);

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        LOGE("RegisterRawInputDevices failed: %lu", GetLastError());
        PostMessageW(window, WM_QUIT, 0, 0);
    }

    MSG message;
    while (g_raw_running.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(window);
    g_raw_window.store(nullptr);
    UnregisterClassW(class_name, window_class.hInstance);
}

/* ---------------------------------------------------------------- button routing */

static void HandleMouseButtons(bool allowed, float dt) {
    if (!allowed) {
        ReleaseAllMappedKeys();
        return;
    }
    for (int index = 0; index < kButtonCount; ++index) {
        int downs = g_btn_down[index].exchange(0);
        int ups = g_btn_up[index].exchange(0);
        if (downs > 0) {
            if (!g_button_held[index])
                PressList(g_button_vks[index], g_button_vk_count[index], true);
            g_button_held[index] = true;
            g_button_release_pending[index] = false;
            g_button_hold_remaining[index] =
                std::max(1, g_cfg.button_hold_ms) / 1000.0f;
        }
        if (ups > 0 && g_button_held[index])
            g_button_release_pending[index] = true;
        if (g_button_held[index] && g_button_hold_remaining[index] > 0.0f)
            g_button_hold_remaining[index] -= dt;
        if (g_button_held[index] && g_button_release_pending[index] &&
            g_button_hold_remaining[index] <= 0.0f) {
            PressList(g_button_vks[index], g_button_vk_count[index], false);
            g_button_held[index] = false;
            g_button_release_pending[index] = false;
        }
    }
}

static void HandleMouseWheel(bool allowed, float dt) {
    if (!allowed) {
        if (g_active_wheel_vk) RefKey(g_active_wheel_vk, false);
        g_active_wheel_vk = 0;
        g_active_wheel_remaining = 0.0f;
        g_wheel_up_count.store(0);
        g_wheel_down_count.store(0);
        return;
    }

    if (g_active_wheel_remaining > 0.0f) {
        g_active_wheel_remaining -= dt;
        if (g_active_wheel_remaining <= 0.0f) {
            if (g_active_wheel_vk) RefKey(g_active_wheel_vk, false);
            g_active_wheel_vk = 0;
        }
    }
    if (g_active_wheel_vk) return;
    if (g_wheel_hold_vk && !(GetAsyncKeyState(g_wheel_hold_vk) & 0x8000)) {
        g_wheel_up_count.store(0);
        g_wheel_down_count.store(0);
        return;
    }

    int up = g_wheel_up_count.load();
    int down = g_wheel_down_count.load();
    int key = 0;
    if (up > 0 && down <= 0) {
        g_wheel_up_count.fetch_sub(1);
        key = g_wheel_vk[0];
    } else if (down > 0) {
        g_wheel_down_count.fetch_sub(1);
        key = g_wheel_vk[1];
    }
    if (key) {
        RefKey(key, true);
        g_active_wheel_vk = key;
        g_active_wheel_remaining = std::max(1, g_cfg.wheel_hold_ms) / 1000.0f;
    }
}

/* ---------------------------------------------------------------- camera path */

static uint32_t CameraRoot() {
    if (!forge_api || !forge_api->singleton_getByName || !g_cfg.camera_root[0]) return 0;
    return forge_api->singleton_getByName(g_cfg.camera_root);
}

static uint32_t ResolveCameraSlot(const CameraSlot& slot) {
    uint32_t address = CameraRoot();
    if (!address) return 0;
    for (uint32_t hop : slot.hops) {
        uint32_t pointer = 0;
        if (!forge_api->mem_read(address + hop, &pointer, sizeof(pointer)) || !pointer)
            return 0;
        address = pointer;
    }
    return address + slot.field;
}

static bool ParseCameraPath(const char* text, CameraSlot* slot) {
    if (!text || !*text || !slot) return false;
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", text);
    char* direction_colon = strrchr(buffer, ':');
    if (!direction_colon) return false;
    *direction_colon = 0;
    char* type_colon = strrchr(buffer, ':');
    if (!type_colon) return false;
    *type_colon = 0;
    int type = (int)strtol(type_colon + 1, nullptr, 0);
    int dir = (int)strtol(direction_colon + 1, nullptr, 0);
    if (type < SLOT_F32 || type > SLOT_I16_HI || (dir != 1 && dir != -1)) return false;

    slot->hops.clear();
    slot->type = type;
    slot->dir = dir;
    char* cursor = buffer;
    while (*cursor) {
        char* slash = strchr(cursor, '/');
        if (slash) *slash = 0;
        uint32_t value = (uint32_t)strtoul(cursor, nullptr, 0);
        slot->hops.push_back(value);
        if (!slash) break;
        cursor = slash + 1;
    }
    if (slot->hops.empty()) return false;
    slot->field = slot->hops.back();
    slot->hops.pop_back();
    return true;
}

static void FormatCameraPath(const CameraSlot& slot, char* out, size_t cap) {
    size_t used = 0;
    if (cap) out[0] = 0;
    for (uint32_t hop : slot.hops) {
        used += (size_t)snprintf(out + used, cap - used, "0x%X/", hop);
        if (used >= cap) return;
    }
    snprintf(out + used, cap - used, "0x%X:%d:%d", slot.field, slot.type, slot.dir);
}

static int ParseCameraSlotList(const char* text, std::vector<CameraSlot>* output) {
    output->clear();
    if (!text || !*text) return 0;
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "%s", text);
    char* cursor = buffer;
    while (*cursor && (int)output->size() < kMaxCamSlots) {
        char* semicolon = strchr(cursor, ';');
        if (semicolon) *semicolon = 0;
        CameraSlot slot;
        if (ParseCameraPath(cursor, &slot)) output->push_back(slot);
        if (!semicolon) break;
        cursor = semicolon + 1;
    }
    return (int)output->size();
}

static void FormatCameraSlotList(const std::vector<CameraSlot>& slots, char* out, size_t cap) {
    if (cap) out[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < slots.size() && used < cap; ++i) {
        char one[512];
        FormatCameraPath(slots[i], one, sizeof(one));
        used += (size_t)snprintf(out + used, cap - used, "%s%s", i ? ";" : "", one);
    }
}

static bool ValidateCameraSlots(const std::vector<CameraSlot>& slots) {
    if (slots.empty() || !CameraRoot()) return false;
    for (const CameraSlot& slot : slots) {
        uint32_t address = ResolveCameraSlot(slot);
        double value = 0.0;
        if (address && ReadValue(address, slot.type, &value)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- camera scan */

struct MemoryBlock {
    std::vector<uint32_t> hops;
    uint32_t address = 0;
    uint32_t size = 0;
    char class_name[64] = {};
    bool is_camera_class = false;
};

struct RawSlot {
    uint16_t block = 0;
    uint16_t offset = 0;
    uint8_t type = SLOT_F32;
};

struct ScoredSlot {
    double score = 0.0;
    double horizontal = 0.0;
    double vertical = 0.0;
    int slot_index = 0;
};

enum class CalibrationState {
    Off,
    IdleWait,
    RightHold,
    RightSettle,
    LeftHold,
    LeftSettle,
    UpHold,
    UpSettle,
    DownHold,
    DownSettle,
    SpinYaw,
    PitchUp,
    PitchDown,
    Finish,
    Done,
    Failed,
};

static CalibrationState g_cal_state = CalibrationState::Off;
static float g_cal_timer = 0.0f;
static std::vector<MemoryBlock> g_blocks;
static std::vector<RawSlot> g_raw_slots;
static std::vector<std::vector<double>> g_samples;
static std::vector<std::vector<uint8_t>> g_sample_valid;
static std::vector<ScoredSlot> g_scored_yaw;
static std::vector<ScoredSlot> g_scored_pitch;
static std::vector<CameraSlot> g_calibrated_yaw;
static std::vector<CameraSlot> g_calibrated_pitch;
static double g_tracked_pitch_min = 0.0;
static double g_tracked_pitch_max = 0.0;
static bool g_pitch_tracking = false;

static bool BuildCameraBlocks(uint32_t root) {
    g_blocks.clear();
    g_raw_slots.clear();
    if (!root || !forge_api->mem_isValid(root, 4)) return false;

    int max_objects = std::max(4, std::min(g_cfg.probe_chunks, 512));
    uint32_t block_size =
        (uint32_t)std::max(256, std::min(g_cfg.probe_bytes, 32 * 1024)) & ~3u;

    std::unordered_set<uint32_t> seen;
    seen.insert(root);
    MemoryBlock root_block;
    root_block.address = root;
    root_block.size = block_size;
    snprintf(root_block.class_name, sizeof(root_block.class_name), "%s", g_cfg.camera_root);
    root_block.is_camera_class = false;
    g_blocks.push_back(std::move(root_block));

    std::vector<uint32_t> root_words(block_size / 4);
    if (forge_api->mem_read(root, root_words.data(), block_size)) {
        for (uint32_t word_index = 0;
             word_index < root_words.size() && (int)g_blocks.size() < max_objects;
             ++word_index) {
            uint32_t pointer = root_words[word_index];
            if (pointer < 0x30000000u || pointer >= 0x80000000u ||
                seen.count(pointer) || !forge_api->mem_isValid(pointer, 4))
                continue;
            uint32_t dti = forge_api->dti_of ? forge_api->dti_of(pointer) : 0;
            const char* class_name =
                (dti && forge_api->dti_name) ? forge_api->dti_name(dti) : nullptr;
            if (!class_name || !strstr(class_name, "Camera")) continue;
            if (!strcmp(class_name, g_cfg.camera_root)) continue;
            seen.insert(pointer);
            MemoryBlock child;
            child.hops.push_back(word_index * 4);
            child.address = pointer;
            child.size = block_size;
            snprintf(child.class_name, sizeof(child.class_name), "%s", class_name);
            child.is_camera_class = true;
            g_blocks.push_back(std::move(child));
        }
    }

    for (size_t block_index = 0; block_index < g_blocks.size(); ++block_index) {
        const MemoryBlock& block = g_blocks[block_index];
        for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
            g_raw_slots.push_back({(uint16_t)block_index, (uint16_t)offset, SLOT_F32});
            g_raw_slots.push_back({(uint16_t)block_index, (uint16_t)offset, SLOT_I32});
            g_raw_slots.push_back({(uint16_t)block_index, (uint16_t)offset, SLOT_I16_LO});
            g_raw_slots.push_back(
                {(uint16_t)block_index, (uint16_t)(offset + 2), SLOT_I16_HI});
        }
    }
    LOGI("camera graph: root 0x%08X, %zu block(s), %zu scalar(s), %d bytes/block",
         root, g_blocks.size(), g_raw_slots.size(), (int)block_size);
    for (const MemoryBlock& block : g_blocks)
        LOGI("  block 0x%08X class %s%s", block.address, block.class_name,
             block.is_camera_class ? " (active camera candidate)" : "");
    return !g_raw_slots.empty();
}

static bool TakeSample(int sample_index) {
    if (sample_index < 0 || sample_index >= (int)g_samples.size()) return false;
    std::vector<std::vector<BYTE>> data(g_blocks.size());
    for (size_t i = 0; i < g_blocks.size(); ++i) {
        data[i].resize(g_blocks[i].size);
        if (!forge_api->mem_read(g_blocks[i].address, data[i].data(), g_blocks[i].size))
            data[i].clear();
    }
    std::fill(g_sample_valid[sample_index].begin(), g_sample_valid[sample_index].end(), 0);
    for (size_t i = 0; i < g_raw_slots.size(); ++i) {
        const RawSlot& slot = g_raw_slots[i];
        const std::vector<BYTE>& block_data = data[slot.block];
        if (block_data.empty() || (size_t)slot.offset + 4 > block_data.size()) continue;
        const BYTE* p = block_data.data() + slot.offset;
        double value = 0.0;
        if (slot.type == SLOT_F32) {
            float f = 0.0f;
            memcpy(&f, p, 4);
            if (!std::isfinite(f)) continue;
            if (fabs(f) > 100000.0f) continue;
            value = f;
        } else if (slot.type == SLOT_I32) {
            int32_t v = 0;
            memcpy(&v, p, 4);
            if (llabs((long long)v) > 100000000) continue;
            value = v;
        } else {
            int16_t v = 0;
            memcpy(&v, p, 2);
            value = v;
        }
        g_samples[sample_index][i] = value;
        g_sample_valid[sample_index][i] = 1;
    }
    return true;
}

static double UnitsGuess(int type, double reference) {
    if (type == SLOT_I16_LO || type == SLOT_I16_HI) return 65536.0;
    if (type == SLOT_I32)
        return fabs(reference) <= 200000.0 ? 65536.0 : 360.0;
    return fabs(reference) <= 8.0 ? 6.283185307179586 : 360.0;
}

static double ToDegrees(double delta, int type, double reference) {
    return delta * 360.0 / UnitsGuess(type, reference);
}

static bool BuildScoredCandidates() {
    g_scored_yaw.clear();
    g_scored_pitch.clear();
    if (g_samples.size() != 6 || g_raw_slots.empty()) return false;

    for (size_t i = 0; i < g_raw_slots.size(); ++i) {
        bool valid = true;
        for (int sample = 0; sample < 6; ++sample)
            if (!g_sample_valid[sample][i]) valid = false;
        if (!valid) continue;

        const RawSlot& raw = g_raw_slots[i];
        double idle0 = g_samples[0][i];
        double idle1 = g_samples[1][i];
        double right = g_samples[2][i];
        double left = g_samples[3][i];
        double up = g_samples[4][i];
        double down = g_samples[5][i];
        double units = UnitsGuess(raw.type, idle1);
        if (!std::isfinite(units) || units <= 0.0) continue;
        double idle_delta = WrapDelta(idle1, idle0, units);
        double idle_tolerance = raw.type == SLOT_F32 ? 1e-5 : 1.0;
        if (fabs(idle_delta) > idle_tolerance) continue;

        double h1 = WrapDelta(right, idle1, units);
        double h2 = WrapDelta(left, right, units);
        double v1 = WrapDelta(up, left, units);
        double v2 = WrapDelta(down, up, units);
        double h1d = fabs(ToDegrees(h1, raw.type, idle1));
        double h2d = fabs(ToDegrees(h2, raw.type, idle1));
        double v1d = fabs(ToDegrees(v1, raw.type, idle1));
        double v2d = fabs(ToDegrees(v2, raw.type, idle1));
        if (!std::isfinite(h1d) || !std::isfinite(h2d) || !std::isfinite(v1d) ||
            !std::isfinite(v2d))
            continue;
        double horizontal = std::min(h1d, h2d);
        double vertical = std::min(v1d, v2d);
        bool opposite_h = h1 * h2 < 0.0;
        bool opposite_v = v1 * v2 < 0.0;
        double hmax = std::max(h1d, h2d);
        double vmax = std::max(v1d, v2d);

        if (opposite_h && horizontal >= kMinCalibrationAxisDegrees &&
            horizontal < 1500.0 && hmax > 0.0 &&
            horizontal / hmax >= kMinCalibrationSymmetry &&
            horizontal > 4.0 * (v1d + v2d)) {
            double score = horizontal - 2.0 * (v1d + v2d);
            g_scored_yaw.push_back({score, horizontal, v1d + v2d, (int)i});
        }
        if (opposite_v && vertical >= kMinCalibrationAxisDegrees &&
            vertical < 1500.0 && vmax > 0.0 &&
            vertical / vmax >= kMinCalibrationSymmetry &&
            vertical > 4.0 * (h1d + h2d)) {
            double score = vertical - 2.0 * (h1d + h2d);
            g_scored_pitch.push_back({score, h1d + h2d, vertical, (int)i});
        }
    }

    auto by_score = [](const ScoredSlot& a, const ScoredSlot& b) { return a.score > b.score; };
    std::sort(g_scored_yaw.begin(), g_scored_yaw.end(), by_score);
    std::sort(g_scored_pitch.begin(), g_scored_pitch.end(), by_score);
    LOGI("calibration candidates: %zu yaw, %zu pitch", g_scored_yaw.size(),
         g_scored_pitch.size());
    return !g_scored_yaw.empty() && !g_scored_pitch.empty();
}

static CameraSlot MakeCameraSlot(int raw_slot_index, int direction) {
    const RawSlot& raw = g_raw_slots[raw_slot_index];
    const MemoryBlock& block = g_blocks[raw.block];
    CameraSlot slot;
    slot.hops = block.hops;
    slot.field = raw.offset;
    slot.type = raw.type;
    slot.dir = direction;
    return slot;
}

static bool SelectCalibrationSlots() {
    g_calibrated_yaw.clear();
    g_calibrated_pitch.clear();
    if (g_scored_yaw.empty() || g_scored_pitch.empty()) return false;

    const ScoredSlot* best_yaw_candidate = &g_scored_yaw.front();
    bool have_camera_yaw = false;
    for (const ScoredSlot& candidate : g_scored_yaw) {
        const RawSlot& raw = g_raw_slots[candidate.slot_index];
        if (g_blocks[raw.block].is_camera_class) {
            best_yaw_candidate = &candidate;
            have_camera_yaw = true;
            break;
        }
    }
    int yaw_index = best_yaw_candidate->slot_index;
    int yaw_type = g_raw_slots[yaw_index].type;
    const ScoredSlot* best_pitch_candidate = nullptr;
    bool have_camera_pitch = false;
    for (const ScoredSlot& candidate : g_scored_pitch) {
        const RawSlot& raw = g_raw_slots[candidate.slot_index];
        if (raw.type == yaw_type && g_blocks[raw.block].is_camera_class) {
            best_pitch_candidate = &candidate;
            have_camera_pitch = true;
            break;
        }
    }
    if (!best_pitch_candidate) {
        for (const ScoredSlot& candidate : g_scored_pitch) {
            if (g_raw_slots[candidate.slot_index].type == yaw_type) {
                best_pitch_candidate = &candidate;
                break;
            }
        }
    }
    if (!best_pitch_candidate) {
        LOGE("calibration rejected: no pitch candidate has the yaw field's scalar type");
        return false;
    }

    double best_yaw = std::max(best_yaw_candidate->score, 0.1);
    double best_pitch = std::max(best_pitch_candidate->score, 0.1);
    double yaw_h = best_yaw_candidate->horizontal;
    double pitch_v = best_pitch_candidate->vertical;
    int pitch_index = best_pitch_candidate->slot_index;

    double yaw_reference = g_samples[1][yaw_index];
    int yaw_dir = g_samples[2][yaw_index] >= g_samples[1][yaw_index] ? 1 : -1;
    int pitch_dir = g_samples[5][pitch_index] >= g_samples[4][pitch_index] ? 1 : -1;

    for (const ScoredSlot& candidate : g_scored_yaw) {
        if ((int)g_calibrated_yaw.size() >= kMaxCamSlots) break;
        if (candidate.score < best_yaw * 0.90 ||
            g_raw_slots[candidate.slot_index].type != yaw_type)
            continue;
        if (have_camera_yaw &&
            !g_blocks[g_raw_slots[candidate.slot_index].block].is_camera_class)
            continue;
        double ratio = candidate.horizontal / std::max(yaw_h, 0.0001);
        if (ratio < 0.90 || ratio > 1.10) continue;
        int slot_dir = g_samples[2][candidate.slot_index] >= g_samples[1][candidate.slot_index]
                           ? 1
                           : -1;
        if (slot_dir != yaw_dir) continue;
        g_calibrated_yaw.push_back(MakeCameraSlot(candidate.slot_index, yaw_dir));
    }

    for (const ScoredSlot& candidate : g_scored_pitch) {
        if ((int)g_calibrated_pitch.size() >= kMaxCamSlots) break;
        const RawSlot& raw = g_raw_slots[candidate.slot_index];
        if (candidate.score < best_pitch * 0.90 || raw.type != yaw_type)
            continue;
        if (have_camera_pitch && !g_blocks[raw.block].is_camera_class) continue;
        double ratio = candidate.vertical / std::max(pitch_v, 0.0001);
        if (ratio < 0.90 || ratio > 1.10) continue;
        int slot_dir = g_samples[5][candidate.slot_index] >= g_samples[4][candidate.slot_index]
                           ? 1
                           : -1;
        if (slot_dir != pitch_dir) continue;
        g_calibrated_pitch.push_back(MakeCameraSlot(candidate.slot_index, pitch_dir));
    }

    if (g_calibrated_yaw.empty())
        g_calibrated_yaw.push_back(MakeCameraSlot(yaw_index, yaw_dir));
    if (g_calibrated_pitch.empty()) {
        LOGE("calibration rejected: no pitch field has the yaw field's scalar type");
        g_calibrated_yaw.clear();
        return false;
    }

    int type = g_calibrated_yaw.front().type;
    g_units_per_turn = (float)UnitsGuess(type, yaw_reference);
    g_tracked_pitch_min = g_pitch_min = g_samples[5][pitch_index];
    g_tracked_pitch_max = g_pitch_max = g_samples[5][pitch_index];
    g_pitch_tracking = true;

    LOGI("selected yaw direction %+d, pitch direction %+d, one turn = %.6f", yaw_dir,
         pitch_dir, (double)g_units_per_turn);
    for (size_t i = 0; i < g_scored_yaw.size() && i < 8; ++i) {
        const ScoredSlot& candidate = g_scored_yaw[i];
        const RawSlot& raw = g_raw_slots[candidate.slot_index];
        char path[512];
        FormatCameraPath(MakeCameraSlot(candidate.slot_index, yaw_dir), path, sizeof(path));
        LOGI("  yaw[%zu] score %.2f deg, %s, class %s, path %s, values %.6f %.6f %.6f %.6f",
             i, candidate.score, TypeName(raw.type), g_blocks[raw.block].class_name, path,
             g_samples[1][candidate.slot_index], g_samples[2][candidate.slot_index],
             g_samples[3][candidate.slot_index], g_samples[4][candidate.slot_index]);
    }
    for (size_t i = 0; i < g_scored_pitch.size() && i < 8; ++i) {
        const ScoredSlot& candidate = g_scored_pitch[i];
        const RawSlot& raw = g_raw_slots[candidate.slot_index];
        char path[512];
        FormatCameraPath(MakeCameraSlot(candidate.slot_index, pitch_dir), path, sizeof(path));
        LOGI("  pitch[%zu] score %.2f deg, %s, class %s, path %s, values %.6f %.6f %.6f %.6f",
             i, candidate.score, TypeName(raw.type), g_blocks[raw.block].class_name, path,
             g_samples[3][candidate.slot_index], g_samples[4][candidate.slot_index],
             g_samples[5][candidate.slot_index], g_samples[2][candidate.slot_index]);
    }
    return !g_calibrated_yaw.empty() && !g_calibrated_pitch.empty();
}

static void TrackPitchLimits() {
    if (!g_pitch_tracking || g_calibrated_pitch.empty()) return;
    for (const CameraSlot& slot : g_calibrated_pitch) {
        uint32_t address = ResolveCameraSlot(slot);
        double value = 0.0;
        if (!address || !ReadValue(address, slot.type, &value)) continue;
        g_tracked_pitch_min = std::min(g_tracked_pitch_min, value);
        g_tracked_pitch_max = std::max(g_tracked_pitch_max, value);
    }
}

static void SaveCalibration() {
    if (!g_conf || !forge_api->config_set || !forge_api->config_save) return;
    char text[4096];
    FormatCameraSlotList(g_calibrated_yaw, text, sizeof(text));
    forge_api->config_set(g_conf, "calib", "yaw_paths", text);
    FormatCameraSlotList(g_calibrated_pitch, text, sizeof(text));
    forge_api->config_set(g_conf, "calib", "pitch_paths", text);

    char number[64];
    snprintf(number, sizeof(number), "%.9f", (double)g_units_per_turn);
    forge_api->config_set(g_conf, "calib", "units_per_turn", number);
    snprintf(number, sizeof(number), "%.9f", g_pitch_min);
    forge_api->config_set(g_conf, "calib", "pitch_min", number);
    snprintf(number, sizeof(number), "%.9f", g_pitch_max);
    forge_api->config_set(g_conf, "calib", "pitch_max", number);
    forge_api->config_set(g_conf, "calib", "calib_start", "false");
    forge_api->config_set(g_conf, "build", "exe_sha256", g_exe_hash);
    forge_api->config_save(g_conf);
}

static void FinishCalibration() {
    HoldCalibrationKey(0);
    g_pitch_tracking = false;
    if (g_calibrated_yaw.empty() || g_calibrated_pitch.empty() ||
        !ValidateCameraSlots(g_calibrated_yaw) || !ValidateCameraSlots(g_calibrated_pitch)) {
        LOGE("calibration failed: selected fields did not resolve at save time");
        g_cal_state = CalibrationState::Failed;
        return;
    }

    double measured_span = g_tracked_pitch_max - g_tracked_pitch_min;
    if (measured_span >= g_units_per_turn * 0.15) {
        double margin = std::max(measured_span * 0.02, g_units_per_turn * 0.001);
        g_pitch_min = g_tracked_pitch_min - margin;
        g_pitch_max = g_tracked_pitch_max + margin;
    } else {
        g_pitch_min = -g_units_per_turn * 0.22;
        g_pitch_max = g_units_per_turn * 0.22;
    }

    g_yaw = g_calibrated_yaw;
    g_pitch = g_calibrated_pitch;
    g_have_camera = true;
    InvalidateMouseTargets();
    g_bad_path_seconds = 0.0f;
    SaveCalibration();
    g_cal_state = CalibrationState::Done;
    LOGI("camera locked: %zu yaw path(s), %zu pitch path(s), U %.6f, pitch %.6f..%.6f",
         g_yaw.size(), g_pitch.size(), (double)g_units_per_turn, g_pitch_min, g_pitch_max);
    if (g_cfg.beep) MessageBeep(MB_OK);
}

static void CalibrationFail(const char* reason) {
    HoldCalibrationKey(0);
    g_pitch_tracking = false;
    LOGE("calibration failed: %s", reason);
    g_cal_state = CalibrationState::Failed;
}

static bool StartCalibration() {
    if (g_cal_state != CalibrationState::Off && g_cal_state != CalibrationState::Done &&
        g_cal_state != CalibrationState::Failed)
        return false;
    if (!g_exe_hash_ok) {
        LOGE("calibration refused: executable hash does not match expected build");
        return false;
    }
    if (g_cfg.camera_vector) {
        LOGI("F7 calibration is not used in camera-vector mode");
        return false;
    }
    if (!GameIsFocused()) {
        LOGE("calibration refused: game window must be focused");
        return false;
    }
    uint32_t root = CameraRoot();
    if (!root) {
        LOGE("calibration failed: singleton %s not found", g_cfg.camera_root);
        return false;
    }
    if (!BuildCameraBlocks(root)) {
        LOGE("calibration failed: no readable camera objects");
        return false;
    }

    g_samples.assign(6, std::vector<double>(g_raw_slots.size(), 0.0));
    g_sample_valid.assign(6, std::vector<uint8_t>(g_raw_slots.size(), 0));
    g_calibrated_yaw.clear();
    g_calibrated_pitch.clear();
    g_scored_yaw.clear();
    g_scored_pitch.clear();
    g_cal_timer = 0.35f;
    g_pitch_tracking = false;
    g_dx.store(0);
    g_dy.store(0);
    if (!TakeSample(0)) return false;
    LOGI("calibration started: enter a free-camera scene and leave the stick untouched");
    g_cal_state = CalibrationState::IdleWait;
    return true;
}

static void TickCalibration(float dt) {
    g_cal_timer -= dt;
    bool timer_done = g_cal_timer <= 0.0f;

    switch (g_cal_state) {
        case CalibrationState::IdleWait:
            if (!timer_done) return;
            if (!TakeSample(1)) {
                CalibrationFail("idle snapshot failed");
                return;
            }
            LOGI("calibration 1/6: turning right");
            HoldCalibrationKey(g_stick_vk[3]);
            g_cal_timer = 1.55f;
            g_cal_state = CalibrationState::RightHold;
            return;

        case CalibrationState::RightHold:
            if (!timer_done) return;
            HoldCalibrationKey(0);
            g_cal_timer = 0.25f;
            g_cal_state = CalibrationState::RightSettle;
            return;

        case CalibrationState::RightSettle:
            if (!timer_done) return;
            if (!TakeSample(2)) {
                CalibrationFail("right snapshot failed");
                return;
            }
            LOGI("calibration 2/6: turning left");
            HoldCalibrationKey(g_stick_vk[1]);
            g_cal_timer = 1.55f;
            g_cal_state = CalibrationState::LeftHold;
            return;

        case CalibrationState::LeftHold:
            if (!timer_done) return;
            HoldCalibrationKey(0);
            g_cal_timer = 0.25f;
            g_cal_state = CalibrationState::LeftSettle;
            return;

        case CalibrationState::LeftSettle:
            if (!timer_done) return;
            if (!TakeSample(3)) {
                CalibrationFail("left snapshot failed");
                return;
            }
            LOGI("calibration 3/6: tilting up");
            HoldCalibrationKey(g_stick_vk[0]);
            g_cal_timer = 1.3f;
            g_cal_state = CalibrationState::UpHold;
            return;

        case CalibrationState::UpHold:
            if (!timer_done) return;
            HoldCalibrationKey(0);
            g_cal_timer = 0.25f;
            g_cal_state = CalibrationState::UpSettle;
            return;

        case CalibrationState::UpSettle:
            if (!timer_done) return;
            if (!TakeSample(4)) {
                CalibrationFail("up snapshot failed");
                return;
            }
            LOGI("calibration 4/6: tilting down");
            HoldCalibrationKey(g_stick_vk[2]);
            g_cal_timer = 1.3f;
            g_cal_state = CalibrationState::DownHold;
            return;

        case CalibrationState::DownHold:
            if (!timer_done) return;
            HoldCalibrationKey(0);
            g_cal_timer = 0.25f;
            g_cal_state = CalibrationState::DownSettle;
            return;

        case CalibrationState::DownSettle:
            if (!timer_done) return;
            if (!TakeSample(5)) {
                CalibrationFail("down snapshot failed");
                return;
            }
            if (!BuildScoredCandidates()) {
                CalibrationFail("no field followed exactly one camera axis");
                return;
            }
            if (!SelectCalibrationSlots()) {
                CalibrationFail("yaw and pitch candidates are incompatible");
                return;
            }
            LOGI("calibration 5/6: measuring yaw wrap and pitch limits");
            HoldCalibrationKey(g_stick_vk[3]);
            g_cal_timer = 3.8f;
            g_cal_state = CalibrationState::SpinYaw;
            return;

        case CalibrationState::SpinYaw:
            TrackPitchLimits();
            if (!timer_done) return;
            HoldCalibrationKey(0);
            HoldCalibrationKey(g_stick_vk[0]);
            g_cal_timer = 2.4f;
            g_cal_state = CalibrationState::PitchUp;
            return;

        case CalibrationState::PitchUp:
            TrackPitchLimits();
            if (!timer_done) return;
            HoldCalibrationKey(0);
            HoldCalibrationKey(g_stick_vk[2]);
            g_cal_timer = 4.2f;
            g_cal_state = CalibrationState::PitchDown;
            return;

        case CalibrationState::PitchDown:
            TrackPitchLimits();
            if (!timer_done) return;
            HoldCalibrationKey(0);
            LOGI("calibration 6/6: validating selected paths");
            g_cal_state = CalibrationState::Finish;
            return;

        case CalibrationState::Finish:
            FinishCalibration();
            return;

        default:
            return;
    }
}

/* ---------------------------------------------------------------- camera apply */

static void InvalidateMouseTargets() {
    std::lock_guard<std::mutex> lock(g_vector_mutex);
    g_targets_valid = false;
    g_vector_valid = false;
    g_vector_camera_object = 0;
    g_yaw_targets.clear();
    g_pitch_targets.clear();
    g_yaw_target_ready.clear();
    g_pitch_target_ready.clear();
}

static bool InitializeMouseTargets() {
    InvalidateMouseTargets();
    if (!g_have_camera || g_yaw.empty() || g_pitch.empty()) return false;
    g_yaw_targets.resize(g_yaw.size(), 0.0);
    g_pitch_targets.resize(g_pitch.size(), 0.0);
    g_yaw_target_ready.assign(g_yaw.size(), 0);
    g_pitch_target_ready.assign(g_pitch.size(), 0);
    bool have_yaw = false;
    bool have_pitch = false;
    for (size_t i = 0; i < g_yaw.size(); ++i) {
        uint32_t address = ResolveCameraSlot(g_yaw[i]);
        if (!address || !ReadValue(address, g_yaw[i].type, &g_yaw_targets[i])) continue;
        g_yaw_target_ready[i] = 1;
        have_yaw = true;
    }
    for (size_t i = 0; i < g_pitch.size(); ++i) {
        uint32_t address = ResolveCameraSlot(g_pitch[i]);
        if (!address || !ReadValue(address, g_pitch[i].type, &g_pitch_targets[i])) continue;
        g_pitch_target_ready[i] = 1;
        have_pitch = true;
    }
    g_targets_valid = have_yaw && have_pitch;
    return g_targets_valid;
}

static double VectorLength(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

static bool ReadCameraVector(uint32_t object, uint32_t offset, Vec3* value) {
    float raw[3]{};
    if (!forge_api->mem_isValid(object + offset, sizeof(raw)) ||
        !forge_api->mem_read(object + offset, raw, sizeof(raw)))
        return false;
    if (!std::isfinite(raw[0]) || !std::isfinite(raw[1]) || !std::isfinite(raw[2]))
        return false;
    value->x = raw[0];
    value->y = raw[1];
    value->z = raw[2];
    return true;
}

static bool WriteCameraVector(uint32_t object, uint32_t offset, const Vec3& value) {
    float raw[3] = {(float)value.x, (float)value.y, (float)value.z};
    return forge_api->mem_isValid(object + offset, sizeof(raw)) &&
           forge_api->mem_write(object + offset, raw, sizeof(raw));
}

static bool InitializeCameraVector(uint32_t object) {
    uint32_t dti = forge_api->dti_of ? forge_api->dti_of(object) : 0;
    const char* class_name =
        (dti && forge_api->dti_name) ? forge_api->dti_name(dti) : nullptr;
    if (!class_name || strcmp(class_name, "uQuestCamera") != 0) {
        LOGE("camera vector rejected object 0x%08X (class %s)", object,
             class_name ? class_name : "?");
        return false;
    }
    Vec3 position;
    Vec3 target;
    if (!ReadCameraVector(object, 0x40, &position) ||
        !ReadCameraVector(object, 0x60, &target))
        return false;
    g_vector_base_radial =
        Vec3{position.x - target.x, position.y - target.y, position.z - target.z};
    double length = VectorLength(g_vector_base_radial);
    if (length < 0.01 || length > 100000.0) return false;
    g_vector_yaw_offset = 0.0;
    g_vector_pitch_offset = 0.0;
    g_vector_camera_object = object;
    g_vector_valid = true;
    LOGI("camera vector locked to object 0x%08X: eye %.2f %.2f %.2f target %.2f %.2f %.2f",
         object, position.x, position.y, position.z, target.x, target.y, target.z);
    return true;
}

static Vec3 RotateY(const Vec3& value, double angle) {
    double c = std::cos(angle);
    double s = std::sin(angle);
    return {value.x * c + value.z * s, value.y, -value.x * s + value.z * c};
}

static Vec3 RotateAxis(const Vec3& value, const Vec3& axis, double angle) {
    double c = std::cos(angle);
    double s = std::sin(angle);
    Vec3 cross{axis.y * value.z - axis.z * value.y,
               axis.z * value.x - axis.x * value.z,
               axis.x * value.y - axis.y * value.x};
    double dot = axis.x * value.x + axis.y * value.y + axis.z * value.z;
    return {value.x * c + cross.x * s + axis.x * dot * (1.0 - c),
            value.y * c + cross.y * s + axis.y * dot * (1.0 - c),
            value.z * c + cross.z * s + axis.z * dot * (1.0 - c)};
}

static bool UpdateCameraVectorOffsets(long long dx, long long dy) {
    std::lock_guard<std::mutex> lock(g_vector_mutex);
    if (!g_live_camera_is_quest.load(std::memory_order_relaxed)) return false;
    uint32_t object = g_live_camera_object.load(std::memory_order_relaxed);
    if (!object || !forge_api->mem_isValid(object + 0x40, 0x50)) return false;
    if (!g_vector_valid || object != g_vector_camera_object) {
        if (!InitializeCameraVector(object)) return false;
    }

    constexpr double kPi = 3.14159265358979323846;
    double yaw_step = (double)dx * g_cfg.sens_x * kPi / 180.0;
    double pitch_step = (double)dy * g_cfg.sens_y * kPi / 180.0;
    if (g_cfg.invert_x) yaw_step = -yaw_step;
    if (g_cfg.invert_y) pitch_step = -pitch_step;
    g_vector_yaw_offset += yaw_step;
    g_vector_pitch_offset += pitch_step;
    constexpr double pitch_limit = 75.0 * kPi / 180.0;
    g_vector_pitch_offset =
        std::clamp(g_vector_pitch_offset, -pitch_limit, pitch_limit);
    return true;
}

static bool ApplyCameraVectorNow() {
    std::lock_guard<std::mutex> lock(g_vector_mutex);
    if (!g_cfg.enabled || !g_vector_valid ||
        !g_live_camera_is_quest.load(std::memory_order_relaxed))
        return false;
    unsigned long long since = g_live_camera_since_tick.load(std::memory_order_relaxed);
    if (!since || GetTickCount64() - since < 400) return false;
    uint32_t object = g_vector_camera_object;
    if (!object || !forge_api->mem_isValid(object + 0x40, 0xAC0)) return false;
    Vec3 target;
    if (!ReadCameraVector(object, 0x60, &target)) return false;
    Vec3 radial = RotateY(g_vector_base_radial, g_vector_yaw_offset);
    Vec3 forward{-radial.x, -radial.y, -radial.z};
    double forward_length = VectorLength(forward);
    if (forward_length < 0.01) return false;
    forward.x /= forward_length;
    forward.y /= forward_length;
    forward.z /= forward_length;
    Vec3 right{-forward.z, 0.0, forward.x};
    double right_length = VectorLength(right);
    if (right_length < 0.01) return false;
    right.x /= right_length;
    right.z /= right_length;
    radial = RotateAxis(radial, right, g_vector_pitch_offset);

    Vec3 position{target.x + radial.x, target.y + radial.y, target.z + radial.z};
    Vec3 final_forward{target.x - position.x, target.y - position.y,
                       target.z - position.z};
    double final_forward_length = VectorLength(final_forward);
    if (final_forward_length < 0.01) return false;
    final_forward.x /= final_forward_length;
    final_forward.y /= final_forward_length;
    final_forward.z /= final_forward_length;
    Vec3 final_right{-final_forward.z, 0.0, final_forward.x};
    double final_right_length = VectorLength(final_right);
    if (final_right_length < 0.01) return false;
    final_right.x /= final_right_length;
    final_right.z /= final_right_length;
    Vec3 up{final_right.y * final_forward.z - final_right.z * final_forward.y,
            final_right.z * final_forward.x - final_right.x * final_forward.z,
            final_right.x * final_forward.y - final_right.y * final_forward.x};
    double up_length = VectorLength(up);
    if (up_length < 0.01) return false;
    up.x /= up_length;
    up.y /= up_length;
    up.z /= up_length;

    bool wrote = WriteCameraVector(object, 0x40, position);
    wrote = WriteCameraVector(object, 0x50, up) || wrote;
    return wrote;
}

static bool ApplyMouseLook(long long dx, long long dy) {
    if (g_cfg.camera_vector) return UpdateCameraVectorOffsets(dx, dy);
    if (!g_have_camera || !g_targets_valid) return false;

    double units = g_units_per_turn;
    double yaw_delta = (double)dx * g_cfg.sens_x * units / 360.0;
    double pitch_delta = (double)dy * g_cfg.sens_y * units / 360.0;
    if (g_cfg.invert_y) pitch_delta = -pitch_delta;

    bool any_read = false;
    for (size_t i = 0; i < g_yaw.size(); ++i) {
        const CameraSlot& slot = g_yaw[i];
        uint32_t address = ResolveCameraSlot(slot);
        if (!address) continue;
        if (!g_yaw_target_ready[i]) {
            if (!ReadValue(address, slot.type, &g_yaw_targets[i])) continue;
            g_yaw_target_ready[i] = 1;
        }
        any_read = true;
        double next = g_yaw_targets[i] + yaw_delta * slot.dir;
        if (slot.type == SLOT_F32) next = WrapValue(next, units);
        g_yaw_targets[i] = next;
        WriteValue(address, slot.type, next);
    }
    for (size_t i = 0; i < g_pitch.size(); ++i) {
        const CameraSlot& slot = g_pitch[i];
        uint32_t address = ResolveCameraSlot(slot);
        if (!address) continue;
        if (!g_pitch_target_ready[i]) {
            if (!ReadValue(address, slot.type, &g_pitch_targets[i])) continue;
            g_pitch_target_ready[i] = 1;
        }
        any_read = true;
        double next =
            std::clamp(g_pitch_targets[i] + pitch_delta * slot.dir, g_pitch_min, g_pitch_max);
        g_pitch_targets[i] = next;
        WriteValue(address, slot.type, next);
    }
    return any_read;
}

/* ---------------------------------------------------------------- settings load */

static void LoadSettings() {
    if (!g_conf) return;
    g_cfg.sens_x = forge_api->config_float(g_conf, "mouse", "sens_x_deg_per_count", 0.06f);
    g_cfg.sens_y = forge_api->config_float(g_conf, "mouse", "sens_y_deg_per_count", 0.06f);
    g_cfg.invert_x = forge_api->config_bool(g_conf, "mouse", "invert_x", false);
    g_cfg.invert_y = forge_api->config_bool(g_conf, "mouse", "invert_y", false);
    g_cfg.enabled = forge_api->config_bool(g_conf, "mouse", "enabled", true);
    g_cfg.auto_switch = forge_api->config_bool(g_conf, "mouse", "auto_switch", true);
    g_cfg.handback_ms = forge_api->config_int(g_conf, "mouse", "handback_ms", 700);
    g_cfg.activate_px = forge_api->config_int(g_conf, "mouse", "activate_px", 2);
    g_cfg.button_hold_ms =
        forge_api->config_int(g_conf, "mouse", "button_hold_ms", 100);
    g_cfg.wheel_hold_ms =
        forge_api->config_int(g_conf, "mouse", "wheel_hold_ms", 100);
    g_cfg.hide_cursor = forge_api->config_bool(g_conf, "mouse", "hide_cursor", true);
    g_cfg.clip_cursor = forge_api->config_bool(g_conf, "mouse", "clip_cursor", false);
    g_cfg.center_cursor = forge_api->config_bool(g_conf, "mouse", "center_cursor", true);
    g_cfg.cursor_fallback =
        forge_api->config_bool(g_conf, "mouse", "cursor_fallback", true);
    g_cfg.beep = forge_api->config_bool(g_conf, "mouse", "beep", true);
    g_cfg.debug = forge_api->config_bool(g_conf, "mouse", "debug", false);
    g_cfg.camera_vector = forge_api->config_bool(g_conf, "mouse", "camera_vector", true);
    g_cfg.camera_radius =
        forge_api->config_float(g_conf, "mouse", "camera_radius", 500.0f);

    g_cfg.probe_chunks = forge_api->config_int(g_conf, "calib", "probe_chunks", 1);
    g_cfg.probe_bytes = forge_api->config_int(g_conf, "calib", "probe_bytes", 4096);
    g_cfg.max_depth = forge_api->config_int(g_conf, "calib", "max_depth", 1);
    GetConfigString("calib", "camera_root", g_cfg.camera_root, sizeof(g_cfg.camera_root),
                    "sFestaCamera");
    GetConfigString("mouse", "stick_keys", g_cfg.stick_keys, sizeof(g_cfg.stick_keys), "IJKL");

    GetConfigString("keys", "left", g_cfg.key_lmb, sizeof(g_cfg.key_lmb), "Z");
    GetConfigString("keys", "right", g_cfg.key_rmb, sizeof(g_cfg.key_rmb), "X");
    GetConfigString("keys", "middle", g_cfg.key_mmb, sizeof(g_cfg.key_mmb), "F");
    GetConfigString("keys", "x1", g_cfg.key_x1, sizeof(g_cfg.key_x1), "SPACE");
    GetConfigString("keys", "x2", g_cfg.key_x2, sizeof(g_cfg.key_x2), "E");
    GetConfigString("keys", "wheel_hold", g_cfg.wheel_hold, sizeof(g_cfg.wheel_hold), "");
    GetConfigString("keys", "wheel_up", g_cfg.wheel_up, sizeof(g_cfg.wheel_up), "3");
    GetConfigString("keys", "wheel_down", g_cfg.wheel_down, sizeof(g_cfg.wheel_down), "4");

    g_button_vk_count[0] = ParseVkList(g_cfg.key_lmb, g_button_vks[0], 4);
    g_button_vk_count[1] = ParseVkList(g_cfg.key_mmb, g_button_vks[1], 4);
    g_button_vk_count[2] = ParseVkList(g_cfg.key_rmb, g_button_vks[2], 4);
    g_button_vk_count[3] = ParseVkList(g_cfg.key_x1, g_button_vks[3], 4);
    g_button_vk_count[4] = ParseVkList(g_cfg.key_x2, g_button_vks[4], 4);
    g_wheel_vk[0] = VkFromName(g_cfg.wheel_up);
    g_wheel_vk[1] = VkFromName(g_cfg.wheel_down);
    g_wheel_hold_vk = VkFromName(g_cfg.wheel_hold);

    const char* p = g_cfg.stick_keys;
    int index = 0;
    while (*p && index < 4) {
        char one[2] = {*p, 0};
        g_stick_vk[index++] = VkFromName(one);
        ++p;
    }
    for (; index < 4; ++index) g_stick_vk[index] = 0;

    char yaw_text[4096], pitch_text[4096];
    GetConfigString("calib", "yaw_paths", yaw_text, sizeof(yaw_text), "");
    GetConfigString("calib", "pitch_paths", pitch_text, sizeof(pitch_text), "");
    ParseCameraSlotList(yaw_text, &g_yaw);
    ParseCameraSlotList(pitch_text, &g_pitch);
    g_units_per_turn = forge_api->config_float(g_conf, "calib", "units_per_turn", 6.2831853f);
    g_pitch_min = forge_api->config_float(g_conf, "calib", "pitch_min", -1.0f);
    g_pitch_max = forge_api->config_float(g_conf, "calib", "pitch_max", 1.0f);
    g_cache_loaded = g_exe_hash_ok && !g_yaw.empty() && !g_pitch.empty();
    g_have_camera =
        g_cache_loaded && ValidateCameraSlots(g_yaw) && ValidateCameraSlots(g_pitch);
    g_cache_was_valid = g_have_camera;
    g_cache_check_timer = 0.0f;
    InvalidateMouseTargets();
}

/* ---------------------------------------------------------------- poke file */

static void PokeLine(const char* line) {
    char command[32] = {};
    char a1[64] = {};
    char a2[64] = {};
    char a3[64] = {};
    int count = sscanf(line, "%31s %63s %63s %63s", command, a1, a2, a3);
    if (count < 1) return;
    uint32_t address = count >= 2 ? (uint32_t)strtoul(a1, nullptr, 0) : 0;
    int type = count >= 4 ? (int)strtol(a3, nullptr, 0) : SLOT_F32;

    if (!strcmp(command, "CA")) {
        StartCalibration();
    } else if (!strcmp(command, "ST")) {
        LOGI("camera: enabled=%d resolved=%d paths=%zu/%zu U=%.9f pitch=%.6f..%.6f",
             (int)g_cfg.enabled, (int)g_have_camera, g_yaw.size(), g_pitch.size(),
             (double)g_units_per_turn, g_pitch_min, g_pitch_max);
    } else if (!strcmp(command, "R")) {
        double value = 0.0;
        if (ReadValue(address, type, &value))
            LOGI("%s 0x%08X = %.9f", TypeName(type), address, value);
        else
            LOGE("unreadable 0x%08X (%s)", address, TypeName(type));
    } else if (!strcmp(command, "W")) {
        double value = strtod(a2, nullptr);
        bool ok = WriteValue(address, type, value);
        double back = 0.0;
        ReadValue(address, type, &back);
        LOGI("%s 0x%08X write %s, reads %.9f", TypeName(type), address, ok ? "ok" : "failed",
             back);
    } else if (!strcmp(command, "D") || !strcmp(command, "F")) {
        int words = count >= 3 ? (int)strtol(a2, nullptr, 0) : 16;
        words = std::clamp(words, 1, 32);
        int dump_type = !strcmp(command, "D") ? SLOT_I16_LO : SLOT_F32;
        int stride = SlotSize(dump_type);
        for (int i = 0; i < words; ++i) {
            double value = 0.0;
            uint32_t at = address + (uint32_t)(i * stride);
            if (!ReadValue(at, dump_type, &value)) continue;
            LOGI("+0x%03X 0x%08X %s = %.9f", i * stride, at, TypeName(dump_type), value);
        }
    } else if (!strcmp(command, "ON") || !strcmp(command, "OFF")) {
        g_cfg.enabled = !strcmp(command, "ON");
        LOGI("mouse look %s", g_cfg.enabled ? "on" : "off");
    } else if (!strcmp(command, "SAVE")) {
        SaveCalibration();
        LOGI("calibration saved");
    } else if (!strcmp(command, "PATHS")) {
        char text[4096];
        FormatCameraSlotList(g_yaw, text, sizeof(text));
        LOGI("yaw paths: %s", text);
        FormatCameraSlotList(g_pitch, text, sizeof(text));
        LOGI("pitch paths: %s", text);
    } else if (!strcmp(command, "DTI")) {
        DumpCameraDti();
    } else if (!strcmp(command, "OD")) {
        uint32_t dti = forge_api->dti_of ? forge_api->dti_of(address) : 0;
        const char* object_name =
            (dti && forge_api->dti_name) ? forge_api->dti_name(dti) : nullptr;
        LOGI("object 0x%08X dti 0x%08X class %s", address, dti,
             object_name ? object_name : "?");
    } else if (!strcmp(command, "SP")) {
        int bytes = count >= 3 ? (int)strtol(a2, nullptr, 0) : 512;
        bytes = std::clamp(bytes, 4, 8192);
        DumpObjectStringPointers("object string pointers", address, (uint32_t)bytes);
    } else if (!strcmp(command, "ODP")) {
        int bytes = count >= 3 ? (int)strtol(a2, nullptr, 0) : 512;
        bytes = std::clamp(bytes, 4, 8192);
        DumpObjectDtiPointers(address, (uint32_t)bytes);
    } else if (!strcmp(command, "HC")) {
        ToggleCameraProbeHooks();
    } else if (!strcmp(command, "LOOK")) {
        int dx = (int)strtol(a1, nullptr, 0);
        int dy = (int)strtol(a2, nullptr, 0);
        LOGI("manual look injection %d/%d: %s", dx, dy,
             ApplyMouseLook(dx, dy) ? "applied" : "failed");
    } else if (!strcmp(command, "CS")) {
        uint32_t object = g_live_camera_object.load();
        g_camera_snapshot.assign(1024, 0.0f);
        if (!object || !forge_api->mem_read(object, g_camera_snapshot.data(),
                                             g_camera_snapshot.size() * sizeof(float))) {
            LOGE("camera snapshot failed: live camera 0x%08X", object);
            g_camera_snapshot.clear();
        } else {
            LOGI("camera snapshot captured: 0x%08X", object);
        }
    } else if (!strcmp(command, "CD")) {
        uint32_t object = g_live_camera_object.load();
        std::vector<float> current(1024, 0.0f);
        if (g_camera_snapshot.empty() || !object ||
            !forge_api->mem_read(object, current.data(), current.size() * sizeof(float))) {
            LOGE("camera diff failed: live camera 0x%08X", object);
        } else {
            LOGI("camera diff 0x%08X:", object);
            int changed = 0;
            for (size_t i = 0; i < current.size(); ++i) {
                float before = g_camera_snapshot[i];
                float after = current[i];
                if (!std::isfinite(before) || !std::isfinite(after)) continue;
                if (fabsf(after - before) <= 1e-6f) continue;
                LOGI("  +0x%03zX %.6f -> %.6f (%+.6f)", i * 4, before, after,
                     after - before);
                if (++changed >= 160) break;
            }
            LOGI("camera diff complete: %d changed float(s)", changed);
        }
    } else if (!strcmp(command, "HEX")) {
        int words = count >= 3 ? (int)strtol(a2, nullptr, 0) : 32;
        words = std::clamp(words, 1, 256);
        for (int i = 0; i < words; ++i) {
            uint32_t at = address + (uint32_t)i * 4;
            uint32_t value = 0;
            if (!forge_api->mem_read(at, &value, 4)) break;
            LOGI("+0x%03X 0x%08X = 0x%08X (%d)", i * 4, at, value, (int32_t)value);
        }
    } else {
        LOGW("unknown poke command: %s", command);
    }
}

static std::wstring Utf8ToWide(const char* text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 0) return {};
    std::wstring result((size_t)count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

static void ProcessPokeFile() {
    char path[640];
    snprintf(path, sizeof(path), "%s\\poke.txt", g_mod_dir);
    std::wstring wide_path = Utf8ToWide(path);
    if (wide_path.empty() || GetFileAttributesW(wide_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    HANDLE file = CreateFileW(wide_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(file, nullptr);
    if (size == 0 || size == INVALID_FILE_SIZE || size > 32768) {
        CloseHandle(file);
        if (size > 32768) DeleteFileW(wide_path.c_str());
        return;
    }
    std::vector<char> data((size_t)size + 1, 0);
    DWORD read = 0;
    ReadFile(file, data.data(), size, &read, nullptr);
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    SetEndOfFile(file);
    CloseHandle(file);

    LOGI("poke.txt:");
    char* cursor = data.data();
    while (*cursor) {
        char* newline = strchr(cursor, '\n');
        if (newline) *newline = 0;
        size_t length = strlen(cursor);
        while (length && (cursor[length - 1] == '\r' || cursor[length - 1] == ' ' ||
                          cursor[length - 1] == '\t'))
            cursor[--length] = 0;
        if (length) PokeLine(cursor);
        cursor = newline ? newline + 1 : cursor + length;
    }
}

static bool ReadAsciiAt(uint32_t address, char* out, size_t cap) {
    if (!out || cap < 2 || !forge_api->mem_isValid(address, 1)) return false;
    size_t used = 0;
    for (size_t i = 0; i + 1 < cap; ++i) {
        char ch = 0;
        if (!forge_api->mem_read(address + (uint32_t)i, &ch, 1)) break;
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7e) break;
        out[used++] = ch;
    }
    out[used] = 0;
    return used >= 3;
}

static void DumpObjectStringPointers(const char* label, uint32_t address, uint32_t bytes) {
    if (!address || !forge_api->mem_isValid(address, bytes)) {
        LOGW("%s 0x%08X is unreadable", label, address);
        return;
    }
    LOGI("%s 0x%08X (%u bytes):", label, address, bytes);
    for (uint32_t offset = 0; offset + 4 <= bytes; offset += 4) {
        uint32_t pointer = 0;
        if (!forge_api->mem_read(address + offset, &pointer, 4)) break;
        if (pointer < 0x00300000u || pointer >= 0x80000000u) continue;
        char text[96];
        if (!ReadAsciiAt(pointer, text, sizeof(text))) continue;
        LOGI("  +0x%03X -> 0x%08X \"%s\"", offset, pointer, text);
    }
}

static void DumpCameraDti() {
    uint32_t root = CameraRoot();
    if (!root) {
        LOGE("DTI dump: %s singleton is unavailable", g_cfg.camera_root);
        return;
    }
    uint32_t dti = forge_api->dti_of ? forge_api->dti_of(root) : 0;
    const char* class_name =
        (dti && forge_api->dti_name) ? forge_api->dti_name(dti) : nullptr;
    LOGI("DTI dump: root 0x%08X, dti 0x%08X, class %s", root, dti,
         class_name ? class_name : "?");
    DumpObjectStringPointers("root string pointers", root, 4096);
    if (dti) DumpObjectStringPointers("DTI string pointers", dti, 512);
}

static void DumpObjectDtiPointers(uint32_t address, uint32_t bytes) {
    if (!address || !forge_api->dti_of || !forge_api->mem_isValid(address, bytes)) return;
    LOGI("object DTI pointers 0x%08X (%u bytes):", address, bytes);
    int logged = 0;
    for (uint32_t offset = 0; offset + 4 <= bytes && logged < 128; offset += 4) {
        uint32_t pointer = 0;
        if (!forge_api->mem_read(address + offset, &pointer, 4)) break;
        if (pointer < 0x00300000u || pointer >= 0x80000000u ||
            !forge_api->mem_isValid(pointer, 4))
            continue;
        uint32_t dti = forge_api->dti_of(pointer);
        if (!dti) continue;
        const char* name = forge_api->dti_name ? forge_api->dti_name(dti) : nullptr;
        if (!name || !*name) continue;
        LOGI("  +0x%03X -> 0x%08X dti 0x%08X %s", offset, pointer, dti, name);
        ++logged;
    }
}

/* ---------------------------------------------------------------- plugin entry */

extern "C" {

FORGE_EXPORT void forge_onLoad(ForgePluginInfo* info) {
    info->required_ver.major = 0;
    info->required_ver.minor = 7;
    info->required_ver.patch = 0;
    snprintf(info->name, sizeof(info->name), "MouseLook");
}

FORGE_EXPORT void forge_onInit(void) {
    g_raw_running.store(true);
    if (forge_api->config_open) g_conf = forge_api->config_open("mouselook.ini");
    if (forge_api->mods_dir)
        snprintf(g_mod_dir, sizeof(g_mod_dir), "%s\\MouseLook", forge_api->mods_dir());

    if (!ComputeExecutableSha256(g_exe_hash)) {
        LOGE("could not calculate the game executable SHA-256");
        snprintf(g_exe_hash, sizeof(g_exe_hash), "%s", kExpectedExeHash);
    }
    char expected[65] = {};
    if (g_conf) GetConfigString("build", "exe_sha256", expected, sizeof(expected), "");
    if (!expected[0] && g_conf && forge_api->config_set) {
        forge_api->config_set(g_conf, "build", "exe_sha256", g_exe_hash);
        if (forge_api->config_save) forge_api->config_save(g_conf);
    }
    g_exe_hash_ok = !strcmp(g_exe_hash, kExpectedExeHash) &&
                    (!expected[0] || !strcmp(expected, kExpectedExeHash));
    if (!g_exe_hash_ok)
        LOGE("executable hash mismatch: found %s, expected %s", g_exe_hash, kExpectedExeHash);

    LoadSettings();
    g_game_window = FindGameWindow();
    g_blank_cursor = CreateBlankCursor();
    if (g_cfg.camera_vector) InstallCameraProbeHooks(false);
    g_raw_thread = std::thread(RawInputThread);

    uint32_t main_base =
        forge_api->mem_getModuleBase ? forge_api->mem_getModuleBase("main") : 0;
    uint32_t main_size =
        forge_api->mem_getModuleSize ? forge_api->mem_getModuleSize("main") : 0;
    uint32_t camera_root = CameraRoot();
    LOGI("ready: game sha256 %s, main 0x%08X+0x%08X, %s 0x%08X",
         g_exe_hash_ok ? "verified" : "MISMATCH", main_base, main_size, g_cfg.camera_root,
         camera_root);
    if (g_have_camera)
        LOGI("camera paths restored: %zu yaw, %zu pitch", g_yaw.size(), g_pitch.size());
    else if (g_cfg.camera_vector)
        LOGI("render-camera mode ready; F8 toggles mouse look");
    else
        LOGI("no valid camera cache; enter a free-camera scene and press F7");
}

FORGE_EXPORT void forge_onUpdate(float dt) {
    ++g_frames;
    bool focused = GameIsFocused();
    bool menu_open = forge_api->menu_isOpen && forge_api->menu_isOpen();

    if (g_cache_loaded) {
        g_cache_check_timer -= dt;
        if (g_cache_check_timer <= 0.0f) {
            g_cache_check_timer = 0.75f;
            bool valid =
                ValidateCameraSlots(g_yaw) && ValidateCameraSlots(g_pitch);
            if (valid != g_cache_was_valid) {
                if (valid)
                    LOGI("camera paths reacquired in the current scene");
                else
                    LOGW("camera paths unavailable in the current scene; waiting");
                g_cache_was_valid = valid;
            }
            g_have_camera = valid;
            if (!valid) InvalidateMouseTargets();
        }
    }

    if (g_cfg.camera_vector && g_live_camera_is_quest.load(std::memory_order_relaxed)) {
        unsigned long long last_hook =
            g_last_camera_hook_tick.load(std::memory_order_relaxed);
        if (last_hook && GetTickCount64() - last_hook > 600) {
            g_live_camera_object.store(0, std::memory_order_relaxed);
            g_live_camera_is_quest.store(false, std::memory_order_relaxed);
            g_live_camera_since_tick.store(0, std::memory_order_relaxed);
            InvalidateMouseTargets();
        }
    }

    static float poke_timer = 0.0f;
    poke_timer += dt;
    if (poke_timer >= 0.25f) {
        poke_timer = 0.0f;
        ProcessPokeFile();
        if (g_conf && forge_api->config_changed && forge_api->config_changed(g_conf))
            LoadSettings();
        if (g_conf && forge_api->config_bool(g_conf, "calib", "calib_start", false)) {
            forge_api->config_set(g_conf, "calib", "calib_start", "false");
            if (forge_api->config_save) forge_api->config_save(g_conf);
            StartCalibration();
        }
    }

    static bool previous_f7 = false;
    static bool previous_f8 = false;
    bool f7 = focused && !menu_open && (GetAsyncKeyState(VK_F7) & 0x8000);
    bool f8 = focused && !menu_open && (GetAsyncKeyState(VK_F8) & 0x8000);
    if (f7 && !previous_f7) StartCalibration();
    if (f8 && !previous_f8) {
        g_cfg.enabled = !g_cfg.enabled;
        LOGI("mouse look %s", g_cfg.enabled ? "on" : "off");
        if (g_cfg.beep) MessageBeep(g_cfg.enabled ? MB_OK : MB_ICONHAND);
    }
    previous_f7 = f7;
    previous_f8 = f8;

    if (g_cal_state != CalibrationState::Off && g_cal_state != CalibrationState::Done &&
        g_cal_state != CalibrationState::Failed) {
        if (!focused || menu_open) {
            CalibrationFail("window lost focus during calibration");
        } else {
            TickCalibration(dt);
        }
        return;
    }

    long long pending_x = 0;
    long long pending_y = 0;
    SampleMouseMotion(focused && !menu_open, false, &pending_x, &pending_y);

    float stick_x = 0.0f;
    float stick_y = 0.0f;
    SampleRightStick(&stick_x, &stick_y);
    bool pad_moving = fabsf(stick_x) > 0.15f || fabsf(stick_y) > 0.15f;
    if (pad_moving && g_cfg.auto_switch) {
        g_pad_cooldown = std::max(50, g_cfg.handback_ms) / 1000.0f;
        g_mouse_owns = false;
        InvalidateMouseTargets();
    } else if (g_pad_cooldown > 0.0f) {
        g_pad_cooldown -= dt;
        if (g_pad_cooldown <= 0.0f) g_pad_cooldown = 0.0f;
    }

    bool mouse_moved =
        (llabs(pending_x) + llabs(pending_y)) >= std::max(1, g_cfg.activate_px);
    if (mouse_moved && (!g_cfg.auto_switch || g_pad_cooldown <= 0.0f)) g_mouse_owns = true;

    bool allow_controls = focused && !menu_open;
    bool allow_camera = allow_controls && g_cfg.enabled && g_have_camera &&
                        (!g_cfg.auto_switch || (g_mouse_owns && g_pad_cooldown <= 0.0f));
    if (g_cfg.camera_vector) allow_camera =
        allow_controls && g_cfg.enabled &&
        g_live_camera_is_quest.load(std::memory_order_relaxed) &&
        (!g_cfg.auto_switch || (g_mouse_owns && g_pad_cooldown <= 0.0f));
    if (allow_camera) {
        if (!g_cfg.camera_vector && !g_targets_valid) InitializeMouseTargets();
        bool wrote = ApplyMouseLook(pending_x, pending_y);
        if (g_cfg.center_cursor) {
            POINT center = GameClientCenter();
            if (center.x || center.y) {
                SetCursorPos(center.x, center.y);
                g_last_cursor = center;
                g_last_cursor_valid = true;
            }
        }
        if (!wrote && (pending_x || pending_y)) {
            g_bad_path_seconds += dt;
            if (g_bad_path_seconds > 1.0f) {
                LOGW("camera path is temporarily unavailable; waiting for the next scene");
                g_bad_path_seconds = 0.0f;
            }
        } else {
            g_bad_path_seconds = 0.0f;
        }
    } else {
        InvalidateMouseTargets();
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            g_last_cursor = cursor;
            g_last_cursor_valid = true;
        }
        if (!allow_controls) g_mouse_owns = false;
    }

    bool gameplay_input =
        !g_cfg.camera_vector || g_live_camera_is_quest.load(std::memory_order_relaxed);
    HandleMouseButtons(allow_controls && gameplay_input, dt);
    HandleMouseWheel(allow_controls && gameplay_input, dt);
    UpdateCursorHidden(g_cfg.hide_cursor && allow_camera);
    SetCursorClipped(g_cfg.clip_cursor && allow_camera);

    if (g_cfg.debug) {
        static ULONGLONG last_debug_log = 0;
        ULONGLONG now = GetTickCount64();
        if (now - last_debug_log >= 1000) {
            last_debug_log = now;
            double yaw_value = 0.0;
            double pitch_value = 0.0;
            if (!g_yaw.empty())
                ReadValue(ResolveCameraSlot(g_yaw.front()), g_yaw.front().type, &yaw_value);
            if (!g_pitch.empty())
                ReadValue(ResolveCameraSlot(g_pitch.front()), g_pitch.front().type,
                          &pitch_value);
            LOGI("yaw %.6f pitch %.6f | active %d mouse %lld/%lld pad %.2f %.2f | focus %d | "
                 "frames %d",
                 yaw_value, pitch_value, (int)allow_camera, pending_x, pending_y, stick_x,
                 stick_y, (int)focused, g_frames);
        }
    }
}

FORGE_EXPORT void forge_onUnload(void) {
    g_raw_running.store(false);
    HoldCalibrationKey(0);
    ReleaseAllMappedKeys();
    UpdateCursorHidden(false);
    if (g_blank_cursor) {
        DestroyCursor(g_blank_cursor);
        g_blank_cursor = nullptr;
    }
    SetCursorClipped(false);
    HWND window = g_raw_window.load();
    if (window) PostMessageW(window, WM_QUIT, 0, 0);
    if (g_raw_thread.joinable()) g_raw_thread.join();
}

} /* extern "C" */
