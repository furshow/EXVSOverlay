// Input while the game isn't focused (alt-tab to a browser, Discord, ...).
//
// The game reads its controls through the loader (bngrw.dll for the Client, lmgrw.dll for the Live
// Monitor), not through window messages, so Windows keeps delivering them in the background:
//   - keyboard: GetAsyncKeyState (global key state, typing in a browser reaches the game),
//   - DirectInput-mode controllers: Raw Input (RegisterRawInputDevices + GetRawInputData), parsed with
//     HidP_GetUsages (buttons) and HidP_GetUsageValue (hat / axes),
//   - XInput controllers: XInputGetState, which is our own export (menu_filter_gamepad covers it).
// Those imports of the loader are patched in its IAT (only the loader: the overlay's own calls and the
// rest of the process are untouched). While menu_game_input_blocked() is true the loader sees:
//   keys up, keyboard / mouse raw input turned into releases, no buttons pressed, the hat centred,
//   X / Y centred (the lever), other axes frozen at their last value.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "menu.h"

typedef LONG NTSTATUS_;
#define HIDP_STATUS_SUCCESS_ ((NTSTATUS_)0x00110000)
typedef SHORT (WINAPI *PGetAsyncKeyState)(int);
typedef UINT (WINAPI *PGetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef NTSTATUS_ (WINAPI *PHidP_GetUsages)(int, USHORT, USHORT, USHORT *, ULONG *, void *, char *, ULONG);
typedef NTSTATUS_ (WINAPI *PHidP_GetUsageValue)(int, USHORT, USHORT, USHORT, ULONG *, void *, char *, ULONG);

// HIDP_VALUE_CAPS (hidpi.h), declared here so no DDK header is needed. 72 bytes.
struct HidValueCaps_ {
    USHORT UsagePage; UCHAR ReportID; BOOLEAN IsAlias; USHORT BitField, LinkCollection, LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute, HasNull; UCHAR Reserved;
    USHORT BitSize, ReportCount, Reserved2[5]; ULONG UnitsExp, Units; LONG LogicalMin, LogicalMax, PhysicalMin, PhysicalMax;
    USHORT UsageMin, UsageMax, StringMin, StringMax, DesignatorMin, DesignatorMax, DataIndexMin, DataIndexMax;
};
typedef NTSTATUS_ (WINAPI *PHidP_GetSpecificValueCaps)(int, USHORT, USHORT, USHORT, HidValueCaps_ *, USHORT *, void *);

static PGetAsyncKeyState rGetAsyncKeyState;
static PGetRawInputData rGetRawInputData;
static PHidP_GetUsages rHidP_GetUsages;
static PHidP_GetUsageValue rHidP_GetUsageValue;
static PHidP_GetSpecificValueCaps rHidP_GetSpecificValueCaps;

static volatile LONG g_blocked_calls;   // diagnostics: calls answered "idle"
static volatile LONG g_last_state = -1; // last blocked state seen by a hook (for the log)

static bool blocked(void) {
    bool b = menu_game_input_blocked();
    LONG s = b ? 1 : 0;
    if (InterlockedExchange(&g_last_state, s) != s && g_last_state >= 0)
        ov_log("input: game %s keyboard/controller input", b ? "ignores" : "receives");
    if (b) InterlockedIncrement(&g_blocked_calls);
    return b;
}

// ---- keyboard -------------------------------------------------------------------------------
static SHORT WINAPI h_GetAsyncKeyState(int vk) {
    SHORT r = rGetAsyncKeyState(vk);   // always called: it also resets the "pressed since last call" bit
    return blocked() ? 0 : r;
}

// ---- raw input: keyboard / mouse events become releases ---------------------------------------
static UINT WINAPI h_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = rGetRawInputData(h, cmd, data, size, hdr);
    if (cmd != RID_INPUT || !data || r == (UINT)-1 || r < sizeof(RAWINPUTHEADER) || !blocked()) return r;
    RAWINPUT *ri = (RAWINPUT *)data;
    if (ri->header.dwType == RIM_TYPEKEYBOARD && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
        ri->data.keyboard.Flags |= RI_KEY_BREAK;
        ri->data.keyboard.Message = ri->data.keyboard.Message == WM_SYSKEYDOWN ? WM_SYSKEYUP : WM_KEYUP;
    } else if (ri->header.dwType == RIM_TYPEMOUSE && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        USHORT f = ri->data.mouse.usButtonFlags, out = 0;
        for (int b = 0; b < 5; b++) if (f & ((1u << (2 * b)) | (2u << (2 * b)))) out |= (USHORT)(2u << (2 * b));   // down/up -> up
        ri->data.mouse.usButtonFlags = out; ri->data.mouse.usButtonData = 0;
        ri->data.mouse.lLastX = ri->data.mouse.lLastY = 0;
    }
    return r;
}

