// EXVS Overlay menu: settings (EXVSOverlay.ini next to the game exe), hotkeys, FPS stats, UI.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include "imgui.h"
#include "imgui_internal.h"   // MovingWindow, to know when a widget is being dragged
#include "menu.h"
#include "framedata.h"

static const int ARCADE_COUNT = 2;   // Card, Coin
static XINPUT_GAMEPAD_ test_pad; static bool test_pad_set;   // EXVSOVERLAY_TEST_PAD="buttons,lx,ly,rx,ry,lt,rt"
static WORD test_pad_b2; static int test_pad_switch = -1;   // EXVSOVERLAY_TEST_PAD2="buttons,frame": other buttons from that frame on (trail test)

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
struct Config {
    bool enabled = true;
    int toggle_vk = VK_INSERT;
    bool hud_enabled = true;       // install the hide-HUD detours at startup
    int hud_vk = VK_F9;            // toggles the battle HUD
    bool pad_toggle = true;        // Back + Start on a controller
    bool block_input = true;       // game ignores keyboard/mouse/controller while the menu is open
    bool ignore_unfocused = true;  // ... and while another window has the focus (browser, Discord, ...)
    bool keep_window = true;       // keep the window size saved in SnowLauncher.ini
    int arcade_button[ARCADE_COUNT] = {};   // Card, Coin: XInput button bit (LT 0x10000, RT 0x20000); 0 = off
    bool card_hide = true;         // the game doesn't see those buttons
    bool show_pad = false;         // controller overlay (input display)
    int pad_corner = 2;            // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    float pad_size = 1.0f;
    float pad_alpha = 0.9f;        // buttons
    float pad_bg = 0.35f;          // background panel
    int pad_labels = 0;            // 0 L1/R1/L2/R2, 1 LB/RB/LT/RT
    int pad_style = 0;             // 0 controller, 1 arcade stick
    float pad_x = -1, pad_y = -1;  // dragged position (fraction of the screen), -1 = use pad_corner
    float fps_x = -1, fps_y = -1;
    bool show_fps = false;
    int fps_corner = 1;         // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    float ui_scale = 1.0f;
    float opacity = 0.94f;
    int up_mode = 0;               // 0 off, 1 AMD FSR 1 (read at startup: the swapchain is taken over then)
    float up_sharp = 0.8f;         // RCAS strength 0..1
    bool auto_update = true;       // check the GitHub releases when the game starts
    bool fd_meter = false;         // Training: frame meter
    bool fd_bones = false;         // Training: live skeleton dots
    bool fd_hurt = false;          // Training: hurtboxes
    bool fd_hit = false;           // Training: hitboxes
    float fdm_x = -1, fdm_y = -1;  // frame meter position (fraction of the screen), -1 = bottom centre
    float menu_x = -1, menu_y = -1; // menu window position (fraction of the screen), -1 = centre
};
static Config cfg;
static char ini_path[MAX_PATH];
static bool force_open;            // EXVSOVERLAY_TEST=1: open at start (used by the test host)

static void ini_get_bool(const char *k, bool *v) { *v = GetPrivateProfileIntA("Overlay", k, *v ? 1 : 0, ini_path) != 0; }
static void ini_get_int(const char *k, int *v) { *v = (int)GetPrivateProfileIntA("Overlay", k, *v, ini_path); }
static void ini_get_float(const char *k, float *v) {
    char def[32], buf[32]; snprintf(def, sizeof(def), "%.2f", *v);
    GetPrivateProfileStringA("Overlay", k, def, buf, sizeof(buf), ini_path);
    *v = (float)atof(buf);
}
static void ini_set(const char *k, const char *fmt, double v) {
    char buf[32]; snprintf(buf, sizeof(buf), fmt, v);
    WritePrivateProfileStringA("Overlay", k, buf, ini_path);
}

static void save_config(void) {
    ini_set("enabled", "%.0f", cfg.enabled);
    ini_set("toggle_key", "%.0f", cfg.toggle_vk);
    ini_set("hide_hud_feature", "%.0f", cfg.hud_enabled);
    ini_set("hide_hud_key", "%.0f", cfg.hud_vk);
    ini_set("pad_toggle", "%.0f", cfg.pad_toggle);
    ini_set("block_input", "%.0f", cfg.block_input);
    ini_set("ignore_unfocused", "%.0f", cfg.ignore_unfocused);
    ini_set("keep_window", "%.0f", cfg.keep_window);
    ini_set("card_button", "%.0f", cfg.arcade_button[0]);
    ini_set("coin_button", "%.0f", cfg.arcade_button[1]);
    ini_set("card_hide", "%.0f", cfg.card_hide);
    ini_set("show_pad", "%.0f", cfg.show_pad);
    ini_set("pad_corner", "%.0f", cfg.pad_corner);
    ini_set("pad_size", "%.2f", cfg.pad_size);
    ini_set("pad_alpha", "%.2f", cfg.pad_alpha);
    ini_set("pad_bg", "%.2f", cfg.pad_bg);
    ini_set("pad_labels", "%.0f", cfg.pad_labels);
    ini_set("pad_style", "%.0f", cfg.pad_style);
    ini_set("pad_x", "%.4f", cfg.pad_x);
    ini_set("pad_y", "%.4f", cfg.pad_y);
    ini_set("fps_x", "%.4f", cfg.fps_x);
    ini_set("fps_y", "%.4f", cfg.fps_y);
    ini_set("show_fps", "%.0f", cfg.show_fps);
    ini_set("fps_corner", "%.0f", cfg.fps_corner);
    ini_set("ui_scale", "%.2f", cfg.ui_scale);
    ini_set("opacity", "%.2f", cfg.opacity);
    ini_set("upscaler", "%.0f", cfg.up_mode);
    ini_set("sharpness", "%.2f", cfg.up_sharp);
    ini_set("auto_update", "%.0f", cfg.auto_update);
    ini_set("frame_meter", "%.0f", cfg.fd_meter);
    ini_set("skeleton_view", "%.0f", cfg.fd_bones);
    ini_set("hurtboxes", "%.0f", cfg.fd_hurt);
    ini_set("hitboxes", "%.0f", cfg.fd_hit);
    ini_set("frame_meter_x", "%.4f", cfg.fdm_x);
    ini_set("frame_meter_y", "%.4f", cfg.fdm_y);
    ini_set("menu_x", "%.4f", cfg.menu_x);
    ini_set("menu_y", "%.4f", cfg.menu_y);
}

void menu_load_config(void) {
    snprintf(ini_path, sizeof(ini_path), "%sEXVSOverlay.ini", ov_dir());
    bool existed = GetFileAttributesA(ini_path) != INVALID_FILE_ATTRIBUTES;
    ini_get_bool("enabled", &cfg.enabled);
    ini_get_int("toggle_key", &cfg.toggle_vk);
    ini_get_bool("hide_hud_feature", &cfg.hud_enabled);
    ini_get_int("hide_hud_key", &cfg.hud_vk);
    if (cfg.hud_vk <= 0 || cfg.hud_vk > 0xFE) cfg.hud_vk = VK_F9;
    ini_get_bool("pad_toggle", &cfg.pad_toggle);
    ini_get_bool("block_input", &cfg.block_input);
    ini_get_bool("ignore_unfocused", &cfg.ignore_unfocused);
    ini_get_bool("keep_window", &cfg.keep_window);
    ini_get_int("card_button", &cfg.arcade_button[0]);
    ini_get_int("coin_button", &cfg.arcade_button[1]);
    ini_get_bool("card_hide", &cfg.card_hide);
    for (int &b : cfg.arcade_button)
        if (b < 0 || b > 0x20000 || (b & (b - 1))) b = 0;
    ini_get_bool("show_pad", &cfg.show_pad);
    ini_get_int("pad_corner", &cfg.pad_corner);
    ini_get_float("pad_size", &cfg.pad_size);
    ini_get_float("pad_alpha", &cfg.pad_alpha);
    ini_get_float("pad_bg", &cfg.pad_bg);
    ini_get_int("pad_labels", &cfg.pad_labels);
    if (cfg.pad_corner < 0 || cfg.pad_corner > 3) cfg.pad_corner = 2;
    if (cfg.pad_size < 0.4f || cfg.pad_size > 3.0f) cfg.pad_size = 1.0f;
    if (cfg.pad_alpha < 0.2f || cfg.pad_alpha > 1.0f) cfg.pad_alpha = 0.9f;
    if (cfg.pad_bg < 0.0f || cfg.pad_bg > 1.0f) cfg.pad_bg = 0.35f;
    cfg.pad_labels = cfg.pad_labels ? 1 : 0;
    ini_get_int("pad_style", &cfg.pad_style);
    cfg.pad_style = cfg.pad_style ? 1 : 0;
    {
        char tp[96];
        if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_PAD", tp, sizeof(tp))) {
            int v[7] = {};
            sscanf(tp, "%i,%i,%i,%i,%i,%i,%i", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
            test_pad = { (WORD)v[0], (BYTE)v[5], (BYTE)v[6], (SHORT)v[1], (SHORT)v[2], (SHORT)v[3], (SHORT)v[4] };
            test_pad_set = true; cfg.show_pad = true;
        }
        if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_PAD2", tp, sizeof(tp))) { int b2 = 0, fr = -1; sscanf(tp, "%i,%i", &b2, &fr); test_pad_b2 = (WORD)b2; test_pad_switch = fr; }
    }
    ini_get_float("pad_x", &cfg.pad_x); ini_get_float("pad_y", &cfg.pad_y);
    ini_get_float("fps_x", &cfg.fps_x); ini_get_float("fps_y", &cfg.fps_y);
    if (cfg.pad_x < 0 || cfg.pad_x > 1 || cfg.pad_y < 0 || cfg.pad_y > 1) cfg.pad_x = cfg.pad_y = -1;
    if (cfg.fps_x < 0 || cfg.fps_x > 1 || cfg.fps_y < 0 || cfg.fps_y > 1) cfg.fps_x = cfg.fps_y = -1;
    ini_get_bool("show_fps", &cfg.show_fps);
    ini_get_int("fps_corner", &cfg.fps_corner);
    ini_get_float("ui_scale", &cfg.ui_scale);
    ini_get_float("opacity", &cfg.opacity);
    ini_get_int("upscaler", &cfg.up_mode);
    ini_get_float("sharpness", &cfg.up_sharp);
    ini_get_bool("auto_update", &cfg.auto_update);
    ini_get_bool("frame_meter", &cfg.fd_meter);
    ini_get_bool("skeleton_view", &cfg.fd_bones);
    ini_get_bool("hurtboxes", &cfg.fd_hurt);
    ini_get_bool("hitboxes", &cfg.fd_hit);
    ini_get_float("frame_meter_x", &cfg.fdm_x); ini_get_float("frame_meter_y", &cfg.fdm_y);
    if (cfg.fdm_x < 0 || cfg.fdm_x > 1 || cfg.fdm_y < 0 || cfg.fdm_y > 1) cfg.fdm_x = cfg.fdm_y = -1;
    ini_get_float("menu_x", &cfg.menu_x); ini_get_float("menu_y", &cfg.menu_y);
    if (cfg.menu_x < 0 || cfg.menu_x > 1 || cfg.menu_y < 0 || cfg.menu_y > 1) cfg.menu_x = cfg.menu_y = -1;
    cfg.up_mode = cfg.up_mode == 1 ? 1 : 0;
    if (!(cfg.up_sharp >= 0.0f && cfg.up_sharp <= 1.0f)) cfg.up_sharp = 0.8f;
    if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_UPSCALE", NULL, 0)) cfg.up_mode = 1;   // test host
    if (cfg.toggle_vk <= 0 || cfg.toggle_vk > 0xFE) cfg.toggle_vk = VK_INSERT;
    if (cfg.fps_corner < 0 || cfg.fps_corner > 3) cfg.fps_corner = 1;
    if (cfg.ui_scale < 0.5f || cfg.ui_scale > 3.0f) cfg.ui_scale = 1.0f;
    if (cfg.opacity < 0.3f || cfg.opacity > 1.0f) cfg.opacity = 0.94f;
    if (!existed) save_config();   // write defaults so players can see/edit them
    char env[8];
    force_open = GetEnvironmentVariableA("EXVSOVERLAY_TEST", env, sizeof(env)) && env[0] == '1';
    ov_log("config: %s (toggle_key %d, pad_toggle %d, show_fps %d, upscaler %d)", ini_path, cfg.toggle_vk, cfg.pad_toggle, cfg.show_fps, cfg.up_mode);
}

