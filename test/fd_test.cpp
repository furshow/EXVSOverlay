// Runs framedata.cpp OUTSIDE the game against the running vsac29_Release.exe (read-only), printing what the frame
// meter would show. Build: see build.sh (fd_test.exe). Usage: fd_test.exe [seconds]
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "framedata.h"

void fd_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    setvbuf(stdout, NULL, _IONBF, 0);
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"vsac29_Release.exe")) pid = pe.th32ProcessID;
    CloseHandle(snap);
    if (!pid) { printf("game not running\n"); return 1; }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    HMODULE mods[1]; DWORD need;
    if (!h || !EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_64BIT)) { printf("can't open the game\n"); return 1; }
    printf("pid %lu exe base %p\n", pid, (void *)mods[0]);
    fd_init(h, (uintptr_t)mods[0]);
    const char *last_status = "";
    FdSummary last[2] = {};
    DWORD t0 = GetTickCount(), lastline = 0;
    static const char CH[] = " .SAR*D#";   // none free startup active recovery hitstun down guard
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        fd_update(true);
        if (strcmp(fd_status(), last_status)) { last_status = fd_status(); printf("[%5.1f s] status: %s\n", (GetTickCount() - t0) / 1000.0, last_status); }
        if (fd_active()) {
            for (int s = 0; s < 2; s++) {
                const FdSummary *x = fd_summary(s);
                if (memcmp(x, &last[s], sizeof(*x))) {
                    last[s] = *x;
                    int u = s ? fd_target() : fd_player();
                    printf("[%5.1f s] %s %-10s %s startup %d  total %d  %s%s\n", (GetTickCount() - t0) / 1000.0, s ? "TARGET" : "PLAYER", fd_unit_code(u),
                           x->hit ? "HIT " : "WHIFF", x->startup, x->total, x->chained ? "chained " : "",
                           x->adv_known ? (x->adv >= 0 ? "adv +" : "adv ") : "", x->adv_known ? 0 : 0);
                    if (x->adv_known) printf("            advantage %+d\n", x->adv);
                }
            }
            if (GetTickCount() - lastline > 3000) {
                lastline = GetTickCount();
                for (int s = 0; s < 2; s++) {
                    int u = s ? fd_target() : fd_player(); char row[61];
                    for (int k = 0; k < 60; k++) row[k] = CH[fd_cell(u, 59 - k)]; row[60] = 0;
                    float b[128][3]; int nb = fd_bones(u, b, 128), on = 0;
                    for (int k = 0; k < nb; k++) { float x, y; if (fd_project(b[k], &x, &y) && x > -1 && x < 1 && y > -1 && y < 1) on++; }
                    printf("   %s %-10s |%s|  bones %d (%d on screen)\n", s ? "T" : "P", fd_unit_code(u), row, nb, on);
                }
            }
        }
        Sleep(4);
    }
    return 0;
}