// ---- HID controllers seen by the loader (for the overlay's input display and menu) --------------
// DirectInput-mode pads (arcade sticks in PS3/PS4 mode, e.g. Hori Fighting Stick Mini 4) have no XInput
// state. The loader parses their reports with HidP_*, so the real values are recorded here, per device
// (its preparsed data), before any blocking. focus_hid_pad() turns the latest one into an Xbox-style pad.
struct HidPad {
    void *pp; ULONGLONG tick; DWORD buttons;   // bit n = HID button n+1
    ULONG hat; bool hat_seen; LONG hat_lo, hat_hi;
    ULONG xy[2]; bool xy_seen[2]; LONG xy_lo[2], xy_hi[2]; USHORT xy_bits[2];
};
static HidPad g_hid[4];
static SRWLOCK g_hid_lock = SRWLOCK_INIT;

static HidPad *hid_slot(void *pp) {   // call with the lock held exclusively
    HidPad *oldest = &g_hid[0];
    for (auto &h : g_hid) {
        if (h.pp == pp) return &h;
        if (h.tick < oldest->tick) oldest = &h;
    }
    memset(oldest, 0, sizeof(*oldest)); oldest->pp = pp;
    ov_log("input: DirectInput controller read by the loader (%p): shown in the controller overlay", pp);
    return oldest;
}
static bool value_range(int type, USHORT page, USHORT link, USHORT usage, void *pp, LONG *lo, LONG *hi, USHORT *bits) {
    if (!rHidP_GetSpecificValueCaps) return false;
    HidValueCaps_ caps[2]; USHORT n = 2;
    if (rHidP_GetSpecificValueCaps(type, page, link, usage, caps, &n, pp) != HIDP_STATUS_SUCCESS_ || n < 1) return false;
    *lo = caps[0].LogicalMin; *hi = caps[0].LogicalMax; if (bits) *bits = caps[0].BitSize;
    return true;
}
static void hid_record_buttons(void *pp, const USHORT *list, ULONG n) {
    DWORD b = 0;
    for (ULONG i = 0; i < n; i++) if (list[i] >= 1 && list[i] <= 32) b |= 1u << (list[i] - 1);
    AcquireSRWLockExclusive(&g_hid_lock);
    HidPad *h = hid_slot(pp); h->buttons = b; h->tick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_hid_lock);
}
static void hid_record_value(int type, USHORT page, USHORT link, USHORT usage, ULONG v, void *pp) {
    if (page != 0x01 || (usage != 0x39 && usage != 0x30 && usage != 0x31)) return;
    AcquireSRWLockExclusive(&g_hid_lock);
    HidPad *h = hid_slot(pp);
    if (usage == 0x39) {
        if (!h->hat_seen) { USHORT bits = 0; if (!value_range(type, page, link, usage, pp, &h->hat_lo, &h->hat_hi, &bits)) { h->hat_lo = 0; h->hat_hi = 7; } h->hat_seen = true; }
        h->hat = v;
    } else {
        int a = usage - 0x30;
        if (!h->xy_seen[a]) { if (!value_range(type, page, link, usage, pp, &h->xy_lo[a], &h->xy_hi[a], &h->xy_bits[a]) || h->xy_hi[a] <= h->xy_lo[a]) { h->xy_lo[a] = 0; h->xy_hi[a] = 255; h->xy_bits[a] = 0; } h->xy_seen[a] = true; }
        h->xy[a] = v;
    }
    h->tick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_hid_lock);
}