int menu_upscale_mode(void) { return cfg.up_mode; }
bool menu_auto_update(void) { return cfg.auto_update; }
float menu_upscale_sharpness(void) { return cfg.up_sharp; }

bool menu_enabled(void) { return cfg.enabled; }

// ---------------------------------------------------------------------------------------------
// State: open/close, timing, hotkeys
// ---------------------------------------------------------------------------------------------
static volatile bool menu_open;
static bool rebinding;
static bool res_reload_pending = true;
static void restarted_window_check(void);
static char gpu_name[128] = "?";

static const int FT_COUNT = 240;
static float frametimes[FT_COUNT]; static int ft_pos;
static LARGE_INTEGER qpf, last_present;
static float fps_display; static double fps_accum_t; static int fps_accum_n;

// controller polling (disconnected pads are slow to query, so retry them only every 2 s)
// Slots 0-3: XInput pads. Slot 4 (HID_PAD): the DirectInput pad the loader reads (arcade sticks in
// PS3/PS4 mode), recorded by input_focus.cpp. The card button only uses the XInput slots.
static const int PAD_SLOTS = 5, HID_PAD = 4;
static bool pad_connected[PAD_SLOTS]; static DWORD pad_retry_at[4];
static XINPUT_STATE_ pad_state[PAD_SLOTS]; static int active_pad = -1;
static bool prev_combo, prev_key;
static void card_tick(bool focus);
static bool card_capture_armed;
static DWORD card_pad_resume;   // tick count until which the menu ignores the controller

bool menu_is_open(void) { return menu_open; }
bool menu_should_draw(void) { return menu_open || cfg.show_fps || cfg.show_pad || ((cfg.fd_meter || cfg.fd_bones || cfg.fd_hurt || cfg.fd_hit) && fd_active()); }
bool menu_blocks_input(void) { return menu_open && cfg.block_input; }
void menu_set_gpu_name(const char *n) { strncpy(gpu_name, n, sizeof(gpu_name) - 1); }

static void set_open(bool open) {
    if (open == menu_open) return;
    menu_open = open;
    rebinding = false;
    if (open) res_reload_pending = true;   // re-read the ini: the launcher may have changed it
    if (!open) save_config();
    ov_log("menu %s", open ? "opened" : "closed");
}

static bool game_has_focus(void) {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// What the game may read right now (the loader's keyboard / HID reads and our XInput export ask this,
// many times per frame, from any thread: the focus is looked up at most every 15 ms).
bool menu_game_input_blocked(void) {
    if (menu_open && cfg.block_input) return true;
    if (!cfg.ignore_unfocused) return false;
    static volatile ULONGLONG checked_at; static volatile bool focused = true;
    ULONGLONG now = GetTickCount64();
    if (now - checked_at >= 15) { focused = game_has_focus(); checked_at = now; }
    return !focused;
}

// Splitscreen: only one copy can have the focus, so the focus doesn't matter there. Each copy reads
// its own controller all the time, and the keyboard belongs to one player.
bool menu_keyboard_blocked(void) {
    if (split_active()) return (menu_open && cfg.block_input) || !split_keyboard();
    return menu_game_input_blocked();
}
bool menu_pad_blocked(void) {
    if (split_active()) return menu_open && cfg.block_input;
    return menu_game_input_blocked();
}

static void poll_pads(void) {
    DWORD now = GetTickCount();
    active_pad = -1;
    for (int i = 0; i < 4; i++) {
        if (split_active() && i != split_pad()) { pad_connected[i] = false; continue; }   // another player's pad
        if (!pad_connected[i] && (int)(now - pad_retry_at[i]) < 0) continue;
        XINPUT_STATE_ s;
        if (ov_real_xinput_get_state(i, &s) == ERROR_SUCCESS) {
            pad_connected[i] = true; pad_state[i] = s;
            if (active_pad < 0) active_pad = i;
        } else {
            pad_connected[i] = false; pad_retry_at[i] = now + 2000;
            memset(&pad_state[i], 0, sizeof(pad_state[i]));
        }
    }
    // the DirectInput pad the loader reads (in splitscreen, the one in this copy's config.ini)
    pad_connected[HID_PAD] = (!split_active() || split_pad() < 0) && focus_hid_pad(&pad_state[HID_PAD].Gamepad);
    if (active_pad < 0 && pad_connected[HID_PAD]) active_pad = HID_PAD;
}

static int *rebind_target = &cfg.toggle_vk;   // which key "Change key" is setting
static bool prev_hud_key;
bool menu_hud_enabled(void) { return cfg.hud_enabled; }

static void poll_rebind(void) {
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        if (vk >= VK_SHIFT && vk <= VK_MENU) continue;        // use the left/right variants instead
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;
        if (GetAsyncKeyState(vk) & 1) {
            if (vk != VK_ESCAPE) { *rebind_target = vk; save_config(); }
            rebinding = false;
            prev_key = prev_hud_key = true;                   // don't act on the same press
            return;
        }
    }
}
static void start_rebind(int *target) {
    for (int vk = 0; vk < 256; vk++) GetAsyncKeyState(vk);   // clear the "pressed since last call" bits
    rebind_target = target;
    rebinding = true;
}

void menu_tick_frame(void) {
    fd_update(cfg.fd_meter || cfg.fd_bones || cfg.fd_hurt || cfg.fd_hit);   // Training tools: read the game (no-op when both are off)
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    if (last_present.QuadPart) {
        double dt = (double)(now.QuadPart - last_present.QuadPart) / (double)qpf.QuadPart;
        frametimes[ft_pos] = (float)(dt * 1000.0); ft_pos = (ft_pos + 1) % FT_COUNT;
        fps_accum_t += dt; fps_accum_n++;
        if (fps_accum_t >= 0.5) { fps_display = (float)(fps_accum_n / fps_accum_t); fps_accum_t = 0; fps_accum_n = 0; }
    }
    last_present = now;

    if (force_open) { force_open = false; set_open(true); }
    restarted_window_check();

    bool focus = game_has_focus();
    poll_pads();

    if (rebinding) { if (focus) poll_rebind(); return; }

    bool key = focus && (GetAsyncKeyState(cfg.toggle_vk) & 0x8000);
    if (key && !prev_key) set_open(!menu_open);
    prev_key = key;

    bool hkey = focus && (GetAsyncKeyState(cfg.hud_vk) & 0x8000);
    if (hkey && !prev_hud_key && hud_available()) hud_set_hidden(!hud_hidden());
    prev_hud_key = hkey;

    bool combo = false;
    if (cfg.pad_toggle && active_pad >= 0) {
        const WORD need = 0x0010 /*START*/ | 0x0020 /*BACK*/;
        for (int i = 0; i < PAD_SLOTS; i++)
            if (pad_connected[i] && (pad_state[i].Gamepad.wButtons & need) == need) combo = true;
    }
    // Back + Start must be held for half a second: tapping Back (e.g. as the card button) while holding
    // Start in the game must not toggle the menu.
    static DWORD combo_since; static bool combo_fired;
    if (combo && !prev_combo) { combo_since = GetTickCount(); combo_fired = false; }
    if (combo && !combo_fired && GetTickCount() - combo_since >= 500) { combo_fired = true; set_open(!menu_open); }
    prev_combo = combo;

    card_tick(focus);
}

// ---------------------------------------------------------------------------------------------
// Card reader on a controller button. The loader reads the card key from config.ini [keyboard] Card
// (F3 by default); SnowLauncher drops the controller "Card" mapping for Xbox pads, so the overlay
// presses that key itself while the chosen button is held. Triggers count as buttons (bits 16/17).
// ---------------------------------------------------------------------------------------------
static DWORD pad_bits(const XINPUT_GAMEPAD_ &g) {
    return g.wButtons | (g.bLeftTrigger > 128 ? 0x10000u : 0) | (g.bRightTrigger > 128 ? 0x20000u : 0);
}

static const char *pad_button_name(DWORD bit) {
    switch (bit) {
    case 0x0001: return "D-pad Up"; case 0x0002: return "D-pad Down"; case 0x0004: return "D-pad Left";
    case 0x0008: return "D-pad Right"; case 0x0010: return "Start"; case 0x0020: return "Back / View";
    case 0x0040: return "Left stick click (L3)"; case 0x0080: return "Right stick click (R3)";
    case 0x0100: return "LB"; case 0x0200: return "RB"; case 0x1000: return "A"; case 0x2000: return "B";
    case 0x4000: return "X"; case 0x8000: return "Y"; case 0x10000: return "LT"; case 0x20000: return "RT";
    default: return "None";
    }
}

// Arcade buttons that the loader only takes from the keyboard ([keyboard] in config.ini).
struct ArcadeBind { const char *name; const char *cfg_key; const char *default_key; };
static const ArcadeBind ARCADE[ARCADE_COUNT] = {
    { "Card", "Card", "F3" },
    { "Coin", "Coin", "1" },
};
static int arcade_vk_cache[ARCADE_COUNT];
static bool arcade_key_down[ARCADE_COUNT];
static int capture_slot = -1;   // which arcade button is waiting for a controller press

static int arcade_vk(int slot) {
    int &vk = arcade_vk_cache[slot];
    if (vk) return vk;
    char cfgp[MAX_PATH], v[32];
    snprintf(cfgp, sizeof(cfgp), "%sconfig.ini", ov_cfg_dir());
    GetPrivateProfileStringA("keyboard", ARCADE[slot].cfg_key, ARCADE[slot].default_key, v, sizeof(v), cfgp);
    char *s = v; while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == ',')) *--e = 0;
    if (strchr(s, ',')) *strchr(s, ',') = 0;   // several keys: use the first one
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 24) vk = VK_F1 + n - 1; }
    else if (strlen(s) == 1 && isalnum((unsigned char)s[0])) vk = toupper((unsigned char)s[0]);
    else if (!_stricmp(s, "Enter")) vk = VK_RETURN;
    else if (!_stricmp(s, "Space")) vk = VK_SPACE;
    else if (!_stricmp(s, "Tab")) vk = VK_TAB;
    if (!vk) vk = ARCADE[slot].default_key[0] == 'F' ? VK_F1 + atoi(ARCADE[slot].default_key + 1) - 1 : ARCADE[slot].default_key[0];
    ov_log("%s key from config.ini: '%s' -> vk 0x%02X", ARCADE[slot].name, s, vk);
    return vk;
}

