// Test for src/input_focus.cpp (input while the game isn't focused), no game and no window needed.
// One source, three builds:
//   -DFAKE_HID   -> hid.dll    : HidP_GetUsages reports 3 buttons, HidP_GetUsageValue 200, caps 0..255 / hat 0..7
//   -DFAKE_LOADER-> bngrw.dll  : imports GetAsyncKeyState, GetRawInputData (user32) and the two HidP calls
//   (none)       -> focus_test.exe <overlay dll> <expect: pass|block>
// The test process never has the focus, so with ignore_unfocused=1 the overlay must answer "idle".
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef LONG NTSTATUS_;
#define OK_ ((NTSTATUS_)0x00110000)

#if defined(FAKE_HID)
struct Caps { USHORT UsagePage; UCHAR ReportID; BOOLEAN IsAlias; USHORT BitField, LinkCollection, LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute, HasNull; UCHAR Reserved;
    USHORT BitSize, ReportCount, Reserved2[5]; ULONG UnitsExp, Units; LONG LogicalMin, LogicalMax, PhysicalMin, PhysicalMax;
    USHORT UsageMin, UsageMax, StringMin, StringMax, DesignatorMin, DesignatorMax, DataIndexMin, DataIndexMax; };
extern "C" __declspec(dllexport) NTSTATUS_ WINAPI HidP_GetUsages(int, USHORT, USHORT, USHORT *l, ULONG *n, void *, char *, ULONG) {
    if (l) { l[0] = 1; l[1] = 2; l[2] = 3; } if (n) *n = 3; return OK_; }
extern "C" __declspec(dllexport) NTSTATUS_ WINAPI HidP_GetUsageValue(int, USHORT, USHORT, USHORT, ULONG *v, void *, char *, ULONG) {
    if (v) *v = 200; return OK_; }
extern "C" __declspec(dllexport) NTSTATUS_ WINAPI HidP_GetSpecificValueCaps(int, USHORT page, USHORT, USHORT usage, Caps *c, USHORT *n, void *) {
    memset(c, 0, sizeof(*c)); c->UsagePage = page; c->BitSize = usage == 0x39 ? 4 : 8;
    c->LogicalMin = 0; c->LogicalMax = usage == 0x39 ? 7 : 255; *n = 1; return OK_; }
#elif defined(FAKE_LOADER)
extern "C" __declspec(dllimport) NTSTATUS_ WINAPI HidP_GetUsages(int, USHORT, USHORT, USHORT *, ULONG *, void *, char *, ULONG);
extern "C" __declspec(dllimport) NTSTATUS_ WINAPI HidP_GetUsageValue(int, USHORT, USHORT, USHORT, ULONG *, void *, char *, ULONG);
extern "C" __declspec(dllexport) SHORT fl_key(int vk) { return GetAsyncKeyState(vk); }
extern "C" __declspec(dllexport) UINT fl_raw(void) { UINT sz = 0; return GetRawInputData(NULL, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)); }
extern "C" __declspec(dllexport) ULONG fl_buttons(void) { USHORT l[8]; ULONG n = 8; HidP_GetUsages(0, 9, 0, l, &n, (void *)1, NULL, 0); return n; }
extern "C" __declspec(dllexport) ULONG fl_value(USHORT usage) { ULONG v = 0; HidP_GetUsageValue(0, 1, 0, usage, &v, (void *)1, NULL, 0); return v; }
#else
#include <stdint.h>
// address the loader's IAT holds for `name`
static void *iat_entry(HMODULE m, const char *name) {
    uint8_t *b = (uint8_t *)m; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dir.VirtualAddress); d->Name; d++) {
        auto *nm = (IMAGE_THUNK_DATA64 *)(b + d->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA64 *)(b + d->FirstThunk);
        for (int i = 0; iat[i].u1.Function; i++)
            if (!(nm[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) && !strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(b + nm[i].u1.AddressOfData))->Name, name))
                return (void *)iat[i].u1.Function;
    }
    return NULL;
}
static bool inside(HMODULE m, void *p) {
    uint8_t *b = (uint8_t *)m; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    return (uint8_t *)p >= b && (uint8_t *)p < b + nt->OptionalHeader.SizeOfImage;
}
int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: focus_test <overlay dll> pass|block\n"); return 2; }
    bool expect_block = !strcmp(argv[2], "block");
    HMODULE loader = LoadLibraryA("bngrw.dll");
    if (!loader) { printf("FAIL load bngrw.dll %lu\n", GetLastError()); return 1; }
    auto key = (SHORT (*)(int))GetProcAddress(loader, "fl_key");
    auto buttons = (ULONG (*)(void))GetProcAddress(loader, "fl_buttons");
    auto value = (ULONG (*)(USHORT))GetProcAddress(loader, "fl_value");
    // the loader reads a value before the overlay loads: 200 is what "frozen" axes keep
    HMODULE ov = LoadLibraryA(argv[1]);
    if (!ov) { printf("FAIL load overlay %lu\n", GetLastError()); return 1; }
    Sleep(1500);
    int fails = 0;
    const char *names[] = { "GetAsyncKeyState", "GetRawInputData", "HidP_GetUsages", "HidP_GetUsageValue" };
    for (const char *n : names) {
        bool ok = inside(ov, iat_entry(loader, n));
        printf("%s %s patched\n", ok ? "ok  " : "FAIL", n); fails += !ok;
    }
    key(VK_SHIFT);
    value(0x32);   // Z read once (remembered if input passes)
    ULONG nb = buttons(), x = value(0x30), y = value(0x31), hat = value(0x39), z = value(0x32);
    printf("buttons=%lu x=%lu y=%lu hat=%lu z=%lu\n", nb, x, y, hat, z);
    bool ok = expect_block ? (nb == 0 && x == 128 && y == 128 && hat == 8 && z == 200)
                           : (nb == 3 && x == 200 && y == 200 && hat == 200 && z == 200);
    printf("%s values (%s)\n", ok ? "ok  " : "FAIL", expect_block ? "blocked" : "passed through"); fails += !ok;
    printf(fails ? "FAILED\n" : "ALL OK\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails ? 1 : 0);   // the overlay's threads keep running otherwise
    return 0;
}
#endif