// Latest DirectInput pad as an Xbox pad, with the usual PS3/PS4 button order (1 Square, 2 Cross, 3 Circle,
// 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2, 9 Share/Select, 10 Options/Start, 11 L3, 12 R3). False when the
// loader hasn't read any HID pad in the last two minutes.
bool focus_hid_pad(XINPUT_GAMEPAD_ *g) {
    memset(g, 0, sizeof(*g));
    AcquireSRWLockShared(&g_hid_lock);
    const HidPad *best = NULL;
    for (auto &h : g_hid) if (h.pp && (!best || h.tick > best->tick)) best = &h;
    HidPad p = {}; if (best) p = *best;
    ReleaseSRWLockShared(&g_hid_lock);
    if (!best || GetTickCount64() - p.tick > 120000) return false;
    static const WORD map[12] = { 0x4000, 0x1000, 0x2000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0020, 0x0010, 0x0040, 0x0080 };
    for (int i = 0; i < 12; i++) if (p.buttons & (1u << i)) g->wButtons |= map[i];
    if (p.buttons & (1u << 6)) g->bLeftTrigger = 255;
    if (p.buttons & (1u << 7)) g->bRightTrigger = 255;
    if (p.hat_seen) {
        LONG d = (LONG)p.hat - p.hat_lo;   // 0 = up, clockwise in 45° steps; out of range = centred
        if (d >= 0 && d <= 7 && (LONG)p.hat <= p.hat_hi) {
            static const WORD dirs[8] = { 0x1, 0x1 | 0x8, 0x8, 0x8 | 0x2, 0x2, 0x2 | 0x4, 0x4, 0x4 | 0x1 };
            g->wButtons |= dirs[d];
        }
    }
    for (int a = 0; a < 2; a++) {
        if (!p.xy_seen[a]) continue;
        LONG lo = p.xy_lo[a], hi = p.xy_hi[a], v = (LONG)p.xy[a];
        USHORT bits = p.xy_bits[a];   // values come unsigned: sign-extend for a signed range
        if (lo < 0 && bits > 0 && bits < 32 && (p.xy[a] & (1u << (bits - 1)))) v = (LONG)(p.xy[a] | ~((1u << bits) - 1));
        double t = (double)(v - lo) / (double)(hi - lo) * 2.0 - 1.0;   // -1..1
        if (t < -1) t = -1; if (t > 1) t = 1;
        SHORT s = (SHORT)(t * 32767.0);
        if (a == 0) g->sThumbLX = s; else g->sThumbLY = (SHORT)-s;   // HID Y grows downwards
    }
    return true;
}

// ---- HID controllers (DirectInput mode) ---------------------------------------------------------
static NTSTATUS_ WINAPI h_HidP_GetUsages(int type, USHORT page, USHORT link, USHORT *list, ULONG *len, void *pp, char *rep, ULONG rlen) {
    NTSTATUS_ st = rHidP_GetUsages(type, page, link, list, len, pp, rep, rlen);
    if (st == HIDP_STATUS_SUCCESS_ && len && list && type == 0 /*HidP_Input*/ && page == 0x09) hid_record_buttons(pp, list, *len);
    if (st == HIDP_STATUS_SUCCESS_ && len && blocked()) *len = 0;   // no button pressed
    return st;
}

// last value of every axis while input was allowed, so blocked reads can return it (frozen)
struct AxisSlot { void *pp; USHORT page, link, usage; ULONG value; };
static AxisSlot g_axes[96];
static SRWLOCK g_axes_lock = SRWLOCK_INIT;

static bool neutral_value(void *pp, int type, USHORT page, USHORT link, USHORT usage, ULONG *out) {
    if (page == 0x01 && (usage == 0x39 || usage == 0x30 || usage == 0x31) && rHidP_GetSpecificValueCaps) {
        HidValueCaps_ caps[2]; USHORT n = 2;
        if (rHidP_GetSpecificValueCaps(type, page, link, usage, caps, &n, pp) == HIDP_STATUS_SUCCESS_ && n >= 1) {
            LONG lo = caps[0].LogicalMin, hi = caps[0].LogicalMax;
            if (usage == 0x39) {   // hat switch: a value outside the logical range = centred
                ULONG lim = caps[0].BitSize && caps[0].BitSize < 32 ? (1u << caps[0].BitSize) : 0;
                if (lo > 0) *out = 0;                                  // e.g. 1..8: 0 is centred
                else if (!lim || (ULONG)hi + 1 < lim) *out = (ULONG)hi + 1;   // e.g. 0..7 in 4 bits: 8
                else goto frozen;                                      // no spare value: keep the last one
            } else {
                // unsigned report value; a signed range (e.g. -32768..32767) has its centre at 0
                if (lo < 0) *out = 0;
                else *out = (ULONG)(((int64_t)lo + hi + 1) / 2);
            }
            return true;
        }
    }
frozen:
    bool found = false;
    AcquireSRWLockShared(&g_axes_lock);
    for (auto &a : g_axes) if (a.pp == pp && a.page == page && a.link == link && a.usage == usage) { *out = a.value; found = true; break; }
    ReleaseSRWLockShared(&g_axes_lock);
    return found;
}