static void send_key(int vk, bool down) {
    INPUT in = {}; in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk; in.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

static void card_tick(bool focus) {
    DWORD all = 0;
    for (int i = 0; i < 4; i++) if (pad_connected[i]) all |= pad_bits(pad_state[i].Gamepad);

    if (capture_slot >= 0) {   // wait for every button to be released, then take the next one pressed
        if (!all) card_capture_armed = true;
        else if (card_capture_armed) {
            for (DWORD b = 1; b <= 0x20000; b <<= 1)
                if (all & b) {
                    for (int s = 0; s < ARCADE_COUNT; s++) if (cfg.arcade_button[s] == (int)b) cfg.arcade_button[s] = 0;  // one job per button
                    cfg.arcade_button[capture_slot] = (int)b; save_config();
                    ov_log("%s button set to %s", ARCADE[capture_slot].name, pad_button_name(b));
                    break;
                }
            capture_slot = -1;
            card_pad_resume = GetTickCount() + 600;
        }
        return;
    }

    for (int s = 0; s < ARCADE_COUNT; s++) {
        int btn = cfg.arcade_button[s];
        // splitscreen: a real key press would reach every copy, so the key is held for this copy only
        bool held = btn && (all & (DWORD)btn) && (focus || split_active()) && !menu_open;
        if (held != arcade_key_down[s]) {
            arcade_key_down[s] = held;
            if (split_active()) split_set_virtual_key(arcade_vk(s), held);
            else send_key(arcade_vk(s), held);
            if (held) ov_log("%s button pressed -> key 0x%02X", ARCADE[s].name, arcade_vk(s));
        }
    }
}

void menu_filter_gamepad(XINPUT_GAMEPAD_ *g) {
    if (menu_pad_blocked()) { memset(g, 0, sizeof(*g)); return; }   // menu open / game not focused
    if (!cfg.card_hide) return;
    for (int s = 0; s < ARCADE_COUNT; s++) {
        int btn = cfg.arcade_button[s];
        if (btn == 0x10000) g->bLeftTrigger = 0;
        else if (btn == 0x20000) g->bRightTrigger = 0;
        else if (btn) g->wButtons &= (WORD)~btn;
    }
}

void menu_feed_gamepad(void) {
    ImGuiIO &io = ImGui::GetIO();
    // while choosing the card button (and shortly after) the press must not also click in the menu
    bool pad_paused = capture_slot >= 0 ||(int)(GetTickCount() - card_pad_resume) < 0;
    if (!menu_open || active_pad < 0 || pad_paused) { io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad; return; }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const XINPUT_GAMEPAD_ &g = pad_state[active_pad].Gamepad;
    struct { ImGuiKey k; WORD m; } btn[] = {
        { ImGuiKey_GamepadStart, 0x0010 }, { ImGuiKey_GamepadBack, 0x0020 },
        { ImGuiKey_GamepadFaceDown, 0x1000 }, { ImGuiKey_GamepadFaceRight, 0x2000 },
        { ImGuiKey_GamepadFaceLeft, 0x4000 }, { ImGuiKey_GamepadFaceUp, 0x8000 },
        { ImGuiKey_GamepadDpadLeft, 0x0004 }, { ImGuiKey_GamepadDpadRight, 0x0008 },
        { ImGuiKey_GamepadDpadUp, 0x0001 }, { ImGuiKey_GamepadDpadDown, 0x0002 },
        { ImGuiKey_GamepadL1, 0x0100 }, { ImGuiKey_GamepadR1, 0x0200 },
    };
    for (auto &b : btn) io.AddKeyEvent(b.k, (g.wButtons & b.m) != 0);
    auto axis = [&](ImGuiKey k, float v, float lo, float hi) {
        float t = (v - lo) / (hi - lo); t = t < 0 ? 0 : t > 1 ? 1 : t;
        io.AddKeyAnalogEvent(k, t > 0.1f, t);
    };
    const float dz = 7849.0f;
    axis(ImGuiKey_GamepadL2, g.bLeftTrigger, 30.0f, 255.0f);
    axis(ImGuiKey_GamepadR2, g.bRightTrigger, 30.0f, 255.0f);
    axis(ImGuiKey_GamepadLStickLeft, g.sThumbLX, -dz, -32768.0f);
    axis(ImGuiKey_GamepadLStickRight, g.sThumbLX, dz, 32767.0f);
    axis(ImGuiKey_GamepadLStickUp, g.sThumbLY, dz, 32767.0f);
    axis(ImGuiKey_GamepadLStickDown, g.sThumbLY, -dz, -32768.0f);
}

// ---------------------------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------------------------
// Everything is designed for 1080p and scaled by (backbuffer height / 1080) * "Menu size".
// The font is rasterized at 2x so it stays sharp up to 4K; FontGlobalScale brings it back down.
static const float FONT_PX = 20.0f, FONT_RASTER = 2.0f;
static float base_scale = 1.0f;     // current total scale
static ImGuiStyle base_style;       // style at scale 1.0

static void apply_scale(UINT bb_height) {
    float s = (bb_height ? (float)bb_height / 1080.0f : 1.0f) * cfg.ui_scale;
    if (s < 0.5f) s = 0.5f;
    if (s > 4.0f) s = 4.0f;
    if (s == base_scale) return;
    base_scale = s;
    ImGui::GetStyle() = base_style;
    ImGui::GetStyle().ScaleAllSizes(s);
    ImGui::GetIO().FontGlobalScale = s / FONT_RASTER;
}

void menu_setup_style(UINT bb_height) {
    ImGuiIO &io = ImGui::GetIO();
    char font[MAX_PATH]; GetWindowsDirectoryA(font, MAX_PATH); strcat(font, "\\Fonts\\segoeui.ttf");
    ImFontConfig fc; fc.OversampleH = 2;
    if (GetFileAttributesA(font) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(font, FONT_PX * FONT_RASTER, &fc)) {
        ImFontConfig dc; dc.SizePixels = 13.0f * FONT_RASTER;
        io.Fonts->AddFontDefault(&dc);
    }

    ImGuiStyle &s = base_style;
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 8; s.FrameRounding = 5; s.TabRounding = 5; s.GrabRounding = 5; s.PopupRounding = 5;
    s.WindowPadding = ImVec2(16, 14); s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8);
    s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    ImVec4 *c = s.Colors;
    const ImVec4 accent(0.16f, 0.48f, 0.95f, 1.00f), accent_hi(0.30f, 0.60f, 1.00f, 1.00f), red(0.86f, 0.20f, 0.22f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.14f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.15f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.21f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.21f, 0.25f, 0.36f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.15f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered] = accent; c[ImGuiCol_ButtonActive] = accent_hi;
    c[ImGuiCol_Header] = ImVec4(0.15f, 0.18f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderHovered] = accent; c[ImGuiCol_HeaderActive] = accent_hi;
    c[ImGuiCol_Tab] = ImVec4(0.11f, 0.13f, 0.19f, 1.00f);
    c[ImGuiCol_TabHovered] = accent_hi; c[ImGuiCol_TabSelected] = accent;
    c[ImGuiCol_TabSelectedOverline] = red;
    c[ImGuiCol_CheckMark] = accent_hi; c[ImGuiCol_SliderGrab] = accent; c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_PlotLines] = accent_hi; c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_NavCursor] = red;
    base_scale = 0.0f;
    apply_scale(bb_height);
}

static const char *key_name(int vk) {
    static char buf[64];
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    bool ext = (vk >= VK_PRIOR && vk <= VK_HELP) || vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_DIVIDE || vk == VK_NUMLOCK;
    LONG lp = (LONG)((sc << 16) | (ext ? (1 << 24) : 0));
    if (!sc || !GetKeyNameTextA(lp, buf, sizeof(buf))) snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
    return buf;
}

static void section(const char *title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.68f, 1.0f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

static void hint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void frame_stats(float *avg_ms, float *worst_ms) {
    double sum = 0; float worst = 0; int n = 0;
    for (int i = 0; i < FT_COUNT; i++) if (frametimes[i] > 0) { sum += frametimes[i]; n++; if (frametimes[i] > worst) worst = frametimes[i]; }
    *avg_ms = n ? (float)(sum / n) : 0; *worst_ms = worst;
}

// ---------------------------------------------------------------------------------------------
// Movable on-screen widgets (FPS counter, controller overlay). They sit in a corner, or wherever the
// player dragged them while the menu was open (stored as a fraction of the screen, top-left corner).
// ---------------------------------------------------------------------------------------------
struct Movable { float *fx, *fy; int *corner; bool dragging; };
static Movable mv_fps = { &cfg.fps_x, &cfg.fps_y, &cfg.fps_corner, false };
static Movable mv_pad = { &cfg.pad_x, &cfg.pad_y, &cfg.pad_corner, false };
static int fdm_corner = 4;   // 4 = bottom centre (only used when not dragged)
static Movable mv_meter = { &cfg.fdm_x, &cfg.fdm_y, &fdm_corner, false };

static void movable_place(Movable &m, float margin, UINT w, UINT h) {
    if (m.dragging) return;   // ImGui is moving it this frame
    if (*m.fx >= 0) { ImGui::SetNextWindowPos(ImVec2(*m.fx * w, *m.fy * h), ImGuiCond_Always, ImVec2(0, 0)); return; }
    int c = *m.corner;
    if (c == 4) { ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h - margin), ImGuiCond_Always, ImVec2(0.5f, 1.0f)); return; }
    ImGui::SetNextWindowPos(ImVec2((c & 1) ? w - margin : margin, (c & 2) ? h - margin : margin), ImGuiCond_Always,
                            ImVec2((c & 1) ? 1.0f : 0.0f, (c & 2) ? 1.0f : 0.0f));
}
static ImGuiWindowFlags movable_flags(ImGuiWindowFlags f) {
    f |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    return menu_open ? f : (f | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
}
// call between Begin and End
static void movable_update(Movable &m, UINT w, UINT h) {
    if (!menu_open) { m.dragging = false; return; }
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    m.dragging = ImGui::GetCurrentContext()->MovingWindow == win;
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    if (m.dragging) {
        p.x = p.x < 0 ? 0 : (p.x + s.x > w ? w - s.x : p.x);
        p.y = p.y < 0 ? 0 : (p.y + s.y > h ? h - s.y : p.y);
        *m.fx = p.x / (float)w; *m.fy = p.y / (float)h;
    }
    // edit mode: outline + hint so the player knows it can be moved
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    ImU32 col = ImGui::GetColorU32(m.dragging ? ImVec4(1.0f, 0.75f, 0.3f, 1) : ImVec4(0.30f, 0.60f, 1.0f, 0.9f));
    fg->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + s.x + 2, p.y + s.y + 2), col, 6, 0, 2);
    const char *t = "Drag to move";
    ImVec2 ts = ImGui::CalcTextSize(t);
    float ty = p.y - ts.y - 6 < 0 ? p.y + s.y + 4 : p.y - ts.y - 6;
    fg->AddRectFilled(ImVec2(p.x - 2, ty - 2), ImVec2(p.x + ts.x + 10, ty + ts.y + 2), ImGui::GetColorU32(ImVec4(0.05f, 0.07f, 0.12f, 0.9f)), 4);
    fg->AddText(ImVec2(p.x + 4, ty), col, t);
}
// "Position" combo: the four corners plus "Custom" after dragging
static void movable_combo(const char *label, Movable &m) {
    const char *items[] = { "Top left", "Top right", "Bottom left", "Bottom right", "Custom (dragged)" };
    int sel = *m.fx >= 0 ? 4 : *m.corner;
    ImGui::SetNextItemWidth(220.0f * base_scale);
    if (ImGui::Combo(label, &sel, items, *m.fx >= 0 ? 5 : 4) && sel < 4) { *m.corner = sel; *m.fx = *m.fy = -1; }
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// Training tools: frame meter (two rows of the last frames + numbers) and live skeleton dots
// ---------------------------------------------------------------------------------------------
static ImU32 fd_color(int c) {
    switch (c) {
        case FD_FREE: return IM_COL32(70, 74, 86, 230);
        case FD_STARTUP: return IM_COL32(64, 200, 96, 255);
        case FD_ACTIVE: return IM_COL32(236, 64, 64, 255);
        case FD_RECOVERY: return IM_COL32(64, 128, 236, 255);
        case FD_HITSTUN: return IM_COL32(240, 200, 48, 255);
        case FD_DOWN: return IM_COL32(176, 88, 208, 255);
        case FD_GUARD: return IM_COL32(64, 204, 224, 255);
    }
    return IM_COL32(0, 0, 0, 0);
}

static void fd_summary_text(char *out, size_t n, const FdSummary *s) {
    if (!s || !s->valid) { snprintf(out, n, "-"); return; }
    char adv[32] = "";
    if (s->adv_known) snprintf(adv, sizeof(adv), "   On hit %+d", s->adv);
    else if (s->hit && s->chained) snprintf(adv, sizeof(adv), "   (combo)");
    if (s->hit) snprintf(out, n, "Startup %df   Total %df%s", s->startup, s->total, adv);
    else snprintf(out, n, "Whiff   Total %df", s->total);
}

static void draw_frame_meter(UINT w, UINT h) {
    const float k = (float)h / 1080.0f;
    const int N = 90;
    const float cw = floorf(9.0f * k), ch = floorf(16.0f * k), gap = 1.0f;
    const float line = ImGui::GetFontSize(), pad = 10 * k;
    const float total_w = N * cw, win_w = total_w + 2 * pad, win_h = 2 * (ch + line + 8 * k) + 2 * pad;
    movable_place(mv_meter, 110.0f * k, w, h);
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.66f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * k);
    bool shown = ImGui::Begin("##framemeter", NULL, movable_flags(0));
    ImGui::PopStyleVar(2);
    if (!shown) { ImGui::End(); return; }
    movable_update(mv_meter, w, h);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    const float x0 = o.x + pad, y0 = o.y + pad + line + 2 * k;
    for (int row = 0; row < 2; row++) {
        int u = row ? fd_target() : fd_player();
        float y = y0 + row * (ch + line + 8 * k);
        char txt[160]; fd_summary_text(txt, sizeof(txt), fd_summary(row));
        char label[200]; snprintf(label, sizeof(label), "%s %s   %s", row ? "TARGET" : "YOU", u >= 0 ? fd_unit_name(u) : "?", txt);
        dl->AddText(ImVec2(x0, y - line - 2 * k), IM_COL32(230, 236, 248, 255), label);
        for (int i = 0; i < N; i++) {
            int c = u >= 0 ? fd_cell(u, N - 1 - i) : FD_NONE;
            float x = x0 + i * cw;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw - gap, y + ch), c == FD_NONE ? IM_COL32(40, 42, 50, 200) : fd_color(c));
        }
    }
    ImGui::End();
}

