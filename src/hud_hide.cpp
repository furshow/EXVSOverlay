// Hide the battle / result HUD.
//
// Technique and class list from HideUI (EXVS2OB-BGMTool contributors, MIT License):
// the game draws every HUD element through three functions (common draw, lock-on, cursor) whose first
// argument is the HUD node. A small detour on each one looks at the node's MSVC RTTI class name and,
// while "hide" is on, returns before drawing nodes of the battle/result HUD classes.
//
// The functions are found at known addresses per game version and only hooked when their first 16
// bytes match the expected code, so an unknown or modified exe is left alone.

#include <windows.h>
#include <string.h>
#include <stdint.h>
#include "menu.h"

struct HudTarget { uint32_t rva; uint8_t fp[16]; int steal; };   // steal = whole instructions copied to the trampoline
struct HudVersion { const char *name; HudTarget t[3]; };

// prologues: "push rsi; sub rsp,20h" (2+4 bytes) and "sub rsp,28h; cmp byte [rcx+4FCh],0" (4+7 bytes)
static const HudVersion VERSIONS[] = {
    { "IB 1.13.30", { { 0xA82ED0, { 0x40, 0x56, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf1 }, 6 },
                      { 0xA37200, { 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x75, 0x5d, 0x48, 0x8b, 0x81 }, 11 },
                      { 0xA3A110, { 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x75, 0x5d, 0x48, 0x8b, 0x81 }, 11 } } },
    { "IB",         { { 0xA5EFC0, { 0x40, 0x56, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf1 }, 6 },
                      { 0xA0FB70, { 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x75, 0x5d, 0x48, 0x8b, 0x81 }, 11 },
                      { 0xA11EE0, { 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x75, 0x5d, 0x48, 0x8b, 0x81 }, 11 } } },
    { "OB",         { { 0xA3F180, { 0x40, 0x56, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf1 }, 6 },
                      { 0xA07F40, { 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x75, 0x3a, 0x48, 0x8b, 0x81 }, 11 },
                      { 0xA0A060, { 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xfc, 0x04, 0x00, 0x00, 0x00, 0x75, 0x3a, 0x48, 0x8b, 0x81 }, 11 } } },
};

static const BYTE *s_image;
static DWORD s_image_size;
static const IMAGE_SECTION_HEADER *s_sections;
static unsigned s_section_count;
static volatile LONG s_hidden, s_ready;
static volatile LONG s_seen, s_skipped;
static const char *s_version;

extern "C" void *g_hud_tramp[3];
void *g_hud_tramp[3];

// ---------------------------------------------------------------------------------------------
// RTTI check (MSVC x64 complete object locator: sig, offset, cdOffset, typeDescriptor RVA, hierarchy RVA, self RVA)
// ---------------------------------------------------------------------------------------------
static bool image_readable(const void *addr, size_t size) {
    ULONG_PTR d = (ULONG_PTR)addr - (ULONG_PTR)s_image;
    if (!s_image || d >= s_image_size || size > s_image_size - d) return false;
    for (unsigned i = 0; i < s_section_count; i++) {
        const IMAGE_SECTION_HEADER &sec = s_sections[i];
        DWORD len = sec.Misc.VirtualSize;
        if (d >= sec.VirtualAddress && d - sec.VirtualAddress < len && size <= len - (d - sec.VirtualAddress))
            return (sec.Characteristics & IMAGE_SCN_MEM_READ) != 0;
    }
    return false;
}

