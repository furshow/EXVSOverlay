// Test of the hide-HUD detours without the game: fake draw functions with the game's exact prologues,
// fake HUD nodes with MSVC-style RTTI, then install() and check which draws run.
//   build: see build.sh (hud_test.exe); run: hud_test.exe -> prints PASS/FAIL lines, exit code 0 = all pass
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

void ov_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
const char *ov_dir(void) { return ".\\"; }
#include "../src/hud_hide.cpp"

extern "C" volatile int g_draws[3];
volatile int g_draws[3];

// Same first instructions as UI_DRAW ("push rsi; sub rsp,20h; cmp byte [rcx+4FCh],0; mov rsi,rcx") and
// UI_DRAW_LOCKON/CURSOR ("sub rsp,28h; cmp byte [rcx+4FCh],0; jne +5Dh"), then count the call.
extern "C" __attribute__((naked)) void fake_draw(void) {
    __asm__ volatile(".intel_syntax noprefix\n"
                     ".byte 0x40,0x56, 0x48,0x83,0xec,0x20, 0x80,0xb9,0xfc,0x04,0x00,0x00,0x00, 0x48,0x8b,0xf1\n"
                     "lock inc dword ptr [rip + g_draws]\n"
                     "add rsp, 0x20\n pop rsi\n ret\n"
                     ".att_syntax prefix\n");
}
#define FAKE_SMALL(N, IDX)                                                                     \
    extern "C" __attribute__((naked)) void fake_small##N(void) {                               \
        __asm__ volatile(".intel_syntax noprefix\n"                                            \
                         ".byte 0x48,0x83,0xec,0x28, 0x80,0xb9,0xfc,0x04,0x00,0x00,0x00, 0x75,0x5d\n" \
                         "lock inc dword ptr [rip + g_draws + 4*" #IDX "]\n"                  \
                         "add rsp, 0x28\n ret\n"                                               \
                         ".fill 0x5d, 1, 0xcc\n"                                               \
                         ".att_syntax prefix\n");                                              \
    }
FAKE_SMALL(1, 1)
FAKE_SMALL(2, 2)

// fake MSVC RTTI: object -> vtable[-1] = complete object locator -> type descriptor (name at +16)
struct Locator { DWORD sig, off, cdoff, type_rva, hier_rva, self_rva; };
static struct { char pad[16]; char name[200]; } type_hud = { {}, ".?AVCInHudGui@@" }, type_menu = { {}, ".?AVCMenuThing@@" };
static Locator loc_hud, loc_menu;
static void *vt_hud[2], *vt_menu[2];
struct Node { void **vtable; char pad[0x500]; };
static Node node_hud, node_menu;

int main() {
    const BYTE *base = (const BYTE *)GetModuleHandleW(NULL);
    auto rva = [&](const void *p) { return (DWORD)((const BYTE *)p - base); };
    loc_hud = { 1, 0, 0, rva(&type_hud), 0, rva(&loc_hud) };
    loc_menu = { 1, 0, 0, rva(&type_menu), 0, rva(&loc_menu) };
    vt_hud[0] = &loc_hud; vt_menu[0] = &loc_menu;
    node_hud.vtable = &vt_hud[1]; node_menu.vtable = &vt_menu[1];

    // set up the module state like hud_init does, then install on the fake functions
    s_image = base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    s_image_size = nt->OptionalHeader.SizeOfImage;
    s_sections = IMAGE_FIRST_SECTION(nt);
    s_section_count = nt->FileHeader.NumberOfSections;
    HudVersion v = VERSIONS[0];
    v.t[0].rva = rva((const void *)fake_draw); v.t[1].rva = rva((const void *)fake_small1); v.t[2].rva = rva((const void *)fake_small2);
    for (int i = 0; i < 3; i++)
        if (memcmp(base + v.t[i].rva, v.t[i].fp, v.t[i].steal) != 0) { printf("FAIL fake prologue %d differs\n", i); return 1; }
    if (!install(v)) { printf("FAIL install\n"); return 1; }
    s_ready = 1;

    typedef void (*Draw)(Node *);
    Draw fns[3] = { (Draw)fake_draw, (Draw)fake_small1, (Draw)fake_small2 };
    int fails = 0;
    auto run = [&](const char *what, Node *n, bool hidden, int expect) {
        hud_set_hidden(hidden);
        int before[3] = { g_draws[0], g_draws[1], g_draws[2] };
        for (Draw f : fns) f(n);
        for (int i = 0; i < 3; i++) {
            int got = g_draws[i] - before[i];
            bool ok = got == expect;
            if (!ok) fails++;
            printf("%s %-34s fn%d drew %d (expected %d)\n", ok ? "PASS" : "FAIL", what, i, got, expect);
        }
    };
    run("visible, HUD node", &node_hud, false, 1);
    run("hidden, HUD node", &node_hud, true, 0);
    run("hidden, menu node (not HUD)", &node_menu, true, 1);
    run("visible again, HUD node", &node_hud, false, 1);
    printf(fails ? "SOME TESTS FAILED\n" : "ALL PASS\n");
    return fails ? 1 : 0;
}