static void draw_skeleton(UINT w, UINT h) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float r = 3.0f * (float)h / 1080.0f;
    static float b[128][3];
    for (int i = 0; i < fd_unit_count(); i++) {
        ImU32 col = i == fd_player() ? IM_COL32(80, 230, 120, 230) : i == fd_target() ? IM_COL32(240, 90, 90, 230) : IM_COL32(230, 230, 230, 200);
        int n = fd_bones(i, b, 128);
        for (int j = 0; j < n; j++) {
            float x, y;
            if (!fd_project(b[j], &x, &y) || x < -1.2f || x > 1.2f || y < -1.2f || y > 1.2f) continue;
            dl->AddCircleFilled(ImVec2((x + 1) * 0.5f * w, (1 - y) * 0.5f * h), r, col, 8);
        }
    }
}

static void draw_spheres(UINT w, UINT h, int (*get)(int, float (*)[4], int), ImU32 fill, ImU32 line) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float th = 2.0f * (float)h / 1080.0f;
    static float s[256][4];
    for (int i = 0; i < fd_unit_count(); i++) {
        int n = get(i, s, 256);
        for (int j = 0; j < n; j++) {
            float x, y, r;
            if (!fd_project_sphere(s[j], s[j][3], &x, &y, &r) || x < -1.3f || x > 1.3f || y < -1.3f || y > 1.3f) continue;
            ImVec2 c((x + 1) * 0.5f * w, (1 - y) * 0.5f * h); float rp = r * 0.5f * h;
            if (rp < 2 || rp > h) continue;
            dl->AddCircleFilled(c, rp, fill, 32);
            dl->AddCircle(c, rp, line, 32, th);
        }
    }
}

static void draw_hurtboxes(UINT w, UINT h) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float th = 2.0f * (float)h / 1080.0f;
    static float s[256][4];
    for (int i = 0; i < fd_unit_count(); i++) {
        int n = fd_hurt(i, s, 64);
        for (int j = 0; j < n; j++) {
            float x, y, r;
            if (!fd_project_sphere(s[j], s[j][3], &x, &y, &r) || x < -1.3f || x > 1.3f || y < -1.3f || y > 1.3f) continue;
            ImVec2 c((x + 1) * 0.5f * w, (1 - y) * 0.5f * h); float rp = r * 0.5f * h;
            if (rp < 2 || rp > h) continue;
            dl->AddCircleFilled(c, rp, IM_COL32(60, 220, 110, 40), 32);
            dl->AddCircle(c, rp, IM_COL32(60, 230, 120, 220), 32, th);
        }
    }
}

