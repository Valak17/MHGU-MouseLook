/*
 * MouseLookDirect - native mouse camera control for the recompiled MHGU PC build.
 *
 * This is an intentionally separate implementation from the old MouseLook
 * plugin. It never installs Forge code hooks and never touches the game during
 * the title or main menu. Once enabled with F6, it finds uQuestCamera through
 * the sFestaCamera singleton and writes the camera eye/up vectors directly.
 */
#include "forge_pc.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

FORGE_PLUGIN_DEFINE_API

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kPitchLimit = 75.0 * kPi / 180.0;
static constexpr uint32_t kRootScanBytes = 4096;
static constexpr uint32_t kQuestCameraPointerPathOffset = 0x54;

static void LOG(ForgeLogLevel level, const char* format, ...) {
    if (!forge_api || !forge_api->log) return;
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    forge_api->log(level, "MouseLookDirect", text);
}

#define LOGI(...) LOG(FORGE_LOG_INFO, __VA_ARGS__)
#define LOGW(...) LOG(FORGE_LOG_WARN, __VA_ARGS__)
#define LOGE(...) LOG(FORGE_LOG_ERROR, __VA_ARGS__)

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Settings {
    float sensitivity_x = 0.06f;
    float sensitivity_y = 0.06f;
    bool invert_x = true;
    bool invert_y = true;
    bool hide_cursor = true;
    bool center_cursor = true;
    bool angle_mode = false;
    int button_hold_ms = 100;
    int wheel_hold_ms = 100;
    int settle_ms = 1500;
    int yaw_direction = -1;
    int pitch_direction = -1;
    float units_per_turn = 65536.0f;
    float pitch_min = -17000.0f;
    float pitch_max = 17000.0f;
    char yaw_offsets[512] = "0xAE,0xEA,0xD2";
    char pitch_offsets[512] = "0x196";
    char toggle_key[32] = "F8";
    char camera_root[64] = "sFestaCamera";
    char key_lmb[48] = "Z";
    char key_rmb[48] = "E";
    char key_mmb[48] = "LCTRL";
    char key_x1[48] = "LSHIFT";
    char key_x2[48] = "C";
    char wheel_up[48] = "Q";
    char wheel_down[48] = "E";
};

struct AngleField {
    uint32_t offset = 0;
    double target = 0.0;
};

static Settings g_settings;
static ForgeConfig* g_config = nullptr;
static std::atomic<bool> g_active{false};
static std::atomic<bool> g_focus_write_active{false};
static bool g_previous_toggle = false;
static int g_toggle_vk = VK_F8;
static bool g_was_focused = false;

static uint32_t g_camera_root = 0;
static uint32_t g_camera_object = 0;
static uint32_t g_camera_path_offset = 0;
static unsigned long long g_camera_seen_tick = 0;
static bool g_camera_initialized = false;
static bool g_angle_initialized = false;
static Vec3 g_base_radial;
static double g_yaw_offset = 0.0;
static double g_pitch_offset = 0.0;
static float g_camera_refresh_timer = 0.0f;
static std::vector<AngleField> g_yaw_fields;
static std::vector<AngleField> g_pitch_fields;

static POINT g_last_cursor{};
static bool g_have_last_cursor = false;
static bool g_cursor_hidden = false;
static bool g_cursor_clipped = false;
static RECT g_saved_clip{};
static bool g_have_saved_clip = false;
static HCURSOR g_blank_cursor = nullptr;
static HCURSOR g_saved_class_cursor = nullptr;
static bool g_saved_class_cursor_valid = false;
static HWND g_subclass_window = nullptr;
static WNDPROC g_original_window_proc = nullptr;
static ForgeHook* g_camera_write_hooks[2] = {nullptr, nullptr};
static std::mutex g_camera_mutex;
static constexpr int kMouseButtonCount = 5;
static std::atomic<int> g_mouse_down[kMouseButtonCount];
static std::atomic<int> g_mouse_up[kMouseButtonCount];
static std::atomic<int> g_wheel_up_events{0};
static std::atomic<int> g_wheel_down_events{0};
static bool g_button_held[kMouseButtonCount]{};
static bool g_button_release_pending[kMouseButtonCount]{};
static float g_button_hold_remaining[kMouseButtonCount]{};
static int g_button_vks[kMouseButtonCount][4]{};
static int g_button_vk_count[kMouseButtonCount]{};
static int g_wheel_vk[2]{};
static int g_active_wheel_vk = 0;
static float g_active_wheel_remaining = 0.0f;
static uint8_t g_key_refs[256]{};
static bool g_raw_mouse_registered = false;

static void ResetCameraMotion();
static bool WriteAngleTargets();
static bool ApplyCameraVectorToObject(uint32_t object);