static bool hidden_class(const char *name) {
    static const char *const classes[] = {
        "CInHudGui@", "CInHudGui_Disp@", "CInHudGuiCommon@",
        "CInHudInfoBar@", "CInHudInfoBar_Disp@",
        "CInHudPlayerSticker@", "CInHudAppearPilot@", "CInHudAppearPilot_Disp@",
        "CInHudActionWindow@", "CInHudJingle@", "CInHudEndJingle@",
        "CInHudAlert@", "CInHudAlert_Disp@",
        "CInHudLockon@", "CInHudLockon_Disp@",
        "CInHudLockonCV@", "CInHudLockonCV_Disp@",
        "CInHudInfoCursor@", "CInHudInfoCursor_Disp@",
        "CInHudMessage@", "CInHudMessage_Disp@",
        "CInHudSubMonitor@", "CInHudSubMonitor_Disp@",
        "CInHudTargetGauge@", "CInHudTargetGauge_Disp@",
        "CInHudAwake@", "CInHudAwake_Disp@",
        "CInHudAwakePilot@", "CInHudAwakePilot_Disp@",
        "CInHudIntrude@", "CInHudPractice@", "CInHudTutorial@",
        "CInHudResult@", "CInHudResultCapture@", "COutHudPracticeResultFinal@",
        "COutHudRootBattleOverlay@", "COutHudAlwaysDisplayObject@",
        "CInHudLmIcon@", "CInHudLmResult@", "CInHudLmResultSticker@",
        "CInHudLmHud2@", "CInHudLmHud2_Disp@",
        "CInHudLmHud4@", "CInHudLmHud4_Disp@", "CInHudLmTimeline@" };
    if (!name || strncmp(name, ".?AV", 4) != 0) return false;
    name += 4;
    for (const char *c : classes) if (strncmp(name, c, strlen(c)) == 0) return true;
    return false;
}

static bool hidden_node(const void *node) {
    if (!node) return false;
    const ULONG_PTR *vtable = *(const ULONG_PTR *const *)node;
    if ((ULONG_PTR)vtable < sizeof(ULONG_PTR) || !image_readable(vtable - 1, 2 * sizeof(ULONG_PTR))) return false;
    const DWORD *loc = (const DWORD *)vtable[-1];
    if (!image_readable(loc, 24) || loc[0] != 1 || loc[1] != 0 || (ULONG_PTR)loc - (ULONG_PTR)s_image != loc[5] || loc[3] >= s_image_size) return false;
    if (s_image_size - loc[3] < 16 + 160) return false;
    const char *name = (const char *)(s_image + loc[3] + 16);
    if (!image_readable(name, 160) || !memchr(name, 0, 160)) return false;
    return hidden_class(name);
}

// called by the thunks before the original draw; nonzero = skip drawing this node
extern "C" int hud_should_hide(const void *node) {
    if (!s_hidden) return 0;
    if (!hidden_node(node)) return 0;
    InterlockedIncrement(&s_skipped);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Detours. Each thunk saves every volatile register (the game may rely on them even though the ABI
// doesn't promise it), asks hud_should_hide(rcx), then either returns or jumps to the trampoline.
// ---------------------------------------------------------------------------------------------
#define HUD_THUNK(N)                                                                                     \
    extern "C" __attribute__((naked)) void hud_thunk##N(void) {                                          \
        __asm__ volatile(".intel_syntax noprefix\n"                                                      \
                         "sub rsp, 0xC8\n"                                                               \
                         "mov [rsp+0x20], rax\n mov [rsp+0x28], rcx\n mov [rsp+0x30], rdx\n"            \
                         "mov [rsp+0x38], r8\n mov [rsp+0x40], r9\n mov [rsp+0x48], r10\n"              \
                         "mov [rsp+0x50], r11\n"                                                         \
                         "movdqu [rsp+0x60], xmm0\n movdqu [rsp+0x70], xmm1\n movdqu [rsp+0x80], xmm2\n" \
                         "movdqu [rsp+0x90], xmm3\n movdqu [rsp+0xA0], xmm4\n movdqu [rsp+0xB0], xmm5\n" \
                         "call hud_should_hide\n"                                                        \
                         "test eax, eax\n"                                                               \
                         "movdqu xmm0, [rsp+0x60]\n movdqu xmm1, [rsp+0x70]\n movdqu xmm2, [rsp+0x80]\n" \
                         "movdqu xmm3, [rsp+0x90]\n movdqu xmm4, [rsp+0xA0]\n movdqu xmm5, [rsp+0xB0]\n" \
                         "mov rax, [rsp+0x20]\n mov rcx, [rsp+0x28]\n mov rdx, [rsp+0x30]\n"            \
                         "mov r8, [rsp+0x38]\n mov r9, [rsp+0x40]\n mov r10, [rsp+0x48]\n"              \
                         "mov r11, [rsp+0x50]\n"                                                         \
                         "lea rsp, [rsp+0xC8]\n" /* lea keeps the flags from 'test' */                   \
                         "jnz 1f\n"                                                                      \
                         "jmp qword ptr [rip + g_hud_tramp + 8*" #N "]\n"                                \
                         "1: ret\n"                                                                      \
                         ".att_syntax prefix\n");                                                        \
    }
HUD_THUNK(0)
HUD_THUNK(1)
HUD_THUNK(2)
static void (*const THUNKS[3])(void) = { hud_thunk0, hud_thunk1, hud_thunk2 };

// executable memory within +-2 GB of the exe, so the 5-byte "jmp rel32" patch can reach it
static BYTE *alloc_near(const BYTE *target) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    ULONG_PTR gran = si.dwAllocationGranularity;
    ULONG_PTR start = ((ULONG_PTR)target + s_image_size + gran) & ~(gran - 1);
    for (ULONG_PTR a = start; a < (ULONG_PTR)target + 0x70000000; a += gran) {
        void *p = VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return (BYTE *)p;
    }
    return NULL;
}