static void draw_fps_counter(UINT w, UINT h) {
    movable_place(mv_fps, 12.0f * (float)h / 1080.0f, w, h);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##fps", NULL, movable_flags(ImGuiWindowFlags_AlwaysAutoResize))) {
        float avg, worst; frame_stats(&avg, &worst);
        ImGui::Text("%.0f FPS", fps_display);
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f ms", avg);
        movable_update(mv_fps, w, h);
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Controller overlay (input display). Drawn in a 740 x 420 design space and scaled to the screen.
// ---------------------------------------------------------------------------------------------
// Arcade stick style: 8-way lever + 8 buttons in the Vewlix layout. Fightsticks in XInput mode send
// X Y RB LB on the top row and A B RT LT on the bottom row; the lever is the D-pad or the left stick.
static void draw_arcade_overlay(UINT w, UINT h, const XINPUT_GAMEPAD_ *g) {
    const float DW = 540.0f, DH = 260.0f;
    const float k = (float)h / 1080.0f * 0.72f * cfg.pad_size;
    movable_place(mv_pad, 14.0f * (float)h / 1080.0f, w, h);
    ImGui::SetNextWindowSize(ImVec2(DW * k, DH * k), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(cfg.pad_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f * k);
    if (ImGui::Begin("##padview", NULL, movable_flags(0))) {
        movable_update(mv_pad, w, h);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetWindowPos();
        auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
        const float a = cfg.pad_alpha;
        const ImU32 idle_c = ImGui::GetColorU32(ImVec4(0.47f, 0.47f, 0.49f, a));
        const ImU32 rim_c = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.33f, a));
        const ImU32 line_c = ImGui::GetColorU32(ImVec4(0.62f, 0.62f, 0.64f, a));
        const ImU32 on_c = ImGui::GetColorU32(ImVec4(0.56f, 0.83f, 0.94f, a));
        const ImU32 text_c = ImGui::GetColorU32(ImVec4(1, 1, 1, a));
        const ImU32 dark_c = ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, a));
        ImFont *font = ImGui::GetFont();
        auto label = [&](ImVec2 c, const char *t, float px, ImU32 col) {
            ImVec2 ts = font->CalcTextSizeA(px * k, FLT_MAX, 0, t);
            dl->AddText(font, px * k, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
        };
        const WORD b = g->wButtons;

        // lever: D-pad first, otherwise the left stick
        float dx = (b & 0x0008 ? 1.0f : 0.0f) - (b & 0x0004 ? 1.0f : 0.0f);
        float dy = (b & 0x0002 ? 1.0f : 0.0f) - (b & 0x0001 ? 1.0f : 0.0f);
        if (dx == 0 && dy == 0) {
            float sx = g->sThumbLX / 32767.0f, sy = -g->sThumbLY / 32767.0f;
            if (fabsf(sx) > 0.45f) dx = sx > 0 ? 1.0f : -1.0f;
            if (fabsf(sy) > 0.45f) dy = sy > 0 ? 1.0f : -1.0f;
        }
        // fighting-game training style: square gate with 8 marks, a ball that slides to the direction and
        // leaves a light trail behind it (from where it was, fading out in a fraction of a second)
        const ImVec2 lc = P(125, 135);
        // gate: a rounded octagon (long, slightly bulging sides and short cut corners); the 8 marks sit on its
        // outline and the ball goes to them: straight directions to the middle of a side, diagonals to a corner
        const float S = 72.0f, BULGE = 84.0f, CUT = 24.0f;   // side line, bulge control point, corner cut
        const float RO = (S + BULGE) * 0.5f, RD = S - CUT * 0.5f;   // centre -> straight mark / diagonal mark
        const bool moved = dx != 0 || dy != 0;
        const ImU32 gate_c = ImGui::GetColorU32(ImVec4(0.80f, 0.82f, 0.88f, a * 0.55f));
        auto gate_path = [&]() {
            // corners clockwise from the top-right, each side bulges out through a quadratic curve
            const float cx[4] = { 1, 1, -1, -1 }, cy[4] = { -1, 1, 1, -1 };
            for (int i = 0; i < 4; i++) {
                float sx = cx[i], sy = cy[i];
                // the corner cut goes from the end of one side to the start of the next
                ImVec2 a0 = (i % 2 == 0) ? P(125 + sx * (S - CUT), 135 + sy * S) : P(125 + sx * S, 135 + sy * (S - CUT));
                ImVec2 a1 = (i % 2 == 0) ? P(125 + sx * S, 135 + sy * (S - CUT)) : P(125 + sx * (S - CUT), 135 + sy * S);
                (void)a0;   // reached by the previous side (the last side ends at corner 0: the path closes there)
                dl->PathLineTo(a1);
                // next side: to the start of the next corner, bulging outwards
                float nx = cx[(i + 1) % 4], ny = cy[(i + 1) % 4];
                ImVec2 b0 = ((i + 1) % 2 == 0) ? P(125 + nx * (S - CUT), 135 + ny * S) : P(125 + nx * S, 135 + ny * (S - CUT));
                ImVec2 ctl = (i % 2 == 0) ? P(125 + sx * BULGE, 135) : P(125, 135 + sy * BULGE);
                dl->PathBezierQuadraticCurveTo(ctl, b0, 16);
            }
        };
        gate_path(); dl->PathFillConvex(ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, a * 0.25f)));
        gate_path(); dl->PathStroke(gate_c, ImDrawFlags_Closed, 2.5f * k);
        const float sgx = dx > 0 ? 1.0f : dx < 0 ? -1.0f : 0.0f, sgy = dy > 0 ? 1.0f : dy < 0 ? -1.0f : 0.0f;
        const float rr = (sgx != 0 && sgy != 0) ? RD : RO;
        const float tx = sgx * rr, ty = sgy * rr;
        for (int i = 0; i < 9; i++) {
            float ix = (float)(i % 3 - 1), iy = (float)(i / 3 - 1), r = (ix != 0 && iy != 0) ? RD : RO;
            float mx = ix * r, my = iy * r;
            bool on = moved && ix == sgx && iy == sgy;
            dl->AddCircleFilled(P(125 + mx, 135 + my), (on ? 6.5f : i == 4 ? 3.0f : 4.5f) * k, gate_c, 16);
        }
        // ball position: slides quickly to the target (frame-rate independent)
        static float bx, by;
        struct TrailPt { float x, y; double t; };
        static TrailPt trail[96]; static int trail_n, trail_head;
        const double now = ImGui::GetTime();
        float dt = ImGui::GetIO().DeltaTime; if (dt > 0.1f) dt = 0.1f;
        const float f = 1.0f - expf(-dt * 38.0f);
        bx += (tx - bx) * f; by += (ty - by) * f;
        if (fabsf(tx - bx) < 0.3f) bx = tx;
        if (fabsf(ty - by) < 0.3f) by = ty;
        trail[trail_head] = { bx, by, now }; trail_head = (trail_head + 1) % 96; if (trail_n < 96) trail_n++;
        // trail: newest -> oldest, thinner and fainter with age
        const double LIFE = 0.28;
        const ImVec4 tc(1.0f, 0.18f, 0.32f, 1.0f);   // red-pink light
        for (int pass = 0; pass < 2; pass++) {       // 0 = soft glow, 1 = core
            for (int i = 0; i + 1 < trail_n; i++) {
                const TrailPt &p0 = trail[(trail_head - 1 - i + 192) % 96], &p1 = trail[(trail_head - 2 - i + 192) % 96];
                double age = now - p1.t;
                if (age > LIFE) break;
                if (p0.x == p1.x && p0.y == p1.y) continue;
                float life = 1.0f - (float)(age / LIFE);
                float wdt = (pass ? 4.0f + 9.0f * life : 10.0f + 16.0f * life) * k;
                float al = a * life * (pass ? 0.95f : 0.25f);
                ImU32 c = ImGui::GetColorU32(ImVec4(tc.x, tc.y + 0.25f * (1 - life), tc.z + 0.35f * (1 - life), al));
                dl->AddLine(P(125 + p0.x, 135 + p0.y), P(125 + p1.x, 135 + p1.y), c, wdt);
                if (pass) dl->AddCircleFilled(P(125 + p1.x, 135 + p1.y), wdt * 0.5f, c, 12);   // round joints (core only: on the glow they'd stack up)
            }
        }
        // the ball
        ImVec2 ball = P(125 + bx, 135 + by);
        dl->AddCircleFilled(ball, 30 * k, ImGui::GetColorU32(ImVec4(1.0f, 0.2f, 0.25f, a * (moved ? 0.30f : 0.15f))), 32);
        dl->AddCircleFilled(ball, 22 * k, ImGui::GetColorU32(ImVec4(0.86f, 0.10f, 0.12f, a)), 32);
        dl->AddCircleFilled(ImVec2(ball.x - 3 * k, ball.y - 3 * k), 17 * k, ImGui::GetColorU32(ImVec4(1.0f, 0.30f, 0.26f, a)), 32);
        dl->AddCircleFilled(ImVec2(ball.x - 8 * k, ball.y - 9 * k), 6.5f * k, ImGui::GetColorU32(ImVec4(1, 1, 1, a * 0.55f)), 16);   // shine

        // 8 buttons, Vewlix curve: each one has its colour, dim at rest and lit with a glow when pressed
        const bool num = cfg.pad_labels == 0;
        struct Btn { float x, y; DWORD bit; const char *xb; const char *nb; float r, g, b; };
        const Btn btns[] = {
            {270, 108, 0x4000, "X", "X", 1.00f, 0.42f, 0.66f}, {342, 92, 0x8000, "Y", "Y", 0.33f, 0.62f, 1.00f},
            {414, 92, 0x0200, "RB", "R1", 0.30f, 0.86f, 0.46f}, {486, 100, 0x0100, "LB", "L1", 1.00f, 0.36f, 0.32f},
            {270, 186, 0x1000, "A", "A", 1.00f, 0.62f, 0.20f}, {342, 170, 0x2000, "B", "B", 0.72f, 0.48f, 1.00f},
            {414, 170, 0x20000, "RT", "R2", 0.25f, 0.84f, 0.90f}, {486, 178, 0x10000, "LT", "L2", 0.85f, 0.85f, 0.88f} };
        const DWORD bits = b | (g->bLeftTrigger > 60 ? 0x10000u : 0) | (g->bRightTrigger > 60 ? 0x20000u : 0);
        for (const Btn &bt : btns) {
            bool on = bits & bt.bit;
            ImVec2 c = P(bt.x, bt.y);
            if (on) dl->AddCircleFilled(c, 36 * k, ImGui::GetColorU32(ImVec4(bt.r, bt.g, bt.b, a * 0.28f)), 40);   // glow
            dl->AddCircleFilled(c, 27 * k, on ? ImGui::GetColorU32(ImVec4(bt.r, bt.g, bt.b, a))
                                              : ImGui::GetColorU32(ImVec4(bt.r * 0.22f, bt.g * 0.22f, bt.b * 0.22f, a * 0.9f)), 40);
            dl->AddCircle(c, 27 * k, ImGui::GetColorU32(ImVec4(bt.r, bt.g, bt.b, a * (on ? 1.0f : 0.55f))), 40, 2.5f * k);
            label(c, num ? bt.nb : bt.xb, 22, on ? dark_c : ImGui::GetColorU32(ImVec4(bt.r * 0.5f + 0.5f, bt.g * 0.5f + 0.5f, bt.b * 0.5f + 0.5f, a)));
        }
        // back / start
        auto small = [&](float x, const char *t, bool on) {
            dl->AddRectFilled(P(x - 34, 26), P(x + 34, 50), on ? on_c : idle_c, 12 * k);
            label(P(x, 38), t, 15, on ? dark_c : text_c);
        };
        small(378, "BACK", b & 0x0020);
        small(470, "START", b & 0x0010);
        if (!test_pad_set && active_pad < 0) label(P(DW / 2, DH - 18), "No controller connected", 20, text_c);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void draw_pad_overlay(UINT w, UINT h) {
    const XINPUT_GAMEPAD_ *g = NULL;
    static int test_frames; test_frames++;
    if (test_pad_set && test_pad_switch >= 0 && test_frames >= test_pad_switch) test_pad.wButtons = test_pad_b2;
    if (test_pad_set) g = &test_pad;
    else if (active_pad >= 0) g = &pad_state[active_pad].Gamepad;
    if (!g && !menu_open) return;   // no controller: nothing to show in game
    static const XINPUT_GAMEPAD_ idle = {};
    if (!g) g = &idle;
    if (cfg.pad_style == 1) { draw_arcade_overlay(w, h, g); return; }

    // the layout uses a 740 x 420 sheet; only the part with the controller (x 110-630, y 30-385) is shown
    const float OX = 110.0f, OY = 30.0f, DW = 520.0f, DH = 355.0f;
    const float k = (float)h / 1080.0f * 0.72f * cfg.pad_size;   // design unit -> pixels (≈375 px wide at 1080p, size 1)
    const ImVec2 size(DW * k, DH * k);
    movable_place(mv_pad, 14.0f * (float)h / 1080.0f, w, h);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(cfg.pad_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f * k);
    if (ImGui::Begin("##padview", NULL, movable_flags(0))) {
        movable_update(mv_pad, w, h);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetWindowPos();
        auto P = [&](float x, float y) { return ImVec2(o.x + (x - OX) * k, o.y + (y - OY) * k); };
        const float a = cfg.pad_alpha;
        const ImU32 idle_c = ImGui::GetColorU32(ImVec4(0.47f, 0.47f, 0.49f, a));
        const ImU32 line_c = ImGui::GetColorU32(ImVec4(0.62f, 0.62f, 0.64f, a));
        const ImU32 on_c = ImGui::GetColorU32(ImVec4(0.56f, 0.83f, 0.94f, a));      // light blue, like the reference
        const ImU32 text_c = ImGui::GetColorU32(ImVec4(1, 1, 1, a));
        const ImU32 dark_c = ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, a));
        ImFont *font = ImGui::GetFont();
        auto label = [&](ImVec2 c, const char *t, float px, ImU32 col) {
            float fs = px * k;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, t);
            dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
        };
        auto pill = [&](float x0, float y0, float x1, float y1, bool on, const char *t, float fill = -1) {
            dl->AddRectFilled(P(x0, y0), P(x1, y1), idle_c, 12 * k);
            if (fill > 0) dl->AddRectFilled(P(x0, y0), P(x0 + (x1 - x0) * fill, y1), on_c, 12 * k);   // analog trigger
            else if (on) dl->AddRectFilled(P(x0, y0), P(x1, y1), on_c, 12 * k);
            label(P((x0 + x1) / 2, (y0 + y1) / 2), t, 30, text_c);
        };
        const bool num = cfg.pad_labels == 0;
        const WORD b = g->wButtons;
        // triggers and bumpers
        pill(228, 50, 318, 88, false, num ? "L2" : "LT", g->bLeftTrigger / 255.0f);
        pill(402, 50, 492, 88, false, num ? "R2" : "RT", g->bRightTrigger / 255.0f);
        pill(228, 104, 318, 142, b & 0x0100, num ? "L1" : "LB");
        pill(402, 104, 492, 142, b & 0x0200, num ? "R1" : "RB");
        // sticks
        auto stick = [&](float cx, float cy, SHORT sx, SHORT sy, bool click) {
            dl->AddCircle(P(cx, cy), 52 * k, line_c, 48, 3 * k);
            float nx = sx / 32767.0f, ny = -sy / 32767.0f;
            dl->AddCircleFilled(P(cx + nx * 22, cy + ny * 22), 34 * k, (click || fabsf(nx) > 0.2f || fabsf(ny) > 0.2f) ? on_c : idle_c, 40);
        };
        stick(180, 213, g->sThumbLX, g->sThumbLY, b & 0x0040);
        stick(430, 315, g->sThumbRX, g->sThumbRY, b & 0x0080);
        pill(138, 296, 230, 334, b & 0x0040, num ? "L3" : "LS");
        pill(508, 296, 600, 334, b & 0x0080, num ? "R3" : "RS");
        // view / guide / menu
        dl->AddCircleFilled(P(288, 211), 13 * k, (b & 0x0020) ? on_c : idle_c, 24);
        dl->AddRect(P(282, 205), P(292, 213), dark_c, 1.5f * k, 0, 2 * k); dl->AddRect(P(285, 208), P(295, 216), dark_c, 1.5f * k, 0, 2 * k);
        dl->AddCircleFilled(P(361, 208), 31 * k, idle_c, 40);
        dl->AddCircle(P(361, 208), 22 * k, text_c, 32, 4 * k);
        dl->AddLine(P(347, 194), P(375, 222), text_c, 5 * k); dl->AddLine(P(375, 194), P(347, 222), text_c, 5 * k);
        dl->AddCircleFilled(P(434, 211), 13 * k, (b & 0x0010) ? on_c : idle_c, 24);
        for (int i = -1; i <= 1; i++) dl->AddLine(P(428, 211 + i * 4), P(440, 211 + i * 4), dark_c, 1.8f * k);
        // face buttons (lit in the Xbox colours when pressed)
        struct Face { float x, y; WORD m; const char *t; ImVec4 col; };
        const Face faces[] = { {551, 165, 0x8000, "Y", ImVec4(1.0f, 0.80f, 0.30f, a)}, {510, 210, 0x4000, "X", ImVec4(0.30f, 0.62f, 1.0f, a)},
                               {591, 210, 0x2000, "B", ImVec4(1.0f, 0.38f, 0.38f, a)}, {551, 252, 0x1000, "A", ImVec4(0.36f, 0.82f, 0.48f, a)} };
        for (const Face &fb : faces) {
            bool on = b & fb.m;
            dl->AddCircleFilled(P(fb.x, fb.y), 19 * k, on ? ImGui::GetColorU32(fb.col) : idle_c, 32);
            label(P(fb.x, fb.y), fb.t, 26, on ? dark_c : text_c);
        }
        // d-pad
        const float cx = 300, cy = 318, s = 17;
        dl->AddRect(P(cx - s, cy - s * 3), P(cx + s, cy + s * 3), line_c, 6 * k, 0, 2.5f * k);
        dl->AddRect(P(cx - s * 3, cy - s), P(cx + s * 3, cy + s), line_c, 6 * k, 0, 2.5f * k);
        dl->AddRectFilled(P(cx - s + 2, cy - s + 2), P(cx + s - 2, cy + s - 2), dark_c);
        auto tri = [&](ImVec2 p1, ImVec2 p2, ImVec2 p3, bool on) { dl->AddTriangleFilled(p1, p2, p3, on ? on_c : idle_c); };
        tri(P(cx, cy - s * 2.6f), P(cx - 10, cy - s * 1.5f), P(cx + 10, cy - s * 1.5f), b & 0x0001);
        tri(P(cx, cy + s * 2.6f), P(cx - 10, cy + s * 1.5f), P(cx + 10, cy + s * 1.5f), b & 0x0002);
        tri(P(cx - s * 2.6f, cy), P(cx - s * 1.5f, cy - 10), P(cx - s * 1.5f, cy + 10), b & 0x0004);
        tri(P(cx + s * 2.6f, cy), P(cx + s * 1.5f, cy - 10), P(cx + s * 1.5f, cy + 10), b & 0x0008);
        if (b & 0x000F) {   // light the pressed arm
            if (b & 0x0001) dl->AddRectFilled(P(cx - s + 3, cy - s * 3 + 3), P(cx + s - 3, cy - s), ImGui::GetColorU32(ImVec4(0.56f, 0.83f, 0.94f, a * 0.35f)), 4 * k);
            if (b & 0x0002) dl->AddRectFilled(P(cx - s + 3, cy + s), P(cx + s - 3, cy + s * 3 - 3), ImGui::GetColorU32(ImVec4(0.56f, 0.83f, 0.94f, a * 0.35f)), 4 * k);
            if (b & 0x0004) dl->AddRectFilled(P(cx - s * 3 + 3, cy - s + 3), P(cx - s, cy + s - 3), ImGui::GetColorU32(ImVec4(0.56f, 0.83f, 0.94f, a * 0.35f)), 4 * k);
            if (b & 0x0008) dl->AddRectFilled(P(cx + s, cy - s + 3), P(cx + s * 3 - 3, cy + s - 3), ImGui::GetColorU32(ImVec4(0.56f, 0.83f, 0.94f, a * 0.35f)), 4 * k);
        }
        if (!test_pad_set && active_pad < 0) label(P(361, 160), "No controller connected", 22, text_c);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void tab_info(UINT w, UINT h) {
    float avg, worst; frame_stats(&avg, &worst);
    section("Performance");
    ImGui::Text("FPS: %.1f", fps_display);
    ImGui::Text("Frame time: %.2f ms average, %.2f ms worst (last %d frames)", avg, worst, FT_COUNT);
    char overlay[32]; snprintf(overlay, sizeof(overlay), "%.1f ms", avg);
    ImGui::PlotLines("##ft", frametimes, FT_COUNT, ft_pos, overlay, 0.0f, 50.0f, ImVec2(-1, 90.0f * base_scale));
    section("System");
    ImGui::Text("Output resolution: %u x %u", w, h);
    { UpStatus us; up_status(&us); if (us.active) ImGui::Text("Game renders at: %u x %u (upscaler on)", us.game_w, us.game_h); }
    ImGui::Text("GPU: %s", gpu_name);
    ImGui::Text("Controller: %s", active_pad >= 0 ? "connected" : "none detected");
}