static double VecLength(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

static Vec3 Normalize(const Vec3& value) {
    double length = VecLength(value);
    if (length <= 0.000001) return {};
    return {value.x / length, value.y / length, value.z / length};
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

static int VkFromName(const char* name) {
    if (!name || !*name) return 0;
    if (!_stricmp(name, "NONE") || !strcmp(name, "-") || !strcmp(name, "0")) return 0;
    if (!_stricmp(name, "LSHIFT")) return VK_LSHIFT;
    if (!_stricmp(name, "RSHIFT")) return VK_RSHIFT;
    if (!_stricmp(name, "SHIFT")) return VK_SHIFT;
    if (!_stricmp(name, "LCTRL")) return VK_LCONTROL;
    if (!_stricmp(name, "RCTRL")) return VK_RCONTROL;
    if (!_stricmp(name, "CTRL")) return VK_CONTROL;
    if (!_stricmp(name, "SPACE")) return VK_SPACE;
    if (!_stricmp(name, "ENTER") || !_stricmp(name, "RETURN")) return VK_RETURN;
    if (!_stricmp(name, "TAB")) return VK_TAB;
    if (!_stricmp(name, "ESC") || !_stricmp(name, "ESCAPE")) return VK_ESCAPE;
    if (!_stricmp(name, "F6")) return VK_F6;
    if (!_stricmp(name, "F7")) return VK_F7;
    if (!_stricmp(name, "F8")) return VK_F8;
    if (!_stricmp(name, "F9")) return VK_F9;
    if (!_stricmp(name, "F6")) return VK_F6;
    if (!_stricmp(name, "F10")) return VK_F10;
    if (!_stricmp(name, "INSERT")) return VK_INSERT;
    if (!_stricmp(name, "HOME")) return VK_HOME;
    if (!_stricmp(name, "END")) return VK_END;
    if (name[0] >= 'A' && name[0] <= 'Z' && !name[1]) return name[0];
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) return name[0];
    return 0;
}

static int ParseVkList(const char* text, int output[4]) {
    int count = 0;
    if (!text) return 0;
    const char* cursor = text;
    while (*cursor && count < 4) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        if (!*cursor) break;
        char token[32]{};
        int length = 0;
        while (*cursor && *cursor != ',' && *cursor != ' ' && *cursor != '\t' &&
               length < (int)sizeof(token) - 1) {
            token[length++] = *cursor++;
        }
        token[length] = 0;
        int vk = VkFromName(token);
        if (vk) output[count++] = vk;
    }
    return count;
}

static void CopyConfigString(const char* section, const char* key, char* output,
                             size_t capacity, const char* fallback) {
    if (!g_config || !forge_api->config_string || !output || capacity == 0) return;
    const char* value =
        forge_api->config_string(g_config, section, key, fallback);
    snprintf(output, capacity, "%s", value ? value : fallback);
}

static void ParseOffsetList(const char* text, std::vector<AngleField>* output) {
    output->clear();
    if (!text) return;
    const char* cursor = text;
    while (*cursor && output->size() < 32) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        if (!*cursor) break;
        char* end = nullptr;
        unsigned long value = strtoul(cursor, &end, 0);
        if (end == cursor) break;
        if (value <= 0xFFCu) output->push_back({(uint32_t)value, 0.0});
        cursor = end;
    }
}