static void remember_value(void *pp, USHORT page, USHORT link, USHORT usage, ULONG v) {
    AcquireSRWLockExclusive(&g_axes_lock);
    AxisSlot *free_slot = NULL;
    for (auto &a : g_axes) {
        if (a.pp == pp && a.page == page && a.link == link && a.usage == usage) { a.value = v; free_slot = NULL; goto done; }
        if (!a.pp && !free_slot) free_slot = &a;
    }
    if (free_slot) *free_slot = { pp, page, link, usage, v };
done:
    ReleaseSRWLockExclusive(&g_axes_lock);
}

static NTSTATUS_ WINAPI h_HidP_GetUsageValue(int type, USHORT page, USHORT link, USHORT usage, ULONG *value, void *pp, char *rep, ULONG rlen) {
    NTSTATUS_ st = rHidP_GetUsageValue(type, page, link, usage, value, pp, rep, rlen);
    if (st != HIDP_STATUS_SUCCESS_ || !value) return st;
    if (type == 0 /*HidP_Input*/) hid_record_value(type, page, link, usage, *value, pp);
    if (!blocked()) { remember_value(pp, page, link, usage, *value); return st; }
    ULONG v; if (neutral_value(pp, type, page, link, usage, &v)) *value = v;
    return st;
}

// ---- IAT patching -------------------------------------------------------------------------------
struct Patch { const char *dll, *name; void *hook; void **real; };
static const Patch PATCHES[] = {
    { "user32.dll", "GetAsyncKeyState", (void *)h_GetAsyncKeyState, (void **)&rGetAsyncKeyState },
    { "user32.dll", "GetRawInputData", (void *)h_GetRawInputData, (void **)&rGetRawInputData },
    { "hid.dll", "HidP_GetUsages", (void *)h_HidP_GetUsages, (void **)&rHidP_GetUsages },
    { "hid.dll", "HidP_GetUsageValue", (void *)h_HidP_GetUsageValue, (void **)&rHidP_GetUsageValue },
};

// Points every import of `mod` that matches PATCHES at our hook. Returns how many were patched.
static int patch_module(HMODULE mod, const char *label) {
    uint8_t *base = (uint8_t *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int done = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        HMODULE target = GetModuleHandleA(dll);
        IMAGE_THUNK_DATA64 *names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk) : NULL;
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + d->FirstThunk);
        for (size_t i = 0; iat[i].u1.Function; i++) {
            for (const Patch &p : PATCHES) {
                if (_stricmp(dll, p.dll) != 0) continue;
                bool match = false;
                if (names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                    IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
                    match = strcmp((const char *)ibn->Name, p.name) == 0;
                } else if (target) {
                    match = (void *)iat[i].u1.Function == (void *)GetProcAddress(target, p.name);
                }
                if (!match || (void *)iat[i].u1.Function == p.hook) continue;
                if (!*p.real) *p.real = (void *)iat[i].u1.Function;
                DWORD old;
                if (VirtualProtect(&iat[i].u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    InterlockedExchangePointer((void **)&iat[i].u1.Function, p.hook);
                    VirtualProtect(&iat[i].u1.Function, sizeof(void *), old, &old);
                    ov_log("input: %s!%s patched in %s", p.dll, p.name, label);
                    done++;
                }
            }
        }
    }
    return done;
}

// The loader may be injected after us, so it's looked for during the first minute.
static DWORD WINAPI focus_thread(LPVOID) {
    const wchar_t *mods[] = { L"bngrw.dll", L"lmgrw.dll" };
    const char *labels[] = { "bngrw.dll", "lmgrw.dll" };
    bool done[2] = {};
    HMODULE hid = LoadLibraryW(L"hid.dll");
    if (hid) rHidP_GetSpecificValueCaps = (PHidP_GetSpecificValueCaps)GetProcAddress(hid, "HidP_GetSpecificValueCaps");
    for (int t = 0; t < 600 && !(done[0] && done[1]); t++) {
        for (int i = 0; i < 2; i++) {
            if (done[i]) continue;
            HMODULE m = GetModuleHandleW(mods[i]);
            if (!m) continue;
            int n = patch_module(m, labels[i]);
            ov_log("input: %d import(s) of %s patched (ignore input when not focused)", n, labels[i]);
            done[i] = true;
        }
        if (done[0] || done[1]) { if (t > 50) break; }   // one loader found: give the other 5 s more
        Sleep(100);
    }
    if (!done[0] && !done[1]) ov_log("input: no loader found (bngrw.dll / lmgrw.dll); only XInput is filtered");
    return 0;
}

void focus_init(void) {
    HANDLE t = CreateThread(NULL, 0, focus_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

long focus_blocked_calls(void) { return g_blocked_calls; }