// ---------------------------------------------------------------------------------------------
// Game resolution. The loader (bngrw.dll) reads [display] Resolution from config.ini (lmnfig.ini for
// the LM) at startup and patches the exe, so a change only applies after restarting the game.
// SnowLauncher rewrites that value from its own SnowLauncher.ini (PCB_Resolution / LM_Resolution,
// index into 4k,2k,1080p,720p,...) every time it starts the game, so both files are updated.
// ---------------------------------------------------------------------------------------------
struct ResOption { const char *ini; const char *label; int launcher_index; UINT height; };
static const ResOption RES_OPTIONS[] = {
    { "144p",  "144p  (256 x 144)",    6, 144 },     // every resolution SnowLauncher offers
    { "240p",  "240p  (426 x 240)",    5, 240 },
    { "480p",  "480p  (854 x 480)",    4, 480 },
    { "720p",  "720p  (1280 x 720)",   3, 720 },
    { "1080p", "1080p (1920 x 1080)",  2, 1080 },
    { "2k",    "2K    (2560 x 1440)",  1, 1440 },
    { "4K",    "4K    (3840 x 2160)",  0, 2160 },
};
static const int RES_COUNT = sizeof(RES_OPTIONS) / sizeof(RES_OPTIONS[0]);
static int res_saved = -1;         // value in the ini when the menu was opened (-1 unknown)
static int res_selected = -1;
static char res_cfg_path[MAX_PATH], res_launcher_path[MAX_PATH];
static bool res_is_lm;

static int res_find(const char *v) {
    for (int i = 0; i < RES_COUNT; i++) if (_stricmp(v, RES_OPTIONS[i].ini) == 0) return i;
    return -1;
}