static void LoadSettings() {
    if (!g_config) return;
    g_settings.sensitivity_x =
        forge_api->config_float(g_config, "mouse", "sens_x_deg_per_count", 0.06f);
    g_settings.sensitivity_y =
        forge_api->config_float(g_config, "mouse", "sens_y_deg_per_count", 0.06f);
    g_settings.invert_x =
        forge_api->config_bool(g_config, "mouse", "invert_x", true);
    g_settings.invert_y =
        forge_api->config_bool(g_config, "mouse", "invert_y", true);
    g_settings.hide_cursor =
        forge_api->config_bool(g_config, "mouse", "hide_cursor", true);
    g_settings.center_cursor =
        forge_api->config_bool(g_config, "mouse", "center_cursor", true);
    g_settings.button_hold_ms =
        std::clamp(forge_api->config_int(g_config, "mouse", "button_hold_ms", 100),
                   1, 1000);
    g_settings.wheel_hold_ms =
        std::clamp(forge_api->config_int(g_config, "mouse", "wheel_hold_ms", 100),
                   1, 1000);
    g_settings.angle_mode =
        forge_api->config_bool(g_config, "camera", "angle_mode", false);
    g_settings.settle_ms =
        std::clamp(forge_api->config_int(g_config, "mouse", "settle_ms", 1500),
                   250, 5000);
    g_settings.units_per_turn =
        forge_api->config_float(g_config, "camera", "units_per_turn", 65536.0f);
    g_settings.pitch_min =
        forge_api->config_float(g_config, "camera", "pitch_min", -17000.0f);
    g_settings.pitch_max =
        forge_api->config_float(g_config, "camera", "pitch_max", 17000.0f);
    g_settings.yaw_direction =
        forge_api->config_int(g_config, "camera", "yaw_direction", -1) < 0 ? -1 : 1;
    g_settings.pitch_direction =
        forge_api->config_int(g_config, "camera", "pitch_direction", -1) < 0 ? -1 : 1;
    CopyConfigString("mouse", "toggle_key", g_settings.toggle_key,
                     sizeof(g_settings.toggle_key), "F8");
    CopyConfigString("camera", "root", g_settings.camera_root,
                     sizeof(g_settings.camera_root), "sFestaCamera");
    CopyConfigString("camera", "yaw_offsets", g_settings.yaw_offsets,
                     sizeof(g_settings.yaw_offsets), "0xAE,0xEA,0xD2");
    CopyConfigString("camera", "pitch_offsets", g_settings.pitch_offsets,
                     sizeof(g_settings.pitch_offsets), "0x196");
    CopyConfigString("keys", "left", g_settings.key_lmb,
                     sizeof(g_settings.key_lmb), "Z");
    CopyConfigString("keys", "right", g_settings.key_rmb,
                     sizeof(g_settings.key_rmb), "E");
    CopyConfigString("keys", "middle", g_settings.key_mmb,
                     sizeof(g_settings.key_mmb), "LCTRL");
    CopyConfigString("keys", "x1", g_settings.key_x1,
                     sizeof(g_settings.key_x1), "LSHIFT");
    CopyConfigString("keys", "x2", g_settings.key_x2,
                     sizeof(g_settings.key_x2), "C");
    CopyConfigString("keys", "wheel_up", g_settings.wheel_up,
                     sizeof(g_settings.wheel_up), "Q");
    CopyConfigString("keys", "wheel_down", g_settings.wheel_down,
                     sizeof(g_settings.wheel_down), "E");
    ParseOffsetList(g_settings.yaw_offsets, &g_yaw_fields);
    ParseOffsetList(g_settings.pitch_offsets, &g_pitch_fields);
    g_button_vk_count[0] = ParseVkList(g_settings.key_lmb, g_button_vks[0]);
    g_button_vk_count[1] = ParseVkList(g_settings.key_mmb, g_button_vks[1]);
    g_button_vk_count[2] = ParseVkList(g_settings.key_rmb, g_button_vks[2]);
    g_button_vk_count[3] = ParseVkList(g_settings.key_x1, g_button_vks[3]);
    g_button_vk_count[4] = ParseVkList(g_settings.key_x2, g_button_vks[4]);
    g_wheel_vk[0] = VkFromName(g_settings.wheel_up);
    g_wheel_vk[1] = VkFromName(g_settings.wheel_down);
    g_toggle_vk = VkFromName(g_settings.toggle_key);
    if (!g_toggle_vk) g_toggle_vk = VK_F8;
}

static bool GameIsFocused() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    HWND root = GetAncestor(foreground, GA_ROOT);
    if (!root) root = foreground;
    DWORD process_id = 0;
    GetWindowThreadProcessId(root, &process_id);
    return process_id == GetCurrentProcessId() && IsWindowVisible(root);
}

static bool ClientCenter(POINT* center) {
    if (!center) return false;
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    HWND root = GetAncestor(foreground, GA_ROOT);
    if (root) foreground = root;
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    if (process_id != GetCurrentProcessId()) return false;
    RECT client{};
    if (!GetClientRect(foreground, &client)) return false;
    POINT point{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
    if (!ClientToScreen(foreground, &point)) return false;
    *center = point;
    return true;
}

static bool IsExtendedKey(int vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
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
                       (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
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

static void PressList(const int* keys, int count, bool down) {
    if (down) {
        for (int i = 0; i < count; ++i) RefKey(keys[i], true);
    } else {
        for (int i = count - 1; i >= 0; --i) RefKey(keys[i], false);
    }
}

static void ReleaseAllMouseActions() {
    for (int vk = 1; vk < 256; ++vk) {
        while (g_key_refs[vk]) RefKey(vk, false);
    }
    for (int i = 0; i < kMouseButtonCount; ++i) {
        g_button_held[i] = false;
        g_button_release_pending[i] = false;
        g_button_hold_remaining[i] = 0.0f;
        g_mouse_down[i].store(0);
        g_mouse_up[i].store(0);
    }
    g_active_wheel_vk = 0;
    g_active_wheel_remaining = 0.0f;
    g_wheel_up_events.store(0);
    g_wheel_down_events.store(0);
}

static void ProcessRawInput(LPARAM lparam) {
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 ||
        size == 0 || size > 256 * 1024)
        return;
    std::vector<BYTE> buffer(size);
    if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buffer.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;
    RAWINPUT* raw = (RAWINPUT*)buffer.data();
    if (raw->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& mouse = raw->data.mouse;
    struct Event {
        USHORT down;
        USHORT up;
        int index;
    };
    static const Event events[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 1},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4},
    };
    for (const Event& event : events) {
        if (mouse.usButtonFlags & event.down) g_mouse_down[event.index] += 1;
        if (mouse.usButtonFlags & event.up) g_mouse_up[event.index] += 1;
    }
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
        SHORT delta = (SHORT)mouse.usButtonData;
        if (delta > 0)
            g_wheel_up_events.fetch_add(1);
        else if (delta < 0)
            g_wheel_down_events.fetch_add(1);
    }
}