static void write_abs_jmp(BYTE *at, const void *to) {   // jmp qword ptr [rip+0]; dq to
    at[0] = 0xFF; at[1] = 0x25; memset(at + 2, 0, 4); memcpy(at + 6, &to, 8);
}

static bool install(const HudVersion &v) {
    BYTE *page = alloc_near(s_image);
    if (!page) { ov_log("hud: no memory near the game exe"); return false; }
    for (int i = 0; i < 3; i++) {
        BYTE *fn = (BYTE *)s_image + v.t[i].rva;
        BYTE *relay = page + i * 64, *tramp = relay + 16;
        write_abs_jmp(relay, (const void *)THUNKS[i]);
        memcpy(tramp, fn, v.t[i].steal);                      // whole, position-independent instructions
        write_abs_jmp(tramp + v.t[i].steal, fn + v.t[i].steal);
        g_hud_tramp[i] = tramp;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    for (int i = 0; i < 3; i++) {
        BYTE *fn = (BYTE *)s_image + v.t[i].rva;
        BYTE patch[16]; memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xE9;
        int32_t rel = (int32_t)((page + i * 64) - (fn + 5));
        memcpy(patch + 1, &rel, 4);
        DWORD old;
        VirtualProtect(fn, v.t[i].steal, PAGE_EXECUTE_READWRITE, &old);
        memcpy(fn, patch, v.t[i].steal);
        VirtualProtect(fn, v.t[i].steal, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, v.t[i].steal);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
void hud_init(void) {
    s_image = (const BYTE *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)s_image;
    if (!s_image || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(s_image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return;
    s_image_size = nt->OptionalHeader.SizeOfImage;
    s_sections = IMAGE_FIRST_SECTION(nt);
    s_section_count = nt->FileHeader.NumberOfSections;
    for (const HudVersion &v : VERSIONS) {
        bool ok = true;
        for (const HudTarget &t : v.t)
            if (!image_readable(s_image + t.rva, 16) || memcmp(s_image + t.rva, t.fp, 16) != 0) { ok = false; break; }
        if (!ok) continue;
        if (install(v)) {
            s_version = v.name;
            InterlockedExchange(&s_ready, 1);
            ov_log("hud: hide-HUD hooks installed (%s)", v.name);
        }
        return;
    }
    ov_log("hud: this game exe isn't a known version; hide-HUD is unavailable");
}

bool hud_available(void) { return s_ready != 0; }
bool hud_hidden(void) { return s_hidden != 0; }
void hud_set_hidden(bool hide) {
    if (!s_ready || (s_hidden != 0) == hide) return;
    InterlockedExchange(&s_hidden, hide ? 1 : 0);
    ov_log("hud: %s (skipped %ld draws so far)", hide ? "hidden" : "shown", s_skipped);
}
const char *hud_version(void) { return s_version ? s_version : ""; }