static void res_reload(void) {
    if (!res_cfg_path[0]) {
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char *name = strrchr(exe, '\\'); name = name ? name + 1 : exe;
        res_is_lm = strstr(name, "_LM") != NULL;
        snprintf(res_cfg_path, sizeof(res_cfg_path), "%s%s", res_is_lm ? ov_dir() : ov_cfg_dir(), res_is_lm ? "lmnfig.ini" : "config.ini");
        snprintf(res_launcher_path, sizeof(res_launcher_path), "%sSnowLauncher.ini", ov_dir());
    }
    char v[32];
    GetPrivateProfileStringA("display", "Resolution", "", v, sizeof(v), res_cfg_path);
    char *e = v + strlen(v); while (e > v && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    res_saved = res_find(v);
    res_selected = res_saved;
}

static bool res_apply(int i) {
    const ResOption &r = RES_OPTIONS[i];
    bool ok = WritePrivateProfileStringA("display", "Resolution", r.ini, res_cfg_path) != 0;
    if (GetFileAttributesA(res_launcher_path) != INVALID_FILE_ATTRIBUTES) {
        char idx[8]; snprintf(idx, sizeof(idx), "%d", r.launcher_index);
        ok &= WritePrivateProfileStringA("Game_Config", res_is_lm ? "LM_Resolution" : "PCB_Resolution", idx, res_launcher_path) != 0;
    }
    ov_log("resolution set to %s (%s) -> %s", r.ini, res_cfg_path, ok ? "ok" : "FAILED");
    if (ok) res_saved = i;
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Window size. SnowLauncher.ini [AppSettings] ClientWindow / LMWindow: 0 = fullscreen (borderless) on
// the main monitor, 1 = on the second monitor, 2.. = window sizes (3840*2160, 3440*1440, ...). The
// launcher applies it once ~5 s after starting the game; this menu applies it live and saves it.
// Only the 16:9 sizes are offered, since the game renders 16:9.
// ---------------------------------------------------------------------------------------------
struct WinOption { int launcher_index; int w, h; const char *label; };
static const WinOption WIN_OPTIONS[] = {
    { 0, 0, 0, "Fullscreen (borderless)" },
    // every window size SnowLauncher offers (SnowLauncher.ini ClientWindow/LMWindow index); 16:9 ones are marked
    { 2, 3840, 2160, "Window 3840 x 2160  16:9" },
    { 3, 3440, 1440, "Window 3440 x 1440" },
    { 4, 3200, 1800, "Window 3200 x 1800  16:9" },
    { 5, 2560, 1440, "Window 2560 x 1440  16:9" },
    { 6, 2560, 1080, "Window 2560 x 1080" },
    { 7, 2048, 1536, "Window 2048 x 1536" },
    { 8, 1920, 1200, "Window 1920 x 1200" },
    { 9, 1920, 1080, "Window 1920 x 1080  16:9" },
    { 10, 1920, 823, "Window 1920 x 823" },
    { 11, 1792, 768, "Window 1792 x 768" },
    { 12, 1680, 1050, "Window 1680 x 1050" },
    { 13, 1680, 945, "Window 1680 x 945  16:9" },
    { 14, 1600, 1200, "Window 1600 x 1200" },
    { 15, 1440, 900, "Window 1440 x 900" },
    { 16, 1440, 810, "Window 1440 x 810  16:9" },
    { 17, 1400, 1050, "Window 1400 x 1050" },
    { 18, 1280, 1024, "Window 1280 x 1024" },
    { 19, 1280, 960, "Window 1280 x 960" },
    { 20, 1280, 800, "Window 1280 x 800" },
    { 21, 1280, 720, "Window 1280 x 720  16:9" },
    { 22, 800, 640, "Window 800 x 640" },
    { 23, 800, 600, "Window 800 x 600" },
    { 24, 640, 480, "Window 640 x 480" },
    { 25, 320, 240, "Window 320 x 240" },
};
static const int WIN_COUNT = sizeof(WIN_OPTIONS) / sizeof(WIN_OPTIONS[0]);
static int win_saved = -1, win_selected = -1, win_saved_raw = -1;

static void win_reload(void) {
    win_saved_raw = (int)GetPrivateProfileIntA("AppSettings", res_is_lm ? "LMWindow" : "ClientWindow", -1, res_launcher_path);
    win_saved = -1;
    for (int i = 0; i < WIN_COUNT; i++)
        if (WIN_OPTIONS[i].launcher_index == win_saved_raw || (win_saved_raw == 1 && i == 0)) win_saved = i;
    win_selected = win_saved;
}

static bool win_apply(int i) {
    const WinOption &o = WIN_OPTIONS[i];
    ov_set_window(o.w, o.h, o.launcher_index == 0);
    bool ok = true;
    if (GetFileAttributesA(res_launcher_path) != INVALID_FILE_ATTRIBUTES) {
        int idx = o.launcher_index;
        if (idx == 0 && win_saved_raw == 1) idx = 1;   // keep "fullscreen on 2nd monitor" if that was the choice
        char v[8]; snprintf(v, sizeof(v), "%d", idx);
        ok = WritePrivateProfileStringA("AppSettings", res_is_lm ? "LMWindow" : "ClientWindow", v, res_launcher_path) != 0;
        if (ok) win_saved_raw = idx;
    }
    ov_log("window option %s -> launcher ini %s", o.label, ok ? "ok" : "FAILED");
    if (ok) win_saved = i;
    return ok;
}

static void monitor_size(UINT *w, UINT *h) {
    HWND hw = ov_game_hwnd(); if (!hw) hw = GetForegroundWindow();
    MONITORINFO mi = { sizeof(mi) }; *w = *h = 0;
    if (GetMonitorInfoA(MonitorFromWindow(hw, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        *w = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left); *h = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
}

// Keep the window as saved in SnowLauncher.ini. SnowLauncher sizes the window only once, ~5 s after
// starting the game, and the game resizes it afterwards, so it often doesn't end up fullscreen (and
// after a restart from this menu the launcher isn't involved at all). Once per second the overlay
// checks the window and re-applies the saved option if it has been wrong for 2 checks in a row.
static bool window_matches(HWND hwnd, const WinOption &o) {
    RECT cr; if (!GetClientRect(hwnd, &cr)) return true;
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (o.launcher_index != 0) return cw == o.w && ch == o.h;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return true;
    RECT wr; GetWindowRect(hwnd, &wr);
    LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    // no frame of any kind, the window covers the monitor and so does its client area
    return !(style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) && wr.left == mi.rcMonitor.left && wr.top == mi.rcMonitor.top &&
           wr.right == mi.rcMonitor.right && wr.bottom == mi.rcMonitor.bottom &&
           cw == mi.rcMonitor.right - mi.rcMonitor.left && ch == mi.rcMonitor.bottom - mi.rcMonitor.top;
}

static void log_window(HWND hwnd, const char *when) {
    RECT wr, cr; GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr);
    ov_log("%s: window %ld,%ld %ldx%ld, client %ldx%ld, style 0x%08lx ex 0x%08lx", when, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
           cr.right, cr.bottom, (unsigned long)GetWindowLongPtrA(hwnd, GWL_STYLE), (unsigned long)GetWindowLongPtrA(hwnd, GWL_EXSTYLE));
}

static void restarted_window_check(void) {
    static DWORD first_tick, next_check; static int wrong_count, fixes; static bool loaded, forced;
    if (!cfg.keep_window || split_active()) return;   // splitscreen: the launcher places the windows
    HWND hwnd = ov_game_hwnd();
    DWORD now = GetTickCount();
    if (!hwnd) return;
    if (!first_tick) { first_tick = now; next_check = now + 3000; }
    if ((int)(now - next_check) < 0) return;
    next_check = now + 1000;
    if (!loaded) { loaded = true; res_reload(); win_reload(); }
    if (win_saved < 0 || IsIconic(hwnd)) return;
    if (GetForegroundWindow() != hwnd && GetParent(GetForegroundWindow()) != hwnd && now - first_tick > 60000) return;  // don't fight alt-tab later on
    const WinOption &o = WIN_OPTIONS[win_saved];
    if (!forced) {
        // Apply the saved option once at startup even if the window already looks right: the loader's own
        // borderless mode can pass the checks and still not end up as a real fullscreen window.
        forced = true;
        log_window(hwnd, "startup");
        ov_log("startup: applying saved window option '%s'", o.label);
        ov_set_window(o.w, o.h, o.launcher_index == 0);
        return;
    }
    if (window_matches(hwnd, o)) { wrong_count = 0; return; }
    if (wrong_count == 0) log_window(hwnd, "mismatch");
    if (++wrong_count < 2) return;
    wrong_count = 0;
    if (++fixes > 30) return;   // something keeps changing it: give up instead of fighting forever
    ov_log("window was not '%s', re-applying (fix %d)", o.label, fixes);
    ov_set_window(o.w, o.h, o.launcher_index == 0);
}

static void tab_display(void) {
    static bool win_failed, res_failed;
    if (res_reload_pending) {   // re-read both ini files each time the menu opens
        res_reload_pending = false; res_failed = win_failed = false;
        res_reload(); win_reload();
    }
    section("Window");
    UINT mw, mh; monitor_size(&mw, &mh);
    const char *wprev = win_selected >= 0 ? WIN_OPTIONS[win_selected].label : "Other (set in the launcher)";
    ImGui::SetNextItemWidth(260.0f * base_scale);
    if (ImGui::BeginCombo("Window size", wprev)) {
        for (int i = 0; i < WIN_COUNT; i++) {
            const WinOption &o = WIN_OPTIONS[i];
            if (o.w && mw && ((UINT)o.w > mw || (UINT)o.h > mh)) continue;   // bigger than the monitor
            if (ImGui::Selectable(o.label, i == win_selected)) win_selected = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(win_selected < 0);
    if (ImGui::Button("Apply##win")) win_failed = !win_apply(win_selected);
    ImGui::EndDisabled();
    if (win_failed) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Changed now, but could not save it to SnowLauncher.ini.");
    ImGui::Checkbox("Keep this window size (fixes the launcher not going fullscreen)", &cfg.keep_window);
    hint("Changes right away and is remembered by the launcher. The image is stretched to the window, "
         "so pick the game resolution below that matches it.");

    section("Game resolution");
    const char *preview = res_selected >= 0 ? RES_OPTIONS[res_selected].label : "Unknown / custom";
    ImGui::SetNextItemWidth(260.0f * base_scale);
    if (ImGui::BeginCombo("Resolution", preview)) {
        for (int i = 0; i < RES_COUNT; i++)
            if (ImGui::Selectable(RES_OPTIONS[i].label, i == res_selected)) res_selected = i;
        ImGui::EndCombo();
    }
    if (res_selected >= 0 && mh && RES_OPTIONS[res_selected].height > mh)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Higher than your monitor (%u p): the 3D can end up in a corner.", mh);
    bool changed = res_selected >= 0 && res_selected != res_saved;
    static bool restart_failed;
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Save")) res_failed = !res_apply(res_selected);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(res_selected < 0);
    if (ImGui::Button(changed ? "Save and restart game" : "Restart game")) ImGui::OpenPopup("Restart game?");
    ImGui::EndDisabled();
    if (res_failed) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Could not write the settings file.");
    if (restart_failed) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Could not restart the game (see EXVSOverlay.log).");
    hint(changed ? "Not saved yet." : "The resolution is read when the game starts, so it needs a restart.");

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restart game?", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("The game will close and start again with the new resolution.");
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Don't do this in the middle of an online match.");
        ImGui::Spacing();
        if (ImGui::Button("Restart now", ImVec2(180.0f * base_scale, 0))) {
            bool ok = !changed || res_apply(res_selected);
            res_failed = !ok;
            if (ok) { save_config(); restart_failed = !ov_restart_game(); }   // only returns on failure
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f * base_scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }

    section("Controller overlay");
    ImGui::Checkbox("Show controller on screen", &cfg.show_pad);
    {
        const char *labels[] = { "L1 / R1 / L2 / R2", "LB / RB / LT / RT" };
        const char *styles[] = { "Controller", "Arcade stick" };
        ImGui::SetNextItemWidth(220.0f * base_scale); ImGui::Combo("Style##pad", &cfg.pad_style, styles, 2);
        movable_combo("Position##pad", mv_pad);
        ImGui::SetNextItemWidth(220.0f * base_scale); ImGui::SliderFloat("Size##pad", &cfg.pad_size, 0.5f, 2.5f, "%.2fx");
        ImGui::SetNextItemWidth(220.0f * base_scale); ImGui::SliderFloat("Buttons opacity##pad", &cfg.pad_alpha, 0.2f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(220.0f * base_scale); ImGui::SliderFloat("Background##pad", &cfg.pad_bg, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(220.0f * base_scale); ImGui::Combo("Shoulder labels##pad", &cfg.pad_labels, labels, 2);
    }
    hint("Shows what you press, like a stream input display. Controller: sticks move and triggers fill up. "
         "Arcade stick: 8-way lever (D-pad or left stick) and 8 buttons in the usual fightstick layout (X Y RB LB / A B RT LT).");

    section("Battle HUD");
    if (hud_available()) {
        bool hide = hud_hidden();
        if (ImGui::Checkbox("Hide battle HUD", &hide)) hud_set_hidden(hide);
        ImGui::Text("Hotkey: %s", rebinding && rebind_target == &cfg.hud_vk ? "press a key... (Esc cancels)" : key_name(cfg.hud_vk));
        ImGui::SameLine();
        if (!rebinding && ImGui::Button("Change key##hud")) start_rebind(&cfg.hud_vk);
        hint("Hides gauges, lock-on, messages and the result screens, for clean screenshots and videos. "
             "The menus stay visible. Always starts shown.");
    } else {
        ImGui::BeginDisabled();
        bool off = false; ImGui::Checkbox("Hide battle HUD", &off);
        ImGui::EndDisabled();
        hint(cfg.hud_enabled ? "Not available: this game exe isn't a version the overlay knows (IB 1.13.30, IB, OB)."
                             : "Turned off in EXVSOverlay.ini (hide_hud_feature=0).");
    }

    section("FPS counter");
    ImGui::Checkbox("Show FPS counter", &cfg.show_fps);
    movable_combo("Position##fps", mv_fps);
    hint("Tip: while this menu is open you can drag the FPS counter and the controller overlay anywhere on screen.");
    section("Menu");
    ImGui::SetNextItemWidth(220.0f * base_scale);
    ImGui::SliderFloat("Menu size", &cfg.ui_scale, 0.75f, 2.0f, "%.2fx");
    ImGui::SetNextItemWidth(220.0f * base_scale);
    ImGui::SliderFloat("Menu opacity", &cfg.opacity, 0.5f, 1.0f, "%.2f");
}

static void tab_graphics(void) {
    UpStatus st; up_status(&st);
    section("Upscaling - AMD FSR 1");
    const char *modes[] = { "Off", "AMD FSR 1 (upscale + sharpen)" };
    ImGui::SetNextItemWidth(300.0f * base_scale);
    if (ImGui::Combo("Upscaler", &cfg.up_mode, modes, 2)) save_config();
    ImGui::BeginDisabled(cfg.up_mode == 0);
    ImGui::SetNextItemWidth(300.0f * base_scale);
    float pct = cfg.up_sharp * 100.0f;
    if (ImGui::SliderFloat("Sharpness", &pct, 0.0f, 100.0f, "%.0f%%")) cfg.up_sharp = pct / 100.0f;
    if (ImGui::IsItemDeactivatedAfterEdit()) save_config();
    ImGui::EndDisabled();

    const ImVec4 ok(0.45f, 0.85f, 0.55f, 1.0f), warn(1.0f, 0.7f, 0.3f, 1.0f), bad(1.0f, 0.35f, 0.35f, 1.0f);
    bool need_restart = false;
    if (st.failed) {
        ImGui::TextColored(bad, "Couldn't start the upscaler this time (details in EXVSOverlay.log).");
    } else if (st.active) {
        float sx = st.game_w ? (float)st.out_w / st.game_w : 1.0f;
        ImGui::Text("Game image %ux%u  ->  screen %ux%u", st.game_w, st.game_h, st.out_w, st.out_h);
        if (cfg.up_mode == 0) {
            ImGui::TextColored(warn, "Off: plain stretch. Restart the game to remove it completely.");
            need_restart = true;
        } else if (st.path == 1) ImGui::TextColored(ok, "FSR 1 is upscaling x%.2f and sharpening.", sx);
        else if (st.path == 2) ImGui::TextColored(ok, "Same size as the screen: FSR 1 sharpening only.");
        else ImGui::TextColored(warn, "The game resolution is bigger than the window: shrunk, FSR not used.");
    } else if (cfg.up_mode != 0) {
        ImGui::TextColored(warn, "Restart the game to turn it on.");
        need_restart = true;
    }
    if (need_restart) {
        static bool restart_failed;
        if (ImGui::Button("Restart game##up")) ImGui::OpenPopup("Restart for upscaler?");
        if (restart_failed) ImGui::TextColored(bad, "Could not restart the game (see EXVSOverlay.log).");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Restart for upscaler?", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("The game will close and start again.");
            ImGui::TextColored(warn, "Don't do this in the middle of an online match.");
            ImGui::Spacing();
            if (ImGui::Button("Restart now", ImVec2(180.0f * base_scale, 0))) {
                save_config(); restart_failed = !ov_restart_game();   // only returns on failure
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120.0f * base_scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
    }
    hint("FSR 1 rebuilds the game image at your window size with sharp edges, instead of the blurry stretch. "
         "For more FPS set the game resolution lower (Display tab, e.g. 720p) and let FSR bring it to your screen. "
         "If the game already matches the window, only the sharpening is used.");
}

static void tab_controls(void) {
    section("Open / close the menu");
    ImGui::Text("Keyboard: %s", rebinding && rebind_target == &cfg.toggle_vk ? "press a key... (Esc cancels)" : key_name(cfg.toggle_vk));
    ImGui::SameLine();
    if (!rebinding && ImGui::Button("Change key")) start_rebind(&cfg.toggle_vk);
    ImGui::Checkbox("Controller: hold Back + Start (half a second)", &cfg.pad_toggle);
    section("Arcade buttons on the controller");
    bool menu_combo_used = false;
    if (ImGui::BeginTable("arcade", 3, ImGuiTableFlags_SizingFixedFit)) {
        for (int s = 0; s < ARCADE_COUNT; s++) {
            ImGui::PushID(s);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            char kn[64]; snprintf(kn, sizeof(kn), "%s", key_name(arcade_vk(s)));
            ImGui::Text("%s  (key %s)", ARCADE[s].name, kn);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (capture_slot == s) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Press a button...");
            else ImGui::TextUnformatted(pad_button_name((DWORD)cfg.arcade_button[s]));
            ImGui::TableNextColumn();
            if (capture_slot == s) { if (ImGui::Button("Cancel")) capture_slot = -1; }
            else {
                if (ImGui::Button("Choose button")) { capture_slot = s; card_capture_armed = false; }
                if (cfg.arcade_button[s]) { ImGui::SameLine(); if (ImGui::Button("Remove")) { cfg.arcade_button[s] = 0; save_config(); } }
            }
            if (cfg.arcade_button[s] == 0x0010 || cfg.arcade_button[s] == 0x0020) menu_combo_used = true;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Checkbox("Hide these buttons from the game", &cfg.card_hide);
    if (menu_combo_used) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Back / Start also open the menu when both are held for half a second.");
    hint("Holding the button presses the game's key for it (from config.ini). After Choose button, release all buttons and press the one you want.");

    section("When the game isn't focused");
    ImGui::Checkbox("Game ignores my keyboard and controller##unfocused", &cfg.ignore_unfocused);
    hint("When you switch to another window (browser, Discord, ...) the game doesn't see what you type or press. "
         "Without this, Windows keeps sending your keyboard and controller to the game in the background.");

    section("While the menu is open");
    ImGui::Checkbox("Game ignores my keyboard, mouse and controller", &cfg.block_input);
    hint("The game keeps running underneath (it can't be paused online). "
         "Controller: D-pad / left stick to move, A to select, B to go back, LB/RB to switch tabs.");
}

static void updates_section(void) {
    const ImVec4 ok(0.45f, 0.9f, 0.55f, 1.0f), warn(1.0f, 0.7f, 0.3f, 1.0f), bad(1.0f, 0.35f, 0.35f, 1.0f);
    static bool restart_failed;
    section("Updates");
    UpdInfo u; upd_info(&u);
    switch (u.state) {
        case UPD_IDLE: ImGui::TextUnformatted(cfg.auto_update ? "Not checked yet (the check runs a few seconds after the game starts)." : "Not checked yet."); break;
        case UPD_CHECKING: ImGui::TextUnformatted("Checking for updates..."); break;
        case UPD_LATEST: ImGui::TextColored(ok, "You have the latest version (%s).", OVERLAY_VERSION); break;
        case UPD_AVAILABLE: ImGui::TextColored(warn, "Version %s is available (you have %s).", u.version, OVERLAY_VERSION); break;
        case UPD_DOWNLOADING: {
            ImGui::Text("Downloading version %s...", u.version);
            float f = u.total ? (float)((double)u.done / (double)u.total) : 0.0f;
            char lbl[64]; snprintf(lbl, sizeof(lbl), "%.1f / %.1f MB", u.done / 1e6, u.total / 1e6);
            ImGui::ProgressBar(f, ImVec2(-1, 0), lbl);
            break;
        }
        case UPD_INSTALLED: ImGui::TextColored(ok, "Version %s is installed. It starts with the next game start.", u.version); break;
        case UPD_FAILED: ImGui::TextColored(bad, "%s", u.error); break;
    }
    if ((u.state == UPD_AVAILABLE || u.state == UPD_FAILED) && u.can_install && u.notes[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", u.notes);
        ImGui::PopStyleColor();
    }
    bool busy = u.state == UPD_CHECKING || u.state == UPD_DOWNLOADING;
    if (u.can_install && (u.state == UPD_AVAILABLE || u.state == UPD_FAILED)) {
        char lbl[64]; snprintf(lbl, sizeof(lbl), "Download and install %s", u.version);
        if (ImGui::Button(lbl)) upd_install();
        ImGui::SameLine();
    }
    if (u.state == UPD_INSTALLED) {
        if (ImGui::Button("Restart game now")) restart_failed = !ov_restart_game();   // only returns on failure
        ImGui::SameLine();
        ImGui::TextColored(warn, "Not in the middle of an online match.");
        if (restart_failed) ImGui::TextColored(bad, "Could not restart the game (see EXVSOverlay.log).");
    } else {
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Check now")) upd_check_now();
        ImGui::EndDisabled();
    }
    if (ImGui::Checkbox("Check for updates when the game starts", &cfg.auto_update)) save_config();
    char h[200]; snprintf(h, sizeof(h), "Updates come from github.com/%s (releases). EXVS Launcher also keeps the overlay up to date.", u.repo);
    hint(h);
}

static void tab_training(void) {
    section("Training tools");
    hint("Only in Training and in Practice vs CPU. They turn themselves off in every other mode, online included.");
    if (ImGui::Checkbox("Frame meter", &cfg.fd_meter)) save_config();
    hint("Two bars with the last 90 frames of you and your target, plus startup, total frames and frame advantage on hit after each attack. "
         "While this menu is open you can drag it anywhere.");
    if (cfg.fdm_x >= 0) { if (ImGui::Button("Put the meter back at the bottom")) { cfg.fdm_x = cfg.fdm_y = -1; save_config(); } }
    if (ImGui::Checkbox("Hurtboxes", &cfg.fd_hurt)) save_config();
    hint("Green circles: the body spheres where each unit can be hit (from the game's own data, placed on the live bones).");
    if (ImGui::Checkbox("Hitboxes", &cfg.fd_hit)) save_config();
    hint("Red circles: the attack spheres of melee moves, only during their active frames (from the game's action table). "
         "Units from older series keep their moves in a script: no hitboxes for them yet.");
    if (cfg.fd_hit && fd_active()) {
        bool any = false; for (int i = 0; i < fd_unit_count(); i++) any |= fd_has_hitboxes(i);
        if (!any) ImGui::TextDisabled("  No hitbox data for these units yet.");
        else if (fd_learned() < 0) ImGui::TextDisabled("  Learning: land 3 melees with a dash (like N1). Until then those show when they hit.");
    }
    if (ImGui::Checkbox("Skeleton view (bones)", &cfg.fd_bones)) save_config();
    hint("Draws every bone of each unit as a dot (you green, target red). First step of the hitbox viewer.");
    ImGui::Spacing();
    ImGui::TextDisabled("Status:"); ImGui::SameLine(); ImGui::TextUnformatted(fd_status());
    if (fd_active()) {
        ImGui::Text("Units found: %d   You: %s   Target: %s", fd_unit_count(), fd_player() >= 0 ? fd_unit_name(fd_player()) : "?", fd_target() >= 0 ? fd_unit_name(fd_target()) : "?");
    }
    section("Colors");
    struct { int c; const char *t; } legend[] = { { FD_STARTUP, "Startup (until the hit)" }, { FD_ACTIVE, "Hit" }, { FD_RECOVERY, "Recovery" },
        { FD_HITSTUN, "Hitstun" }, { FD_DOWN, "Knocked down" }, { FD_GUARD, "Guarding" }, { FD_FREE, "Free (can act)" } };
    for (auto &l : legend) {
        ImVec2 p = ImGui::GetCursorScreenPos(); float s = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + s, p.y + s), fd_color(l.c), 2);
        ImGui::Dummy(ImVec2(s, s)); ImGui::SameLine(); ImGui::TextUnformatted(l.t);
    }
    hint("EXVS melee attacks home in on the target, so startup changes with distance: the meter shows what really happened. "
         "Frame advantage is measured from when you can act again to when the target can.");
}

static void tab_about(void) {
    section("EXVS Overlay " OVERLAY_VERSION);
    ImGui::TextWrapped("In-game menu for EXVSIB. It doesn't change any game files.");
    ImGui::TextWrapped("Settings: %sEXVSOverlay.ini", ov_dir());
    ImGui::TextWrapped("Log: %sEXVSOverlay.log", ov_dir());
    hint("To remove it, delete xinput9_1_0.dll from the game folder (or turn it off in EXVS Launcher).");
    updates_section();

    section("Credits");
    ImGui::Text("Created by Furshow");
    ImGui::TextWrapped("Part of EXVS Launcher.");
    ImGui::Spacing();
    ImGui::TextWrapped("Dear ImGui - MIT License, (c) Omar Cornut. Built with Zig / Clang.");
    ImGui::TextWrapped("Hide battle HUD based on HideUI - MIT License, (c) 2026 EXVS2OB-BGMTool contributors.");
    ImGui::TextWrapped("Upscaling: AMD FidelityFX Super Resolution 1 (EASU + RCAS) - MIT License, (c) 2021 Advanced Micro Devices, Inc.");
    hint("Fan-made tool, free and non-commercial. Not affiliated with or endorsed by Bandai Namco or Sunrise. "
         "Mobile Suit Gundam and Extreme VS. 2 Infinite Boost are trademarks of their respective owners.");
}

void menu_draw(UINT w, UINT h) {
    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = menu_open;
    apply_scale(h);

    if (cfg.show_pad) draw_pad_overlay(w, h);
    if (cfg.show_fps) draw_fps_counter(w, h);   // also while the menu is open, so it can be dragged
    if (fd_active()) {
        if (cfg.fd_hurt) draw_hurtboxes(w, h);
        if (cfg.fd_hit) {
            draw_spheres(w, h, fd_hit, IM_COL32(240, 50, 50, 90), IM_COL32(255, 70, 70, 240));
            draw_spheres(w, h, [](int i, float (*o)[4], int m) { return i == 0 ? fd_proj(o, m) : 0; }, IM_COL32(250, 210, 40, 80), IM_COL32(255, 220, 50, 240));
        }
        if (cfg.fd_bones) draw_skeleton(w, h);
        if (cfg.fd_meter) draw_frame_meter(w, h);
    }
    if (!menu_open) return;

    const float s = base_scale;
    static bool menu_dragging;
    if (!menu_dragging) {
        if (cfg.menu_x >= 0) ImGui::SetNextWindowPos(ImVec2(cfg.menu_x * w, cfg.menu_y * h), ImGuiCond_Always);
        else ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(720.0f * s, 660.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(cfg.opacity);
    bool open = true;
    char title[64]; snprintf(title, sizeof(title), "EXVS Overlay  %s###menu", OVERLAY_VERSION);
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        {   // dragged by its title bar: remember where (fraction of the screen, kept inside it)
            ImGuiWindow *win = ImGui::GetCurrentWindow();
            menu_dragging = ImGui::GetCurrentContext()->MovingWindow == win;
            if (menu_dragging) {
                ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
                p.x = p.x < 0 ? 0 : (p.x + sz.x > w ? w - sz.x : p.x); p.y = p.y < 0 ? 0 : (p.y + sz.y > h ? h - sz.y : p.y);
                cfg.menu_x = p.x / (float)w; cfg.menu_y = p.y / (float)h;
            }
        }
        // test host only: EXVSOVERLAY_TEST_TAB=display opens that tab, EXVSOVERLAY_TEST_RES=<ini value> saves it
        static int test_init = 0; ImGuiTabItemFlags display_flags = 0, controls_flags = 0, graphics_flags = 0, about_flags = 0, training_flags = 0;
        if (!test_init) {
            test_init = 1; char v[16];
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_TAB", v, sizeof(v)) && _stricmp(v, "display") == 0) display_flags = ImGuiTabItemFlags_SetSelected;
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_TAB", v, sizeof(v)) && _stricmp(v, "controls") == 0) controls_flags = ImGuiTabItemFlags_SetSelected;
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_TAB", v, sizeof(v)) && _stricmp(v, "graphics") == 0) graphics_flags = ImGuiTabItemFlags_SetSelected;
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_TAB", v, sizeof(v)) && _stricmp(v, "about") == 0) about_flags = ImGuiTabItemFlags_SetSelected;
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_TAB", v, sizeof(v)) && _stricmp(v, "training") == 0) training_flags = ImGuiTabItemFlags_SetSelected;
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_RES", v, sizeof(v))) {
                res_reload(); int i = res_find(v);
                if (i >= 0) { res_apply(i); res_reload_pending = true; }
            }
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_RESTART", v, sizeof(v)) && GetEnvironmentVariableA("EXVSOVERLAY_RESTARTED", NULL, 0) == 0) {
                ov_log("test: restarting");
                ov_restart_game();
            }
            if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_WIN", v, sizeof(v))) {
                res_reload(); win_reload(); int want = atoi(v);
                for (int i = 0; i < WIN_COUNT; i++) if (WIN_OPTIONS[i].launcher_index == want) { win_apply(i); res_reload_pending = true; }
            }
        }
        float footer = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild("body", ImVec2(0, -footer))) {
            UpdInfo u; upd_info(&u);
            if (u.state == UPD_AVAILABLE) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Update available: version %s (About > Updates).", u.version);
            else if (u.state == UPD_INSTALLED) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.55f, 1.0f), "Version %s is installed: restart the game to use it.", u.version);
            if (ImGui::BeginTabBar("tabs")) {
                if (ImGui::BeginTabItem("Info")) { tab_info(w, h); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Display", NULL, display_flags)) { tab_display(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Graphics", NULL, graphics_flags)) { tab_graphics(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controls", NULL, controls_flags)) { tab_controls(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Training", NULL, training_flags)) { tab_training(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("About", NULL, about_flags)) { tab_about(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
        char close_label[96]; snprintf(close_label, sizeof(close_label), "Close  (%s)", key_name(cfg.toggle_vk));
        float cw = cfg.menu_x >= 0 ? ImGui::GetContentRegionAvail().x - 160.0f * s : -1;
        if (ImGui::Button(close_label, ImVec2(cw, 0))) open = false;
        if (cfg.menu_x >= 0) { ImGui::SameLine(); if (ImGui::Button("Center menu", ImVec2(-1, 0))) { cfg.menu_x = cfg.menu_y = -1; save_config(); } }
    }
    ImGui::End();
    if (!open) set_open(false);
}