static bool RegisterRawMouse(bool enabled) {
    HWND foreground = GetForegroundWindow();
    HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    if (enabled && !root) return false;
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = enabled ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    device.hwndTarget = enabled ? root : nullptr;
    bool ok = RegisterRawInputDevices(&device, 1, sizeof(device)) != 0;
    g_raw_mouse_registered = ok && enabled;
    return ok;
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
    if (message == WM_INPUT && g_active.load(std::memory_order_relaxed))
        ProcessRawInput(lparam);
    if (message == WM_SETCURSOR && g_active.load(std::memory_order_relaxed)) {
        SetCursor(g_blank_cursor);
        return TRUE;
    }
    if (g_original_window_proc)
        return CallWindowProcW(g_original_window_proc, window, message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

static void RemoveCursorSubclass() {
    if (g_subclass_window && g_original_window_proc && IsWindow(g_subclass_window)) {
        SetWindowLongPtrW(g_subclass_window, GWLP_WNDPROC,
                          (LONG_PTR)g_original_window_proc);
    }
    if (g_saved_class_cursor_valid && g_subclass_window &&
        IsWindow(g_subclass_window)) {
        SetClassLongPtrW(g_subclass_window, GCLP_HCURSOR,
                         (LONG_PTR)g_saved_class_cursor);
    }
    g_subclass_window = nullptr;
    g_original_window_proc = nullptr;
    g_saved_class_cursor_valid = false;
}

static void InstallCursorSubclass(HWND window) {
    if (!window || !IsWindow(window) || g_subclass_window == window) return;
    RemoveCursorSubclass();
    if (!g_blank_cursor) g_blank_cursor = CreateBlankCursor();
    g_saved_class_cursor = (HCURSOR)GetClassLongPtrW(window, GCLP_HCURSOR);
    g_saved_class_cursor_valid = true;
    SetClassLongPtrW(window, GCLP_HCURSOR, (LONG_PTR)g_blank_cursor);
    SetLastError(0);
    WNDPROC previous =
        (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)CursorWindowProc);
    if (!previous && GetLastError()) {
        RemoveCursorSubclass();
        return;
    }
    g_subclass_window = window;
    g_original_window_proc = previous;
}

static void SetCursorCapture(bool active) {
    if (active) {
        if (!g_have_saved_clip) {
            g_have_saved_clip = GetClipCursor(&g_saved_clip) != 0;
        }
        if (g_settings.hide_cursor && !g_cursor_hidden) {
            ShowCursor(FALSE);
            g_cursor_hidden = true;
        }
        HWND foreground = GetForegroundWindow();
        HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (root && g_settings.hide_cursor) InstallCursorSubclass(root);
        if (root && g_settings.center_cursor) {
            RECT client{};
            if (GetClientRect(root, &client)) {
                POINT top_left{client.left, client.top};
                POINT bottom_right{client.right, client.bottom};
                ClientToScreen(root, &top_left);
                ClientToScreen(root, &bottom_right);
                RECT clip{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
                ClipCursor(&clip);
                g_cursor_clipped = true;
            }
        }
        SetCursor(g_settings.hide_cursor && g_blank_cursor ? g_blank_cursor : nullptr);
        return;
    }

    RemoveCursorSubclass();
    if (g_cursor_hidden) {
        ShowCursor(TRUE);
        g_cursor_hidden = false;
    }
    if (g_cursor_clipped) {
        ClipCursor(g_have_saved_clip ? &g_saved_clip : nullptr);
        g_cursor_clipped = false;
    }
    SetCursor(LoadCursorA(nullptr, IDC_ARROW));
    g_have_last_cursor = false;
}

static bool ReadVec3(uint32_t object, uint32_t offset, Vec3* value) {
    if (!object || !value || !forge_api->mem_isValid(object + offset, 12)) return false;
    float raw[3]{};
    if (!forge_api->mem_read(object + offset, raw, sizeof(raw))) return false;
    if (!std::isfinite(raw[0]) || !std::isfinite(raw[1]) || !std::isfinite(raw[2]))
        return false;
    if (std::fabs(raw[0]) > 1000000.0f || std::fabs(raw[1]) > 1000000.0f ||
        std::fabs(raw[2]) > 1000000.0f)
        return false;
    value->x = raw[0];
    value->y = raw[1];
    value->z = raw[2];
    return true;
}

static bool WriteVec3(uint32_t object, uint32_t offset, const Vec3& value) {
    if (!object || !forge_api->mem_isValid(object + offset, 12)) return false;
    float raw[3] = {(float)value.x, (float)value.y, (float)value.z};
    return forge_api->mem_write(object + offset, raw, sizeof(raw));
}

static const char* ObjectClassName(uint32_t object) {
    if (!object || !forge_api->dti_of || !forge_api->dti_name) return nullptr;
    uint32_t dti = forge_api->dti_of(object);
    return dti ? forge_api->dti_name(dti) : nullptr;
}

static bool FindQuestCameraInRoot(uint32_t root, uint32_t* object,
                                  uint32_t* path_offset) {
    if (!root || !object || !path_offset || !forge_api->mem_isValid(root, kRootScanBytes))
        return false;
    std::vector<uint32_t> words(kRootScanBytes / 4);
    if (!forge_api->mem_read(root, words.data(), kRootScanBytes)) return false;

    uint32_t fallback_object = 0;
    uint32_t fallback_offset = 0;
    for (uint32_t index = 0; index < words.size(); ++index) {
        uint32_t pointer = words[index];
        if (pointer < 0x30000000u || pointer >= 0x80000000u) continue;
        if (!forge_api->mem_isValid(pointer, 0xAC0)) continue;
        const char* class_name = ObjectClassName(pointer);
        if (!class_name) continue;
        uint32_t offset = index * 4;
        if (!strcmp(class_name, "uQuestCamera")) {
            Vec3 position;
            Vec3 target;
            if (ReadVec3(pointer, 0x40, &position) && ReadVec3(pointer, 0x60, &target) &&
                VecLength({position.x - target.x, position.y - target.y,
                           position.z - target.z}) > 0.01) {
                *object = pointer;
                *path_offset = offset;
                return true;
            }
            if (!fallback_object) {
                fallback_object = pointer;
                fallback_offset = offset;
            }
        }
    }
    if (fallback_object) {
        *object = fallback_object;
        *path_offset = fallback_offset;
        return true;
    }
    return false;
}

static void ResetCameraMotion() {
    g_camera_initialized = false;
    g_base_radial = {};
    g_yaw_offset = 0.0;
    g_pitch_offset = 0.0;
    g_angle_initialized = false;
}

static void RefreshCameraObject() {
    if (!forge_api->singleton_getByName) return;
    uint32_t root = forge_api->singleton_getByName(g_settings.camera_root);
    if (!root) {
        if (g_camera_root) LOGW("camera singleton %s is unavailable", g_settings.camera_root);
        g_camera_root = 0;
        g_camera_object = 0;
        ResetCameraMotion();
        return;
    }

    if (root != g_camera_root) {
        LOGI("camera singleton resolved at 0x%08X", root);
        g_camera_root = root;
        g_camera_path_offset = 0;
    }

    uint32_t object = 0;
    uint32_t path_offset = 0;
    if (!FindQuestCameraInRoot(root, &object, &path_offset)) {
        if (g_camera_object)
            LOGI("uQuestCamera is no longer present; waiting for gameplay");
        g_camera_object = 0;
        g_camera_path_offset = 0;
        ResetCameraMotion();
        return;
    }

    if (object != g_camera_object || path_offset != g_camera_path_offset) {
        g_camera_object = object;
        g_camera_path_offset = path_offset;
        g_camera_seen_tick = GetTickCount64();
        ResetCameraMotion();
        LOGI("uQuestCamera found at root+0x%X -> 0x%08X", path_offset, object);
    }
}

static bool InitializeCameraMotion() {
    if (!g_camera_object) return false;
    const char* class_name = ObjectClassName(g_camera_object);
    if (!class_name || strcmp(class_name, "uQuestCamera") != 0) return false;
    unsigned long long since = g_camera_seen_tick;
    if (!since || GetTickCount64() - since < (unsigned long long)g_settings.settle_ms)
        return false;

    Vec3 position;
    Vec3 target;
    if (!ReadVec3(g_camera_object, 0x40, &position) ||
        !ReadVec3(g_camera_object, 0x60, &target))
        return false;
    g_base_radial =
        Vec3{position.x - target.x, position.y - target.y, position.z - target.z};
    double distance = VecLength(g_base_radial);
    if (distance < 0.01 || distance > 10000.0) return false;

    g_yaw_offset = 0.0;
    g_pitch_offset = 0.0;
    g_camera_initialized = true;
    LOGI("camera direct-write locked: eye %.2f %.2f %.2f target %.2f %.2f %.2f",
         position.x, position.y, position.z, target.x, target.y, target.z);
    return true;
}

static bool ReadAngle(uint32_t root, uint32_t offset, double* value) {
    if (!root || !value || !forge_api->mem_isValid(root + offset, 2)) return false;
    int16_t raw = 0;
    if (!forge_api->mem_read(root + offset, &raw, sizeof(raw))) return false;
    *value = raw;
    return true;
}

static bool WriteAngle(uint32_t root, uint32_t offset, double value) {
    if (!root || !forge_api->mem_isValid(root + offset, 2)) return false;
    int16_t raw = (int16_t)llround(value);
    return forge_api->mem_write(root + offset, &raw, sizeof(raw));
}

static bool InitializeAngleMotion() {
    if (!g_camera_root || !g_camera_object || g_yaw_fields.empty() ||
        g_pitch_fields.empty())
        return false;
    unsigned long long since = g_camera_seen_tick;
    if (!since || GetTickCount64() - since < (unsigned long long)g_settings.settle_ms)
        return false;
    int yaw_ready = 0;
    int pitch_ready = 0;
    for (AngleField& field : g_yaw_fields) {
        if (ReadAngle(g_camera_root, field.offset, &field.target)) ++yaw_ready;
    }
    for (AngleField& field : g_pitch_fields) {
        if (ReadAngle(g_camera_root, field.offset, &field.target)) ++pitch_ready;
    }
    if (!yaw_ready || !pitch_ready) return false;
    g_angle_initialized = true;
    LOGI("angle fields locked: %d yaw, %d pitch, units %.3f",
         yaw_ready, pitch_ready, (double)g_settings.units_per_turn);
    return true;
}

static void UpdateAngleTargets(long long dx, long long dy) {
    if (!g_angle_initialized) return;
    double units = std::max(1.0f, g_settings.units_per_turn);
    double yaw_delta = (double)dx * g_settings.sensitivity_x * units / 360.0;
    double pitch_delta = (double)dy * g_settings.sensitivity_y * units / 360.0;
    if (g_settings.invert_x) yaw_delta = -yaw_delta;
    if (g_settings.invert_y) pitch_delta = -pitch_delta;
    yaw_delta *= g_settings.yaw_direction;
    pitch_delta *= g_settings.pitch_direction;

    for (AngleField& field : g_yaw_fields) {
        double next = field.target + yaw_delta;
        while (next > 32767.0) next -= units;
        while (next < -32768.0) next += units;
        field.target = next;
    }
    for (AngleField& field : g_pitch_fields) {
        field.target = std::clamp(field.target + pitch_delta,
                                  (double)g_settings.pitch_min,
                                  (double)g_settings.pitch_max);
    }
}

static bool WriteAngleTargets() {
    if (!g_angle_initialized || !g_camera_root) return false;
    int written = 0;
    for (const AngleField& field : g_yaw_fields)
        if (WriteAngle(g_camera_root, field.offset, field.target)) ++written;
    for (const AngleField& field : g_pitch_fields)
        if (WriteAngle(g_camera_root, field.offset, field.target)) ++written;
    return written > 0;
}

static void AddMouseDelta(long long dx, long long dy) {
    if (!dx && !dy) return;
    double yaw_step =
        (double)dx * g_settings.sensitivity_x * kPi / 180.0;
    double pitch_step =
        (double)dy * g_settings.sensitivity_y * kPi / 180.0;
    if (g_settings.invert_x) yaw_step = -yaw_step;
    if (g_settings.invert_y) pitch_step = -pitch_step;
    g_yaw_offset += yaw_step;
    g_pitch_offset =
        std::clamp(g_pitch_offset + pitch_step, -kPitchLimit, kPitchLimit);
}

static bool ApplyCameraVectorToObject(uint32_t object) {
    if (!g_camera_initialized || !object) return false;
    if (!forge_api->mem_isValid(object + 0x40, 0xAC0)) return false;
    const char* class_name = ObjectClassName(object);
    if (!class_name || strcmp(class_name, "uQuestCamera") != 0) return false;

    Vec3 target;
    if (!ReadVec3(object, 0x60, &target)) return false;

    Vec3 radial = RotateY(g_base_radial, g_yaw_offset);
    Vec3 forward = Normalize({-radial.x, -radial.y, -radial.z});
    if (VecLength(forward) < 0.5) return false;

    Vec3 right = Normalize({-forward.z, 0.0, forward.x});
    if (VecLength(right) < 0.5) return false;
    radial = RotateAxis(radial, right, g_pitch_offset);

    Vec3 position{target.x + radial.x, target.y + radial.y, target.z + radial.z};
    Vec3 final_forward = Normalize({target.x - position.x, target.y - position.y,
                                    target.z - position.z});
    if (VecLength(final_forward) < 0.5) return false;
    Vec3 final_right = Normalize({-final_forward.z, 0.0, final_forward.x});
    if (VecLength(final_right) < 0.5) return false;
    Vec3 up = Normalize({final_right.y * final_forward.z -
                             final_right.z * final_forward.y,
                         final_right.z * final_forward.x -
                             final_right.x * final_forward.z,
                         final_right.x * final_forward.y -
                             final_right.y * final_forward.x});
    if (VecLength(up) < 0.5) return false;

    bool wrote_position = WriteVec3(object, 0x40, position);
    bool wrote_up = WriteVec3(object, 0x50, up);
    return wrote_position && wrote_up;
}

static ForgeHookAction CameraWritePre(ForgeCpu* cpu, void* user) {
    if (!g_active.load(std::memory_order_relaxed) ||
        !g_focus_write_active.load(std::memory_order_relaxed))
        return FORGE_HOOK_CONTINUE;
    std::lock_guard<std::mutex> lock(g_camera_mutex);
    if (g_settings.angle_mode) {
        WriteAngleTargets();
    } else {
        ApplyCameraVectorToObject(cpu->r[1]);
    }
    return FORGE_HOOK_CONTINUE;
}

static bool EnsureCameraWriteHook() {
    if (g_camera_write_hooks[0] && g_camera_write_hooks[1]) return true;
    if (!g_camera_object || !forge_api->hook_create) return false;
    uint32_t base =
        forge_api->mem_getModuleBase ? forge_api->mem_getModuleBase("main") : 0;
    if (!base) return false;
    const uint32_t offsets[2] = {0x1C33C, 0x1C358};
    for (int i = 0; i < 2; ++i) {
        if (g_camera_write_hooks[i]) continue;
        g_camera_write_hooks[i] = forge_api->hook_create(
            base + offsets[i], CameraWritePre, nullptr, nullptr);
        LOGI("camera write hook main+0x%X: %s", offsets[i],
             g_camera_write_hooks[i] ? "installed" : "failed");
    }
    return g_camera_write_hooks[0] && g_camera_write_hooks[1];
}

static void SampleCursorDelta(long long* dx, long long* dy) {
    *dx = 0;
    *dy = 0;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    if (g_have_last_cursor) {
        *dx = (long long)cursor.x - g_last_cursor.x;
        *dy = (long long)cursor.y - g_last_cursor.y;
    }

    if (g_settings.center_cursor) {
        POINT center{};
        if (ClientCenter(&center) && SetCursorPos(center.x, center.y)) {
            cursor = center;
        }
    }
    g_last_cursor = cursor;
    g_have_last_cursor = true;
}

static void HandleMouseActions(bool allowed, float dt) {
    if (!allowed) {
        ReleaseAllMouseActions();
        return;
    }
    for (int index = 0; index < kMouseButtonCount; ++index) {
        int downs = g_mouse_down[index].exchange(0);
        int ups = g_mouse_up[index].exchange(0);
        if (downs > 0) {
            if (!g_button_held[index])
                PressList(g_button_vks[index], g_button_vk_count[index], true);
            g_button_held[index] = true;
            g_button_release_pending[index] = false;
            g_button_hold_remaining[index] = g_settings.button_hold_ms / 1000.0f;
        }
        if (ups > 0 && g_button_held[index]) g_button_release_pending[index] = true;
        if (g_button_held[index] && g_button_hold_remaining[index] > 0.0f)
            g_button_hold_remaining[index] -= dt;
        if (g_button_held[index] && g_button_release_pending[index] &&
            g_button_hold_remaining[index] <= 0.0f) {
            PressList(g_button_vks[index], g_button_vk_count[index], false);
            g_button_held[index] = false;
            g_button_release_pending[index] = false;
        }
    }

    if (g_active_wheel_remaining > 0.0f) {
        g_active_wheel_remaining -= dt;
        if (g_active_wheel_remaining <= 0.0f) {
            if (g_active_wheel_vk) RefKey(g_active_wheel_vk, false);
            g_active_wheel_vk = 0;
        }
    }
    if (g_active_wheel_vk) return;
    int up = g_wheel_up_events.load();
    int down = g_wheel_down_events.load();
    int key = 0;
    if (up > 0 && down <= 0) {
        g_wheel_up_events.fetch_sub(1);
        key = g_wheel_vk[0];
    } else if (down > 0) {
        g_wheel_down_events.fetch_sub(1);
        key = g_wheel_vk[1];
    }
    if (key) {
        RefKey(key, true);
        g_active_wheel_vk = key;
        g_active_wheel_remaining = g_settings.wheel_hold_ms / 1000.0f;
    }
}

static void SetActive(bool active) {
    if (g_active == active) return;
    g_active = active;
    g_have_last_cursor = false;
    ResetCameraMotion();
    if (active) {
        POINT center{};
        if (g_settings.center_cursor && ClientCenter(&center) &&
            SetCursorPos(center.x, center.y)) {
            g_last_cursor = center;
            g_have_last_cursor = true;
        }
        SetCursorCapture(true);
        RegisterRawMouse(true);
        LOGI("enabled; waiting for a stable uQuestCamera");
    } else {
        RegisterRawMouse(false);
        ReleaseAllMouseActions();
        SetCursorCapture(false);
        LOGI("disabled; camera memory is no longer written");
    }
}

extern "C" {

FORGE_EXPORT void forge_onLoad(ForgePluginInfo* info) {
    info->required_ver.major = 0;
    info->required_ver.minor = 7;
    info->required_ver.patch = 0;
    snprintf(info->name, sizeof(info->name), "MouseLookDirect");
}

FORGE_EXPORT void forge_onInit(void) {
    if (forge_api->config_open)
        g_config = forge_api->config_open("mouselook_direct.ini");
    LoadSettings();
    g_active = false;
    g_camera_root = 0;
    g_camera_object = 0;
    g_camera_initialized = false;
    LOGI("loaded; no hooks, no camera reads and no mouse capture at startup");
    LOGI("%s enables mouse look only after a stable uQuestCamera appears",
         g_settings.toggle_key);
}

FORGE_EXPORT void forge_onUpdate(float dt) {
    bool focused = GameIsFocused();
    bool toggle_down = focused && (GetAsyncKeyState(g_toggle_vk) & 0x8000) != 0;
    if (toggle_down && !g_previous_toggle) SetActive(!g_active);
    g_previous_toggle = toggle_down;

    if (!focused) {
        g_focus_write_active.store(false, std::memory_order_relaxed);
        if (g_active && g_was_focused) {
            SetCursorCapture(false);
            RegisterRawMouse(false);
            ReleaseAllMouseActions();
            g_have_last_cursor = false;
        }
        g_was_focused = false;
        return;
    }
    g_focus_write_active.store(g_active.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    if (g_active && !g_was_focused) {
        SetCursorCapture(true);
        RegisterRawMouse(true);
        g_have_last_cursor = false;
    }
    g_was_focused = true;
    if (!g_active) return;
    HandleMouseActions(true, dt);

    g_camera_refresh_timer -= dt;
    if (g_camera_refresh_timer <= 0.0f) {
        g_camera_refresh_timer = 0.5f;
        RefreshCameraObject();
    }
    long long dx = 0;
    long long dy = 0;
    SampleCursorDelta(&dx, &dy);

    bool applied = false;
    if (g_settings.angle_mode) {
        if (!g_angle_initialized && !InitializeAngleMotion()) return;
        if (!EnsureCameraWriteHook()) return;
        std::lock_guard<std::mutex> lock(g_camera_mutex);
        UpdateAngleTargets(dx, dy);
        applied = true;
    } else {
        if (!g_camera_object) return;
        if (!g_camera_initialized && !InitializeCameraMotion()) return;
        if (!EnsureCameraWriteHook()) return;
        std::lock_guard<std::mutex> lock(g_camera_mutex);
        AddMouseDelta(dx, dy);
        applied = true;
    }
    if (!applied) {
        static unsigned long long last_failure_log = 0;
        unsigned long long now = GetTickCount64();
        if (now - last_failure_log > 2000) {
            last_failure_log = now;
            LOGW("camera write failed; waiting for a valid quest camera");
        }
    }
}

FORGE_EXPORT void forge_onUnload(void) {
    SetActive(false);
    SetCursorCapture(false);
    RemoveCursorSubclass();
    if (g_blank_cursor) {
        DestroyCursor(g_blank_cursor);
        g_blank_cursor = nullptr;
    }
    g_camera_initialized = false;
    g_angle_initialized = false;
    g_camera_object = 0;
}

} /* extern "C" */
