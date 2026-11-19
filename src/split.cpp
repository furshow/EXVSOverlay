// Splitscreen: several copies of the game on one PC, one per player (started by the EXVS Launcher).
//
// Each copy is started as `vsac29_Release.exe "<player folder>"`. The loader (bngrw.dll) then reads
// config.ini and card.ini from that folder instead of the game folder, and the launcher writes a
// [split] section there for the overlay:
//   [split]
//   Enabled  = 1
//   Player   = 1..4
//   Pad      = XInput slot (0-3) this copy reads, -1 = none
//   Keyboard = 1 when this player plays on the keyboard
//
// Only one window has the focus at a time, so in a split copy the controllers are never blocked for
// focus: the copy only sees its own XInput pad (as pad 1) and the keyboard only reaches the keyboard
// player's copy. The card / coin buttons of the overlay become keys pressed for this copy only.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "menu.h"

static char g_cfg_dir[MAX_PATH];   // folder of this copy's config.ini (with trailing backslash)
static bool g_active, g_keyboard;
static int g_player, g_pad = -1;
static volatile LONG g_vkeys[8];   // 256 bits: keys the overlay holds down for this copy
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile bool g_ready;

// The arguments after the exe name, split like CommandLineToArgvW does for simple cases (quotes, no
// escapes): shell32 isn't loaded this early. Returns how many there are; the first one goes to `first`.
static int command_args(wchar_t *first, size_t cap) {
    const wchar_t *p = GetCommandLineW();
    if (*p == L'"') { p++; while (*p && *p != L'"') p++; if (*p) p++; }   // argv[0] (no escapes there)
    else while (*p && *p != L' ' && *p != L'\t') p++;
    int n = 0; first[0] = 0;
    for (;;) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) return n;
        wchar_t buf[MAX_PATH]; size_t k = 0; bool q = false;
        while (*p && (q || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') q = !q; else if (k + 1 < MAX_PATH) buf[k++] = *p;
            p++;
        }
        buf[k] = 0;
        if (n++ == 0) { wcsncpy(first, buf, cap - 1); first[cap - 1] = 0; }
    }
}

static BOOL CALLBACK split_load(PINIT_ONCE, PVOID, PVOID *) {
    // Same rule as the loader: one argument = the folder with config.ini, otherwise the exe folder.
    wchar_t arg[MAX_PATH];
    if (command_args(arg, MAX_PATH) == 1 && arg[0]) {
        wchar_t full[MAX_PATH];
        DWORD n = GetFullPathNameW(arg, MAX_PATH, full, NULL);
        DWORD attr = n && n < MAX_PATH ? GetFileAttributesW(full) : INVALID_FILE_ATTRIBUTES;
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            WideCharToMultiByte(CP_ACP, 0, full, -1, g_cfg_dir, MAX_PATH - 2, NULL, NULL);
            size_t l = strlen(g_cfg_dir);
            if (l && g_cfg_dir[l - 1] != '\\') { g_cfg_dir[l] = '\\'; g_cfg_dir[l + 1] = 0; }
        }
    }
    g_ready = true;
    if (!g_cfg_dir[0]) return TRUE;

    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%sconfig.ini", g_cfg_dir);
    g_active = GetPrivateProfileIntA("split", "Enabled", 0, ini) != 0;
    if (!g_active) { ov_log("config folder: %s", g_cfg_dir); return TRUE; }
    g_player = (int)GetPrivateProfileIntA("split", "Player", 1, ini);
    if (g_player < 1 || g_player > 4) g_player = 1;
    char v[16]; GetPrivateProfileStringA("split", "Pad", "-1", v, sizeof(v), ini);
    g_pad = atoi(v); if (g_pad < -1 || g_pad > 3) g_pad = -1;
    g_keyboard = GetPrivateProfileIntA("split", "Keyboard", 0, ini) != 0;
    ov_log("splitscreen: player %d, config %s, XInput pad %d, keyboard %s", g_player, g_cfg_dir, g_pad, g_keyboard ? "yes" : "no");
    return TRUE;
}
// Loaded on first use (from any thread; not from DllMain, which can't read files safely).
void split_init(void) { InitOnceExecuteOnce(&g_once, split_load, NULL, NULL); }

const char *ov_cfg_dir(void) { split_init(); return g_cfg_dir[0] ? g_cfg_dir : ov_dir(); }
bool split_active(void) { split_init(); return g_active; }
int split_player(void) { split_init(); return g_active ? g_player : 0; }
int split_log_player(void) { return g_ready && g_active ? g_player : 0; }   // for ov_log: never loads
int split_pad(void) { split_init(); return g_pad; }
bool split_keyboard(void) { split_init(); return g_keyboard; }

bool split_virtual_key(int vk) {
    if (vk < 0 || vk > 255) return false;
    return (g_vkeys[vk >> 5] >> (vk & 31)) & 1;
}
void split_set_virtual_key(int vk, bool down) {
    if (vk < 0 || vk > 255) return;
    LONG bit = 1L << (vk & 31);
    if (down) InterlockedOr(&g_vkeys[vk >> 5], bit); else InterlockedAnd(&g_vkeys[vk >> 5], ~bit);
}
