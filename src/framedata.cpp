// Training tools: live frame meter + skeleton view (EXVSIB client 1.13.30 only, Training / Practice modes only).
//
// Everything is READ from the game's memory (nothing is written). Layout found by live RAM research
// (see the project's FINDINGS.md):
//   unit object = class CUnitTaskCharacterCHR_<series>_<unit> (one per unit; objects are pooled and reused)
//     +0x448 game clock (+26 per frame)        +0x4c0 position (x, y, z)
//     +0x514 movement state (20/21/4/5 = down)  +0x580 guarding flag
//     +0x2ee0 hit timer (60 on each hit, -1 per frame; control comes back at 4)
//     +0x2f48 HP                                 +0x2f84 down value
//     [+0x3218] action controller: +0x55c action hash (0 = free), +0x560 action serial (+1 per new action)
//     [[[+0x31e8]+0x1e8]+0x3a0]+0x70 -> bone matrices, world space, 4x4 row-major (translation in row 3)
//   camera: [LegacyDOFImpl + 0x18] -> +0x2b0 view matrix, +0x2f0 projection (row vectors: p * view * proj)
//   mode: a live CAcSeqBattleFlow_Training or _Practice object (anything else, online included = off)
//
// All reads go through rd(), which uses ReadProcessMemory: a bad pointer gives "false", never a crash.
// The same code runs outside the game in test/fd_test.cpp (FD_TEST) against the running game.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <vector>
#include <algorithm>
#include "framedata.h"
#include "unit_names.h"
#include "unit_hurtboxes.h"
#include "unit_hitboxes.h"
#include "unit_projectiles.h"
#ifndef FD_TEST
#include "imgui.h"
#include "menu.h"
#endif

// ---------------------------------------------------------------------------------------------
// memory access
// ---------------------------------------------------------------------------------------------
static HANDLE g_proc;          // GetCurrentProcess() in the game, an opened handle in the test
static uintptr_t g_exe;        // base of vsac29_Release.exe

static bool rd(uintptr_t a, void *out, size_t n) {
    SIZE_T got = 0;
    return a > 0x10000 && ReadProcessMemory(g_proc, (LPCVOID)a, out, n, &got) && got == n;
}
template <typename T> static T rv(uintptr_t a, T def = T()) { T v; return rd(a, &v, sizeof(v)) ? v : def; }
static uintptr_t rp(uintptr_t a) { return rv<uintptr_t>(a, 0); }

static const DWORD EXE_STAMP = 0x69e2081b, EXE_IMAGE = 0x311f000;   // vsac29_Release.exe 1.13.30 (client)

enum Kind { K_UNIT, K_FLOW_OK, K_FLOW_OTHER, K_DOF, K_CORE_OK, K_PROJ, K_POOL };
struct VT { uintptr_t addr; int kind; char name[40]; bool assist; };   // assist: an assist automaton (its reach was measured)
static std::vector<VT> g_vt;    // sorted by addr
static uintptr_t g_vt_lo, g_vt_hi;
static const char *g_status = "Starting...";
static bool g_supported;

#ifndef FD_TEST
void fd_log(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    ov_log("%s", b);
}
#endif

// ---------------------------------------------------------------------------------------------
// RTTI: class name -> vtable, from the exe image (type descriptor -> complete object locator -> vtable)
// ---------------------------------------------------------------------------------------------
static bool classify(const char *n, int *kind) {
    if (!strncmp(n, ".?AVCUnitTaskCharacterCHR_", 26)) {
        if (!strncmp(n + 26, "501", 3)) return false;          // 501xxx = assist units
        *kind = K_UNIT; return true;
    }
    if (!strcmp(n, ".?AVCAcSeqBattleFlow_Training@SEQ@@") || !strcmp(n, ".?AVCAcSeqBattleFlow_Practice@SEQ@@")) { *kind = K_FLOW_OK; return true; }
    if (!strncmp(n, ".?AVCAcSeqBattleFlow_", 21) && strncmp(n, ".?AVCAcSeqBattleFlow_Root", 25) && strncmp(n, ".?AVCAcSeqBattleFlowBase", 24)) { *kind = K_FLOW_OTHER; return true; }
    if (!strncmp(n, ".?AVLegacyDOFImpl@", 18)) { *kind = K_DOF; return true; }
    if (!strncmp(n, ".?AVCUnitTaskActorProj", 22)) { *kind = K_PROJ; return true; }
    if (!strncmp(n, ".?AVCUnitTaskAutomata", 21)) { *kind = K_PROJ; return true; }    // thrown weapons, bits, assists
    if (!strcmp(n, ".?AVCBattleUnitTaskDepotPoolProjectile@GAM@VDK@@")) { *kind = K_POOL; return true; }   // projectiles (pooled)
    // the Training / Practice battle sequences themselves (second way to tell the mode)
    if (!strcmp(n, ".?AVCAcSecTrainingCore@SEQ@@") || !strcmp(n, ".?AVCAcSecPracticeBattleCore@SEQ@@")) { *kind = K_CORE_OK; return true; }
    return false;
}

static bool build_vtables(void) {
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!rd(g_exe, &dos, sizeof(dos)) || !rd(g_exe + dos.e_lfanew, &nt, sizeof(nt))) { g_status = "Can't read the game exe."; return false; }
    if (nt.FileHeader.TimeDateStamp != EXE_STAMP || nt.OptionalHeader.SizeOfImage != EXE_IMAGE) {
        g_status = "This game version isn't supported (made for the 1.13.30 client)."; return false;
    }
    IMAGE_SECTION_HEADER sec[24]; int ns = nt.FileHeader.NumberOfSections; if (ns > 24) ns = 24;
    rd(g_exe + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader, sec, ns * sizeof(sec[0]));
    uint32_t rdata_va = 0, rdata_sz = 0, data_va = 0, data_sz = 0;
    for (int i = 0; i < ns; i++) {
        if (!memcmp(sec[i].Name, ".rdata", 7)) { rdata_va = sec[i].VirtualAddress; rdata_sz = sec[i].Misc.VirtualSize; }
        if (!memcmp(sec[i].Name, ".data", 6)) { data_va = sec[i].VirtualAddress; data_sz = sec[i].Misc.VirtualSize; }
    }
    if (!rdata_sz || !data_sz) { g_status = "Unexpected exe layout."; return false; }
    std::vector<uint8_t> data(data_sz), rdata(rdata_sz);
    if (!rd(g_exe + data_va, data.data(), data_sz) || !rd(g_exe + rdata_va, rdata.data(), rdata_sz)) { g_status = "Can't read the game exe."; return false; }

    struct TD { uint32_t rva; int kind; const char *name; };
    std::vector<TD> tds;
    for (uint32_t i = 0x10; i + 8 < data_sz; i++) {
        if (data[i] != '.' || data[i + 1] != '?' || data[i + 2] != 'A' || data[i + 3] != 'V') continue;
        const char *n = (const char *)&data[i]; int k;
        if (memchr(n, 0, data_sz - i) && classify(n, &k)) tds.push_back({ data_va + i - 0x10, k, n });
    }
    std::sort(tds.begin(), tds.end(), [](const TD &a, const TD &b) { return a.rva < b.rva; });
    auto find_td = [&](uint32_t rva) -> const TD * {
        auto it = std::lower_bound(tds.begin(), tds.end(), rva, [](const TD &t, uint32_t v) { return t.rva < v; });
        return it != tds.end() && it->rva == rva ? &*it : nullptr;
    };
    struct COL { uint32_t rva; const TD *td; };
    std::vector<COL> cols;
    for (uint32_t i = 0; i + 0x18 <= rdata_sz; i += 4) {
        uint32_t sig, off, td, self;
        memcpy(&sig, &rdata[i], 4); if (sig != 1) continue;
        memcpy(&off, &rdata[i + 4], 4); memcpy(&td, &rdata[i + 0xc], 4); memcpy(&self, &rdata[i + 0x14], 4);
        if (off || self != rdata_va + i) continue;
        if (const TD *t = find_td(td)) cols.push_back({ rdata_va + i, t });
    }
    std::sort(cols.begin(), cols.end(), [](const COL &a, const COL &b) { return a.rva < b.rva; });
    g_vt.clear();
    for (uint32_t i = 0; i + 16 <= rdata_sz; i += 8) {
        uint64_t q; memcpy(&q, &rdata[i], 8);
        if (q < g_exe + rdata_va || q >= g_exe + rdata_va + rdata_sz) continue;
        uint32_t r = (uint32_t)(q - g_exe);
        auto it = std::lower_bound(cols.begin(), cols.end(), r, [](const COL &c, uint32_t v) { return c.rva < v; });
        if (it == cols.end() || it->rva != r) continue;
        VT v = {}; v.addr = g_exe + rdata_va + i + 8; v.kind = it->td->kind;
        v.assist = v.kind == K_PROJ && !strncmp(it->td->name, ".?AVCUnitTaskAutomata", 21) && strstr(it->td->name, "Assist");
        const char *n = it->td->name + 4;                 // skip ".?AV"
        if (v.kind == K_UNIT) n += 22;                    // skip "CUnitTaskCharacterCHR_" -> "069GQUUUX_001GQUUUX_001@EXVS2@@"
        size_t len = strcspn(n, "@"); if (len > sizeof(v.name) - 1) len = sizeof(v.name) - 1;
        memcpy(v.name, n, len);
        g_vt.push_back(v);
    }
    std::sort(g_vt.begin(), g_vt.end(), [](const VT &a, const VT &b) { return a.addr < b.addr; });
    int units = 0, ok = 0, dof = 0;
    for (auto &v : g_vt) { units += v.kind == K_UNIT; ok += v.kind == K_FLOW_OK; dof += v.kind == K_DOF; }
    if (units < 100 || ok < 1 || dof < 1) { g_status = "Couldn't find the game's classes (unexpected exe)."; return false; }
    g_vt_lo = g_vt.front().addr; g_vt_hi = g_vt.back().addr;
    fd_log("framedata: %d unit classes, %zu vtables total", units, g_vt.size());
    return true;
}

static const VT *vt_of(uintptr_t v) {
    if (v < g_vt_lo || v > g_vt_hi) return nullptr;
    auto it = std::lower_bound(g_vt.begin(), g_vt.end(), v, [](const VT &a, uintptr_t x) { return a.addr < x; });
    return it != g_vt.end() && it->addr == v ? &*it : nullptr;
}

// ---------------------------------------------------------------------------------------------
// battle discovery (background thread): one pass over the heap for the vtables above
// ---------------------------------------------------------------------------------------------
struct Found { uintptr_t addr; int kind; char name[40]; uintptr_t bones; int nbones; };
struct Battle { std::vector<Found> units; std::vector<uintptr_t> projs; std::vector<std::pair<uintptr_t, uintptr_t>> regions; int proj_al[4] = {}; uintptr_t pool = 0; uintptr_t dof = 0, flow_ok = 0, core = 0; bool from_core = false; int flow_other = 0, nlog = 0; DWORD tick = 0; };
static SRWLOCK g_lock = SRWLOCK_INIT;
static Battle g_battle;          // last scan result (under g_lock)

static bool playable(const char *full);   // unit id in the playable-unit table (pooled NPCs / leftovers are not)

static void scan_heap(Battle &b) {
    struct Flow { uintptr_t at; const VT *v; };
    std::vector<Flow> flows;
    std::vector<uint8_t> buf(1 << 20);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000;
    while (a < 0x7fffffff0000ull && VirtualQueryEx(g_proc, (LPCVOID)a, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        // game objects live in private read/write memory (never in GPU-mapped / write-combined memory: slow to read)
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t p = base; p < end; p += buf.size()) {
                size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - p);
                SIZE_T got = 0;
                if (!ReadProcessMemory(g_proc, (LPCVOID)p, buf.data(), n, &got)) continue;
                const uint64_t *q = (const uint64_t *)buf.data();
                for (size_t i = 0; i < got / 8; i++) {
                    if (q[i] < g_vt_lo || q[i] > g_vt_hi) continue;
                    const VT *v = vt_of(q[i]);
                    if (!v) continue;
                    uintptr_t at = p + i * 8;
                    if (v->kind == K_UNIT) {
                        if (at & 0xffff) continue;                       // unit objects start a 64 KB-aligned block
                        if (!playable(v->name)) continue;
                        Found f = { at, K_UNIT, {} }; memcpy(f.name, v->name, sizeof(f.name)); b.units.push_back(f);
                    } else if (v->kind == K_DOF) { if (!(at & 0xfff) || !b.dof || (b.dof & 0xfff)) b.dof = at; }
                    else if (v->kind == K_CORE_OK) b.core = at;
                    else if (v->kind == K_PROJ) {
                        int al = !(at & 0xffff) ? 3 : !(at & 0xfff) ? 2 : !(at & 0xff) ? 1 : 0;
                        b.proj_al[al]++;
                        if (al >= 2 && b.projs.size() < 512) b.projs.push_back(at);
                    } else if (v->kind == K_POOL) b.pool = at;
                    else flows.push_back({ at, v });
                }
            }
        }
        a = end;
    }
    // The game keeps a catalog with one object of EVERY battle flow (small heap objects, packed together) plus loose
    // leftovers. The flow that is really running is a big object that starts on its own page (seen in every run):
    // only page-aligned flow objects count.
    std::sort(flows.begin(), flows.end(), [](const Flow &x, const Flow &y) { return x.at < y.at; });
    for (size_t i = 0; i < flows.size(); i++) {
        if (flows[i].at & 0xfff) continue;
        if (flows[i].v->kind == K_FLOW_OK) b.flow_ok = flows[i].at; else b.flow_other++;
        if (b.nlog++ < 6) fd_log("framedata:   running battle flow: %s at %p", flows[i].v->name, (void *)flows[i].at);
    }
    // Some runs have no page-aligned flow at all. Then the live Training / Practice sequence object decides,
    // as long as no other mode's flow is running (online stays off).
    // (confirmed by the discovery loop: at least 2 units with a live skeleton, else leftovers at boot fool it)
    if (!b.flow_ok && !b.flow_other && b.core && b.units.size() >= 2) { b.flow_ok = b.core; b.from_core = true; }
    static int diag;
    if (!b.flow_ok && b.units.size() >= 2 && diag++ < 2) {
        fd_log("framedata: no running flow found; core %p; %zu flow objects:", (void *)b.core, flows.size());
        for (size_t i = 0; i < flows.size() && i < 24; i++) fd_log("framedata:   %p %s", (void *)flows[i].at, flows[i].v->name);
    }
}

static bool still_valid(const Battle &b) {
    auto same = [](uintptr_t at, int kind) { const VT *v = vt_of(rp(at)); return v && v->kind == kind; };
    if (!b.flow_ok || !(same(b.flow_ok, K_FLOW_OK) || same(b.flow_ok, K_CORE_OK)) || !b.dof || !same(b.dof, K_DOF)) return false;
    for (auto &u : b.units) if (!same(u.addr, K_UNIT)) return false;
    return true;
}

static volatile LONG g_want;     // features enabled (set by fd_tick from the menu config)
static volatile LONG g_bones_bad;   // a unit's bone array is missing / stale (set by fd_bones)

// Bone arrays: the pointer path to them differs between units, so they are found by what they are: a run of 4x4
// world matrices (row-major, translation in row 3, column 3 = 0 0 0 1) whose first bone (the root) stands exactly
// on the unit's position. One pass over the heap finds them for every unit.
static unsigned unit_id(const char *n);
static int expected_bones(const char *full) {                              // body skeleton size (from the hurtbox table)
    unsigned id = unit_id(full);
    int lo = 0, hi = (int)(sizeof(HURT_UNITS) / sizeof(HURT_UNITS[0])) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (HURT_UNITS[m].id == id) return HURT_UNITS[m].nbones; if (HURT_UNITS[m].id < id) lo = m + 1; else hi = m - 1; }
    return 0;
}
static void scan_bones(Battle &b) {
    struct P { float x, y, z; } pos[8]; int nu = (int)std::min<size_t>(b.units.size(), 8);
    int expect[8]; uintptr_t cand[8][4] = {}; int ncand[8] = {};
    for (int i = 0; i < nu; i++) expect[i] = expected_bones(b.units[i].name);
    for (int i = 0; i < nu; i++) { if (!rd(b.units[i].addr + 0x4c0, &pos[i], 12)) pos[i].x = 1e30f; b.units[i].bones = 0; b.units[i].nbones = 0; }
    if (!nu) return;
    std::vector<uint8_t> buf((1 << 20) + 128 * 64);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000;
    int left = nu;
    while (left && a < 0x7fffffff0000ull && VirtualQueryEx(g_proc, (LPCVOID)a, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t p = base; p < end && left; p += 1 << 20) {
                size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - p);
                SIZE_T got = 0;
                if (!ReadProcessMemory(g_proc, (LPCVOID)p, buf.data(), n, &got) || got < 64) continue;
                for (int u = 0; u < nu; u++) rd(b.units[u].addr + 0x4c0, &pos[u], 12);   // the units move during the search
                const float *f = (const float *)buf.data(); const uint32_t *w = (const uint32_t *)buf.data();
                size_t cnt = std::min<size_t>(got, 1 << 20) / 4;
                for (size_t i = 0; i + 16 <= cnt; i += 4) {                   // 16-byte aligned candidates
                    if (w[i + 15] != 0x3f800000u || w[i + 3] || w[i + 7] || w[i + 11]) continue;
                    float tx = f[i + 12], ty = f[i + 13], tz = f[i + 14];
                    for (int u = 0; u < nu; u++) {
                        float tol = expect[u] ? 25.0f : 0.75f;                 // known size: near is enough (it moves)
                        if (b.units[u].bones || fabsf(tx - pos[u].x) > tol || fabsf(tz - pos[u].z) > tol || fabsf(ty - pos[u].y) > tol + 10) continue;
                        if (expect[u] && i >= 16 && w[i - 1] == 0x3f800000u && !w[i - 13] && !w[i - 9] && !w[i - 5]) continue;   // not a run start
                        int run = 1;                                           // count the following bone matrices
                        while (run < 128 && i + (size_t)(run + 1) * 16 <= got / 4) {
                            const uint32_t *m = w + i + run * 16; const float *mf = f + i + run * 16;
                            if (m[15] != 0x3f800000u || m[3] || m[7] || m[11]) break;
                            if (fabsf(mf[12] - pos[u].x) > 40 || fabsf(mf[14] - pos[u].z) > 40 || fabsf(mf[13] - pos[u].y) > 60) break;
                            run++;
                        }
                        if (expect[u]) { if (run == expect[u] && ncand[u] < 4) cand[u][ncand[u]++] = p + i * 4; }
                        else if (run >= 16) { b.units[u].bones = p + i * 4; b.units[u].nbones = run; left--; }
                    }
                }
            }
        }
        a = end;
    }
    for (int i = 0; i < nu; i++) {                                             // several runs of that size: nearest root now
        if (b.units[i].bones || !ncand[i]) continue;
        float now[3]; if (!rd(b.units[i].addr + 0x4c0, now, 12)) continue;
        float best = 1e30f;
        for (int c = 0; c < ncand[i]; c++) {
            float r[16]; if (!rd(cand[i][c], r, 64)) continue;
            float d = fabsf(r[12] - now[0]) + fabsf(r[14] - now[2]) + fabsf(r[13] - now[1]);
            if (d < best) { best = d; b.units[i].bones = cand[i][c]; b.units[i].nbones = expect[i]; }
        }
        if (best > 20) { b.units[i].bones = 0; b.units[i].nbones = 0; }
    }
    for (int i = 0; i < nu; i++) fd_log("framedata: bones of %s: %d at %p", b.units[i].name, b.units[i].nbones, (void *)b.units[i].bones);
}

// Weapon parts with their own skeleton (e.g. the long beam saber): a separate run of world matrices near the unit.
// Found on request (the first time a move needs it) by its layout: every bone's distance to its parent must match
// the rest pose (same ratio for all bones, the unit's scale). Runs on this thread; the result is picked up per frame.
static volatile LONG g_part_req;     // 1 = request pending
static uintptr_t g_req_unit; static unsigned short g_req_part;
static volatile LONG g_part_ready;   // 1 = result below ready
static uintptr_t g_res_unit, g_res_addr[4]; static unsigned short g_res_part;

static bool part_matches(const float *f, const HitPart &hp) {
    const HitSig *sg = &HIT_SIGS[hp.first_sig];
    float ratio = 0, best = 0;
    for (int k = 1; k < hp.nbones; k++) if (sg[k].parent >= 0 && sg[k].dist > best) {
        const float *a = f + k * 16 + 12, *c = f + sg[k].parent * 16 + 12;
        float d = sqrtf((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2]));
        best = sg[k].dist; ratio = d / sg[k].dist;
    }
    if (best < 0.5f || ratio < 0.5f || ratio > 2.0f) return false;
    float stretch = 0, wire = -1;
    for (int k = 1; k < hp.nbones; k++) {
        if (sg[k].parent < 0) continue;
        const float *a = f + k * 16 + 12, *c = f + sg[k].parent * 16 + 12;
        float d = sqrtf((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2]));
        float r = sg[k].dist * ratio;
        if (sg[k].dist < 0.3f) {                                            // zero length at rest (a wire): coiled (short)
            if (d <= 1.0f) continue;                                        // or stretched, all segments the same
            if (wire < 0) wire = d;
            else if (fabsf(d - wire) > 0.2f + 0.1f * wire) return false;
            continue;
        }
        if (fabsf(d - r) <= 0.15f + 0.1f * r) continue;
        if (d < 0.3f) return false;
        float st = d / r;
        if (!stretch) { if (st < 0.2f || st > 10) return false; stretch = st; }
        else if (fabsf(st - stretch) > 0.1f * stretch) return false;
    }
    return true;
}

static int find_part(uintptr_t unit, const HitPart &hp, uintptr_t *found) {
    float pos[3]; if (!rd(unit + 0x4c0, pos, 12)) return 0;
    int nf = 0;
    std::vector<uint8_t> buf((1 << 20) + 64 * 64);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000, fallback = 0; int diag = 0;
    while (a < 0x7fffffff0000ull && VirtualQueryEx(g_proc, (LPCVOID)a, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t p = base; p < end; p += 1 << 20) {
                size_t n = (size_t)std::min<uintptr_t>(buf.size(), end - p);
                SIZE_T got = 0;
                if (!ReadProcessMemory(g_proc, (LPCVOID)p, buf.data(), n, &got) || got < 64) continue;
                const float *f = (const float *)buf.data(); const uint32_t *w = (const uint32_t *)buf.data();
                size_t cnt = got / 4, lim = std::min<size_t>(cnt, (1 << 20) / 4);
                auto ismat = [&](size_t i) { return i + 16 <= cnt && w[i + 15] == 0x3f800000u && !w[i + 3] && !w[i + 7] && !w[i + 11]; };
                for (size_t i = 0; i < lim; i += 4) {
                    if (!ismat(i) || (i >= 16 && ismat(i - 16))) continue;     // start of a run
                    if (fabsf(f[i + 12] - pos[0]) > 25 || fabsf(f[i + 14] - pos[2]) > 25 || fabsf(f[i + 13] - pos[1]) > 30) continue;
                    int run = 1; while (run < hp.nbones + 1 && ismat(i + run * 16)) run++;
                    if (run < hp.nbones) continue;
                    float sc = sqrtf(f[i] * f[i] + f[i + 1] * f[i + 1] + f[i + 2] * f[i + 2]);
                    if (part_matches(f + i, hp)) { if (nf < 4) found[nf++] = p + i * 4; continue; }
                    if (run == hp.nbones && sc > 0.3f) {                         // same size, layout differs (blade grows?)
                        if (!fallback) fallback = p + i * 4;
                        if (diag++ < 3) {
                            char line[300]; int ln = 0; const HitSig *sg = &HIT_SIGS[hp.first_sig];
                            for (int k = 1; k < hp.nbones && ln < 280; k++) {
                                if (sg[k].parent < 0) continue;
                                const float *x = f + i + k * 16 + 12, *c = f + i + sg[k].parent * 16 + 12;
                                float d = sqrtf((x[0] - c[0]) * (x[0] - c[0]) + (x[1] - c[1]) * (x[1] - c[1]) + (x[2] - c[2]) * (x[2] - c[2]));
                                ln += snprintf(line + ln, sizeof(line) - ln, " %.1f/%.1f", d, sg[k].dist);
                            }
                            fd_log("framedata:   candidate %p (live/rest):%s", (void *)(p + i * 4), line);
                        }
                    }
                }
            }
        }
        a = end;
    }
    if (fallback && nf == 0) found[nf++] = fallback;                         // the loose match only when nothing fits
    return nf;
}

static DWORD WINAPI discovery_thread(LPVOID) {
    if (!build_vtables()) { fd_log("framedata: %s", g_status); return 0; }
    g_supported = true;
    g_status = "Waiting for Training mode...";
    DWORD last_full = 0, last_bones = 0;
    char last_log[200] = "";
    for (;;) {
        Sleep(250);
        if (!g_want) continue;
        if (g_part_req && !g_part_ready) {                                    // a weapon part skeleton is wanted
            uintptr_t u = g_req_unit; unsigned short pid = g_req_part;
            uintptr_t got[4] = {}; int nf = find_part(u, HIT_PARTS[pid], got); uintptr_t at = got[0];
            static int misses;
            if (at || misses++ < 5) fd_log("framedata: weapon part %u (%u bones): %p", pid, HIT_PARTS[pid].nbones, (void *)at);
            g_res_unit = u; g_res_part = pid; for (int q = 0; q < 4; q++) g_res_addr[q] = q < nf ? got[q] : 0;
            InterlockedExchange(&g_part_ready, 1); InterlockedExchange(&g_part_req, 0);
        }
        static DWORD last_main;
        if (GetTickCount() - last_main < 1000) continue;
        last_main = GetTickCount();
        Battle cur;
        AcquireSRWLockShared(&g_lock); cur = g_battle; ReleaseSRWLockShared(&g_lock);
        if (still_valid(cur) && GetTickCount() - last_full < 10000) {
            if (g_bones_bad && GetTickCount() - last_bones > 2000) {          // a skeleton went stale: find them again
                InterlockedExchange(&g_bones_bad, 0); last_bones = GetTickCount();
                scan_bones(cur);
                AcquireSRWLockExclusive(&g_lock); if (g_battle.units.size() == cur.units.size()) g_battle.units = cur.units; ReleaseSRWLockExclusive(&g_lock);
            }
            continue;
        }
        Battle b; scan_heap(b); b.tick = GetTickCount(); last_full = b.tick;
        // keep the bone arrays of units that didn't change; search the rest
        bool need = false;
        for (auto &u : b.units) {
            for (auto &o : cur.units) if (o.addr == u.addr) { u.bones = o.bones; u.nbones = o.nbones; }
            need |= !u.bones;
        }
        if (need && b.flow_ok && !b.flow_other) { scan_bones(b); last_bones = GetTickCount(); }

        if (b.from_core) {
            int alive = 0; for (auto &u : b.units) alive += u.bones != 0;
            if (alive < 2) { b.flow_ok = 0; b.from_core = false; }
        }
        char line[200];
        snprintf(line, sizeof(line), "framedata: scan: %zu units, training/practice %p, other flows %d, camera %p", b.units.size(), (void *)b.flow_ok, b.flow_other, (void *)b.dof);
        if (strcmp(line, last_log)) { fd_log("%s", line); strcpy(last_log, line); }
        AcquireSRWLockExclusive(&g_lock); g_battle = b; ReleaseSRWLockExclusive(&g_lock);
    }
}

// ---------------------------------------------------------------------------------------------
// per-frame tracking
// ---------------------------------------------------------------------------------------------
enum Cell { C_NONE = FD_NONE, C_FREE = FD_FREE, C_STARTUP = FD_STARTUP, C_ACTIVE = FD_ACTIVE, C_RECOVERY = FD_RECOVERY, C_HITSTUN = FD_HITSTUN, C_DOWN = FD_DOWN, C_GUARD = FD_GUARD };
static const int RING = 160;
struct Unit {
    uintptr_t addr; char code[16]; char full[40]; char name[48]; int bones_logged = -1, bones_why = -1;
    uintptr_t bones; int nbones;
    bool init; uint32_t clock; int hp; uint32_t serial, hash;
    // this frame
    int cur_hp, timer, move, guard; uint32_t cur_serial, cur_hash; float pos[3]; bool got_hit;
    // attacker
    int act_start, first_hit, last_hit;
    // hitboxes: the move of the current action (table by action hash) and the frame its hit animation started
    const HitMove *hmove; int hit_start;
    float shot_size, shot_speed; int shot_frame;
    float last_root[3], last_pos[3]; int root_frame, frozen;     // projectile size of the shot action it did last, and when
    uint32_t part_logged; int nparts; unsigned short part_id[8]; uintptr_t part_addr[8][4]; DWORD part_asked[8]; unsigned char part_matched[8];
    // defender
    bool stunned; int hit_frame;
    unsigned char cells[RING]; int head;
};
static std::vector<Unit> g_units;
static uintptr_t g_flow_seen;
static int g_frame;              // absolute game frame (clock / 26)
static FdSummary g_sum[2];       // [0] player, [1] target
static int g_player = -1, g_target = -1;
struct Pending { int att, def, att_free; bool combo; bool active; } g_pend;
static float g_view[16], g_proj[16]; static bool g_cam_ok;

// Hit animation start. A chain (N1 -> N2 -> N3) keeps the first stage's action hash live; every new stage changes
// the serial, so the stage is followed through the table's "next" link. Stages without an approach start their
// hit animation at the stage start. Moves with an approach (homing dash, length depends on the distance) switch to
// the hit animation later: the overlay learns which dword of the unit / action controller first changes at that
// moment, from hits: the switch is between (hit - last) and (hit - first) frames of the move's window. Until it is
// learned, the spheres show from the frame the hit lands.
static const int SPAN_U = 0xC000, SPAN_C = 0x1000, NW = (SPAN_U + SPAN_C) / 4, MAXU = 4;
// Hit animation start of moves with an approach: the first change of a dword after the action starts. Controller
// +0x118 works for most moves (learned the same in every run); moves where it doesn't (e.g. GQuuuuuuX's side long
// saber) learn their own dword from 2 landed hits (the switch is between hit - last and hit - first frames of the
// window). Learned ones are kept in EXVSOverlay.hitstart.txt next to the game ("unit id, move hash, dword").
static const int G_OFF = SPAN_U / 4 + 0x118 / 4;
struct MoveLearn { unsigned uid; uint32_t hash; int off, hits; int votes[NW]; };
static MoveLearn *g_ml[16]; static int g_nml;
static uint32_t g_tr_prev[MAXU][NW]; static int g_tr_first[MAXU][NW]; static unsigned char g_tr_n[MAXU][NW];
static bool g_tr_on[MAXU], g_learn_on[MAXU]; static uint32_t g_tr_v0[MAXU]; static int g_tr_use[MAXU]; static MoveLearn *g_tr_ml[MAXU];

static const HitMove *find_move(const char *full, uint32_t hash);
static uintptr_t part_array(int ui, struct Unit &un, unsigned short pid);
static unsigned unit_id(const char *n);
static int g_hitlog;

static void learn_path(char *out, size_t n) {
    char p[MAX_PATH]; GetModuleFileNameA(NULL, p, MAX_PATH);
    char *sl = strrchr(p, '\\'); if (sl) sl[1] = 0;
    snprintf(out, n, "%sEXVSOverlay.hitstart.txt", p);
}
static MoveLearn *move_learn(unsigned uid, uint32_t hash, bool create) {
    static bool loaded;
    if (!loaded) {                                                          // learned moves from earlier sessions
        loaded = true;
        char path[MAX_PATH + 32]; learn_path(path, sizeof(path));
        if (FILE *f = fopen(path, "r")) {
            unsigned u, h; int o;
            while (g_nml < 16 && fscanf(f, "%u %x %d", &u, &h, &o) == 3) {
                MoveLearn *m = (MoveLearn *)calloc(1, sizeof(MoveLearn));
                if (!m) break;
                m->uid = u; m->hash = h; m->off = o; g_ml[g_nml++] = m;
            }
            fclose(f);
        }
    }
    for (int k = 0; k < g_nml; k++) if (g_ml[k]->uid == uid && g_ml[k]->hash == hash) return g_ml[k];
    if (!create || g_nml >= 16) return NULL;
    MoveLearn *m = (MoveLearn *)calloc(1, sizeof(MoveLearn));
    if (!m) return NULL;
    m->uid = uid; m->hash = hash; m->off = -1; g_ml[g_nml++] = m;
    return m;
}

static bool read_tr(const Unit &u, uint32_t *blk) {
    return rd(u.addr, blk, SPAN_U) && rd(rp(u.addr + 0x3218), blk + SPAN_U / 4, SPAN_C);
}
static uint32_t read_tr1(const Unit &u, int k) {
    return k < SPAN_U / 4 ? rv<uint32_t>(u.addr + k * 4) : rv<uint32_t>(rp(u.addr + 0x3218) + (k - SPAN_U / 4) * 4);
}

static void track_hit_anim(int i, Unit &u, bool new_action) {
    if (new_action) {
        const HitMove *prev = u.hmove;
        bool stage = prev && u.hash == u.cur_hash && prev->next;       // next stage of the same chain
        u.hmove = stage ? find_move(u.full, prev->next) : u.cur_hash ? find_move(u.full, u.cur_hash) : NULL;
        u.hit_start = u.hmove && (!u.hmove->start_anim || u.hmove->hit_anim <= 1) ? g_frame : -1;
        if (i < MAXU) { g_tr_on[i] = false; g_learn_on[i] = false; }
        if (u.hmove)
            for (int sp = 0; sp < u.hmove->count; sp++) {
                const HitSphere &h = HIT_SPHERES[u.hmove->first_sphere + sp];
                if (h.part != 0xffff) { part_array(i, u, h.part); break; }
            }
        if (g_hitlog++ < 60) {
            if (u.hmove) fd_log("framedata: %s action %08x -> move %08x%s: approach %s, frames %d-%d, %d spheres", u.code, u.cur_hash,
                                u.hmove->hash, stage ? " (next stage)" : "", u.hmove->start_anim ? "yes" : "no", u.hmove->first, u.hmove->last, u.hmove->count);
            else fd_log("framedata: %s action %08x: not in the hitbox table", u.code, u.cur_hash);
        }
        if (u.hit_start < 0 && u.hmove && u.hmove->start_anim && u.hmove->count && i < MAXU) {
            MoveLearn *ml = move_learn(unit_id(u.full), u.hmove->hash, false);
            g_tr_ml[i] = ml;
            g_tr_use[i] = ml && ml->off >= 0 ? ml->off : G_OFF;
            g_tr_v0[i] = read_tr1(u, g_tr_use[i]); g_tr_on[i] = true;
            if ((!ml || ml->off < 0) && read_tr(u, g_tr_prev[i])) {             // not learned for this move: watch everything
                memset(g_tr_first[i], 0xff, sizeof(g_tr_first[i])); memset(g_tr_n[i], 0, sizeof(g_tr_n[i]));
                g_learn_on[i] = true;
            }
        }
        return;
    }
    if (u.hmove && u.hmove->count) {                                    // keep a weapon part's skeleton found while the move runs
        const HitSphere &h0 = HIT_SPHERES[u.hmove->first_sphere + u.hmove->count - 1];
        if (h0.part != 0xffff) part_array(i, u, h0.part);
    }
    if (!u.hmove || i >= MAXU) return;
    if (g_tr_on[i] && u.hit_start < 0 && read_tr1(u, g_tr_use[i]) != g_tr_v0[i]) u.hit_start = g_frame;
    if (!g_learn_on[i] || u.first_hit >= 0) return;
    static uint32_t blk[NW];
    if (!read_tr(u, blk)) return;
    for (int k = 0; k < NW; k++)
        if (blk[k] != g_tr_prev[i][k]) {
            if (g_tr_first[i][k] < 0) g_tr_first[i][k] = g_frame;
            if (g_tr_n[i][k] < 255) g_tr_n[i][k]++;
            g_tr_prev[i][k] = blk[k];
        }
}

// the attacker's hit landed this frame (first hit of the action)
static void hit_landed(int i, Unit &u) {
    const HitMove *m = u.hmove;
    if (!m || !m->count || !m->last) return;                           // not a melee move (shots)
    int lo = g_frame - m->last - 1, hi = g_frame - m->first + 1;
    bool fits = u.hit_start >= lo && u.hit_start <= hi;                 // the switch we saw agrees with the hit
    if (i < MAXU && g_learn_on[i] && !fits) {                           // learn this move's own switch
        MoveLearn *ml = g_tr_ml[i] ? g_tr_ml[i] : move_learn(unit_id(u.full), m->hash, true);
        if (ml && ml->off < 0) {
            ml->hits++;
            int best = -1, bv = 0;
            for (int k = 0; k < NW; k++) {
                int f = g_tr_first[i][k];
                if (f >= lo && f <= hi && f > u.act_start && g_tr_n[i][k] <= 4) ml->votes[k]++;
                if (ml->votes[k] > bv) { bv = ml->votes[k]; best = k; }
            }
            fd_log("framedata:   learning the hit start of %08x: %d hits, best +0x%x with %d votes", m->hash, ml->hits, best * 4, bv);
            if (ml->hits >= 2 && bv >= 2 && bv * 3 >= ml->hits * 2) {      // 2 agreeing hits and 2/3 of them
                ml->off = best;
                char path[MAX_PATH + 32]; learn_path(path, sizeof(path));
                if (FILE *f = fopen(path, "a")) { fprintf(f, "%u %08x %d\n", ml->uid, ml->hash, ml->off); fclose(f); }
                fd_log("framedata: move %08x: hit animation start = first change of dword %d", m->hash, ml->off);
            }
        }
        g_learn_on[i] = false;
    }
    if (!fits && m->start_anim && m->hit_anim > 1) u.hit_start = g_frame - m->first;   // approach move: the hit lands on the first active frame
}

int fd_learned(void) { return G_OFF; }


// Projectiles: pooled objects (CUnitTaskActorProj*), found by the heap scan. Research step: position assumed at
// +0x4c0 like the units; one that moved this frame is drawn. Every 30 frames, objects whose memory changed are
// logged with the offsets of changed coordinate-like float triplets (to confirm / find the position).
struct Proj { uintptr_t addr; float pos[3], vel[3]; int moved; float size; int fast; bool assist; uint64_t sum; int changed; };   // size from the table (0 = not known yet), frames seen moving
static std::vector<Proj> g_projs;
static int g_projlog;
static void log_pool(uintptr_t pool, const int *al) {
    static uintptr_t logged; static int n;
    if (!pool || pool == logged || n >= 3) return;
    logged = pool; n++;
    fd_log("framedata: projectile pool %p; projectile objects by alignment: 16:%d 256:%d 4K:%d 64K:%d", (void *)pool, al[0], al[1], al[2], al[3]);
    static uint64_t q[0x800 / 8];
    if (!rd(pool, q, sizeof(q))) return;
    int shown = 0;
    for (int k = 1; k < 0x800 / 8 && shown < 24; k++) {
        uintptr_t a = (uintptr_t)q[k];
        if (a < 0x10000 || a > 0x7fffffffffffull || (a & 7)) continue;
        const VT *v = vt_of(rp(a));
        if (v && v->kind == K_PROJ) { fd_log("framedata:   pool+0x%x -> projectile %p %s", k * 8, (void *)a, v->name); shown++; continue; }
        int hits = 0; uintptr_t first = 0;
        for (int j = 0; j < 16; j++) { uintptr_t e = rp(a + j * 8); const VT *w = e ? vt_of(rp(e)) : NULL; if (w && w->kind == K_PROJ) { hits++; if (!first) first = e; } }
        if (hits) { fd_log("framedata:   pool+0x%x -> array %p: %d/16 projectile pointers (first %p %s)", k * 8, (void *)a, hits, (void *)first, vt_of(rp(first))->name); shown++; }
    }
    if (!shown) fd_log("framedata:   pool: no projectile pointers in its first 0x800 bytes");
}

// The projectile pool (CBattleUnitTaskDepotPoolProjectile) keeps the live projectiles in an array at +0x10
// (begin / end pointers at +0x10 / +0x18 when it is a vector). Read every frame.
static unsigned unit_id(const char *n);
// Size of a projectile: live projectiles do not keep their table row, so the row is matched by the measured speed
// (distance per frame = the speed column, e.g. beam rifle 7.2, charge shot 10). Several rows can share a speed:
// the size most of them have wins. Size column ab606d9e (rifle 0.8, charge shot 2.0, confirmed by live hits).
static const float ASSIST_REACH = 9.0f;   // assist automata: measured, first hits land ~9 from the target's hurt spheres
static float row_size(const ProjRow &r) {
    if (r.a > 0 && r.a <= 30) return r.a;
    if (r.b > 0) return r.b;
    return 0;
}
static const ProjUnit *proj_unit(const char *full) {
    unsigned id = unit_id(full);
    int lo = 0, hi = (int)(sizeof(PROJ_UNITS) / sizeof(PROJ_UNITS[0])) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (PROJ_UNITS[m].id == id) return &PROJ_UNITS[m]; if (PROJ_UNITS[m].id < id) lo = m + 1; else hi = m - 1; }
    return NULL;
}
static void note_shot(Unit &u) {                                          // called every frame of an action
    const ProjUnit *pu = u.cur_hash ? proj_unit(u.full) : NULL;
    if (!pu || !pu->act_count) return;
    int lo = (int)pu->act_first, hi = (int)(pu->act_first + pu->act_count) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (PROJ_ACTS[m].hash == u.cur_hash) { u.shot_size = PROJ_ACTS[m].size; u.shot_speed = PROJ_ACTS[m].speed; u.shot_frame = g_frame; return; }
        if (PROJ_ACTS[m].hash < u.cur_hash) lo = m + 1; else hi = m - 1;
    }
}
// size of a new projectile: the shot of the nearest unit that fired in the last 1.5 s, else by speed
static float proj_size_for_speed(float sp);
static float proj_size(const float *pos, float sp) {
    float best = 1e30f, z = 0;
    for (auto &u : g_units) {
        if (u.shot_size <= 0 || g_frame - u.shot_frame > 90) continue;
        if (u.shot_speed > 0 && fabsf(u.shot_speed - sp) > 0.15f) continue;   // not this shot (another unit's, or an older one)
        float d = 0; for (int j = 0; j < 3; j++) d += (u.pos[j] - pos[j]) * (u.pos[j] - pos[j]);
        if (d < best) { best = d; z = u.shot_size; }
    }
    return z > 0 ? z : proj_size_for_speed(sp);
}

static float proj_size_for_speed(float sp) {
    float sizes[64]; int cnt[64], n = 0;
    for (auto &u : g_units) {
        unsigned id = unit_id(u.full);
        int lo = 0, hi = (int)(sizeof(PROJ_UNITS) / sizeof(PROJ_UNITS[0])) - 1; const ProjUnit *pu = NULL;
        while (lo <= hi) { int m = (lo + hi) / 2; if (PROJ_UNITS[m].id == id) { pu = &PROJ_UNITS[m]; break; } if (PROJ_UNITS[m].id < id) lo = m + 1; else hi = m - 1; }
        if (!pu) continue;
        for (unsigned k = pu->first; k < pu->first + pu->count; k++) {
            const ProjRow &r = PROJ_ROWS[k];
            float z = row_size(r);
            if (z <= 0 || fabsf(r.speed - sp) > 0.06f) continue;
            int j = 0; while (j < n && fabsf(sizes[j] - z) > 1e-3f) j++;
            if (j == n && n < 64) { sizes[n] = z; cnt[n] = 0; n++; }
            if (j < 64) cnt[j]++;
        }
    }
    int best = -1; for (int j = 0; j < n; j++) if (best < 0 || cnt[j] > cnt[best] || (cnt[j] == cnt[best] && sizes[j] < sizes[best])) best = j;
    return best >= 0 ? sizes[best] : 0;
}

static void update_projs(const Battle &bt) {
    std::vector<Proj> next;
    uintptr_t beg = bt.pool ? rp(bt.pool + 0x10) : 0, end = bt.pool ? rp(bt.pool + 0x18) : 0;
    size_t n = end > beg && (end - beg) / 8 <= 2048 && !((end - beg) & 7) ? (end - beg) / 8 : 256;
    static uintptr_t arr[2048];
    if (beg && rd(beg, arr, n * 8)) {
        for (size_t k = 0; k < n && next.size() < 256; k++) {
            uintptr_t o = arr[k];
            if (o < 0x10000 || o > 0x7fffffffffffull || (o & 0xf)) continue;
            const VT *v = vt_of(rp(o));
            if (!v || v->kind != K_PROJ) continue;
            Proj pr = {}; pr.addr = o; pr.moved = -1000; pr.assist = v->assist;
            if (!rd(o + 0x4c0, pr.pos, 12)) continue;
            static uint64_t ob[0x800 / 8];                                       // a live one changes (timers), even standing still
            pr.changed = -1000;
            if (rd(o, ob, sizeof(ob))) { uint64_t h = 1469598103934665603ull; for (auto x : ob) h = (h ^ x) * 1099511628211ull; pr.sum = h; }
            for (auto &q : g_projs) if (q.addr == o && (q.moved == -1000 || g_frame - q.moved < 30)) { pr.size = q.size; pr.fast = q.fast; }   // pooled: a reused object is a new shot
            for (auto &q : g_projs) if (q.addr == o) pr.changed = q.sum != pr.sum ? g_frame : q.changed;
            for (auto &q : g_projs) if (q.addr == o) {
                float d = fabsf(pr.pos[0] - q.pos[0]) + fabsf(pr.pos[1] - q.pos[1]) + fabsf(pr.pos[2] - q.pos[2]);
                pr.moved = d > 0.05f && d < 200 ? g_frame : q.moved;
                for (int j = 0; j < 3; j++) pr.vel[j] = d < 200 ? pr.pos[j] - q.pos[j] : 0;
            }
            if (pr.assist) pr.size = ASSIST_REACH;
            {                                                                   // research: each new kind of projectile once
                static uintptr_t seen_vt[64]; static int nseen;
                bool known = false; for (int z = 0; z < nseen; z++) known |= seen_vt[z] == v->addr;
                if (!known && pr.moved == g_frame && nseen < 64) {
                    seen_vt[nseen++] = v->addr;
                    float sp = sqrtf(pr.vel[0] * pr.vel[0] + pr.vel[1] * pr.vel[1] + pr.vel[2] * pr.vel[2]);
                    fd_log("framedata: new projectile kind %s, speed %.1f", v->name, sp);
                }
            }
            if (pr.moved == g_frame && !pr.size && ++pr.fast >= 2) {           // speed settled: look the size up
                float sp = sqrtf(pr.vel[0] * pr.vel[0] + pr.vel[1] * pr.vel[1] + pr.vel[2] * pr.vel[2]);
                float back[3]; for (int j = 0; j < 3; j++) back[j] = pr.pos[j] - pr.vel[j] * pr.fast;   // about where it appeared
                pr.size = proj_size(back, sp);
                if (pr.fast >= 6 && !pr.size) pr.size = -1;                   // no row with that speed (homing, slowing): default
            }
            next.push_back(pr);
        }
    }
    // research: on the frame a unit gets hit, how far from its hurt spheres are the projectiles / assists that moved
    static int rlog;
    for (int d = 0; d < (int)g_units.size() && rlog < 60; d++) {
        if (!g_units[d].got_hit) continue;
        static float hs[64][4]; int nh = fd_hurt(d, hs, 64);
        if (!nh) continue;
        for (auto &q : next) {
            if (g_frame - q.moved > 3) continue;
            float pt = 1e9f;
            for (int h = 0; h < nh; h++) {
                float ex = hs[h][0] - q.pos[0], ey = hs[h][1] - q.pos[1], ez = hs[h][2] - q.pos[2];
                pt = std::min(pt, sqrtf(ex * ex + ey * ey + ez * ez) - hs[h][3]);
            }
            if (pt > 40) continue;
            const VT *vt = vt_of(rp(q.addr)); rlog++;
            float sp = sqrtf(q.vel[0] * q.vel[0] + q.vel[1] * q.vel[1] + q.vel[2] * q.vel[2]);
            fd_log("framedata: hit by %s: %.2f from the hurt sphere surface, drawn size %.2f, speed %.1f", vt ? vt->name : "?", pt, q.size, sp);
        }
    }
    static size_t maxn;
    if (g_projlog < 12 && next.size() > maxn) { maxn = next.size(); g_projlog++; fd_log("framedata: %zu projectiles live (pool array %zu entries)", next.size(), n); }
    g_projs.swap(next);
}

int fd_proj(float (*out)[4], int max) {
    int k = 0;
    for (auto &p : g_projs) {
        if (k >= max) break;
        if ((g_frame - p.moved > 3 && g_frame - p.changed > 3) || (fabsf(p.pos[0]) < 0.01f && fabsf(p.pos[2]) < 0.01f)) continue;
        out[k][0] = p.pos[0]; out[k][1] = p.pos[1]; out[k][2] = p.pos[2]; out[k][3] = p.size > 0 ? p.size : 1.0f; k++;
    }
    return k;
}

static void push_cell(Unit &u, int c) { u.head = (u.head + 1) % RING; u.cells[u.head] = (unsigned char)c; }

static bool is_down(int move) { return move == 20 || move == 21 || move == 4 || move == 5; }

static void finish_action(int i, int f, bool chained) {
    Unit &u = g_units[i];
    if (u.act_start < 0) return;
    FdSummary s = {};
    s.valid = true; s.total = f - u.act_start;
    s.startup = u.first_hit >= 0 ? u.first_hit - u.act_start + 1 : -1;
    s.hit = u.first_hit >= 0; s.chained = chained; s.adv_known = false;
    if (i == g_player) g_sum[0] = s; else if (i == g_target) g_sum[1] = s;
    if (u.first_hit >= 0 && !chained) {                 // wait for the defender to recover -> advantage
        for (int d = 0; d < (int)g_units.size(); d++)
            if (d != i && g_units[d].stunned && g_units[d].hit_frame >= u.act_start) { g_pend = { i, d, f, false, true }; break; }
    }
    u.act_start = -1;
}

static void step_frame(void) {
    // pass 1: who got hit
    for (auto &u : g_units) u.got_hit = u.init && u.cur_hp < u.hp;
    for (int i = 0; i < (int)g_units.size(); i++) {
        Unit &u = g_units[i];
        if (u.got_hit) { u.stunned = true; u.hit_frame = g_frame; if (g_pend.active && g_pend.def == i) g_pend.combo = true; }
    }
    // pass 2: actions and cells
    for (int i = 0; i < (int)g_units.size(); i++) {
        Unit &u = g_units[i];
        bool acting = u.cur_hash != 0;
        if (acting && (u.hash == 0 || u.cur_serial != u.serial)) {          // a new action / stage
            if (u.act_start >= 0) finish_action(i, g_frame, true);
            u.act_start = g_frame; u.first_hit = u.last_hit = -1;
            if (g_pend.active && g_pend.att == i) g_pend.combo = true;
            track_hit_anim(i, u, true);
        } else if (acting) track_hit_anim(i, u, false);
        if (!acting) u.hmove = NULL;
        if (acting) note_shot(u);
        if (!acting && u.hash != 0) finish_action(i, g_frame, false);
        if (acting && u.act_start >= 0)
            for (int d = 0; d < (int)g_units.size(); d++)
                if (d != i && g_units[d].got_hit) {
                    if (u.first_hit < 0 && g_hitlog < 40) fd_log("framedata:   hit landed %d frames after the action start (hit anim start %d)", g_frame - u.act_start, u.hit_start >= 0 ? g_frame - u.hit_start : -1);
                    if (u.first_hit < 0) { u.first_hit = g_frame; hit_landed(i, u); } u.last_hit = g_frame; }
        int c;
        if (u.stunned && (u.timer > 4 || is_down(u.move))) c = is_down(u.move) ? C_DOWN : C_HITSTUN;
        else {
            if (u.stunned) {                                                 // control is back
                u.stunned = false;
                if (g_pend.active && g_pend.def == i) {
                    int adv = g_frame - g_pend.att_free;
                    int slot = g_pend.att == g_player ? 0 : g_pend.att == g_target ? 1 : -1;
                    if (slot >= 0 && !g_pend.combo) { g_sum[slot].adv_known = true; g_sum[slot].adv = adv; }
                    g_pend.active = false;
                }
            }
            if (u.guard) c = C_GUARD;
            else if (acting) c = u.first_hit < 0 ? C_STARTUP : (u.last_hit == g_frame ? C_ACTIVE : C_RECOVERY);
            else c = C_FREE;
        }
        push_cell(u, c);
        u.hp = u.cur_hp; u.serial = u.cur_serial; u.hash = u.cur_hash; u.init = true;
    }
}

static bool read_unit(Unit &u) {
    uint32_t clock = rv<uint32_t>(u.addr + 0x448);
    float pos[3]; if (!rd(u.addr + 0x4c0, pos, sizeof(pos))) return false;
    uintptr_t ctrl = rp(u.addr + 0x3218);
    u.cur_hp = rv<int>(u.addr + 0x2f48); u.timer = rv<int>(u.addr + 0x2ee0);
    u.move = rv<int>(u.addr + 0x514); u.guard = rv<int>(u.addr + 0x580) == 1;
    u.cur_hash = rv<uint32_t>(ctrl + 0x55c); u.cur_serial = rv<uint32_t>(ctrl + 0x560);
    memcpy(u.pos, pos, sizeof(pos));
    u.clock = clock;
    return true;
}

static void mul_row(const float *v4, const float *m, float *out) {   // out = v4 * m (row vector)
    for (int c = 0; c < 4; c++) out[c] = v4[0] * m[c] + v4[1] * m[4 + c] + v4[2] * m[8 + c] + v4[3] * m[12 + c];
}

bool fd_project(const float *world, float *ndc_x, float *ndc_y) {
    if (!g_cam_ok) return false;
    float p[4] = { world[0], world[1], world[2], 1 }, v[4], c[4];
    mul_row(p, g_view, v); mul_row(v, g_proj, c);
    if (c[3] <= 0.01f) return false;
    *ndc_x = c[0] / c[3]; *ndc_y = c[1] / c[3];
    return true;
}

void fd_init(HANDLE proc, uintptr_t exe_base) {
    g_proc = proc; g_exe = exe_base;
    HANDLE t = CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

const char *fd_status(void) { return g_status; }
bool fd_active(void) { return g_flow_seen != 0 && !g_units.empty(); }
const FdSummary *fd_summary(int slot) { return slot >= 0 && slot < 2 ? &g_sum[slot] : nullptr; }

void fd_update(bool want) {
    InterlockedExchange(&g_want, want ? 1 : 0);
    if (!want || !g_supported) { if (g_supported && !want) g_status = "Off."; return; }
    Battle b;
    AcquireSRWLockShared(&g_lock); b = g_battle; ReleaseSRWLockShared(&g_lock);
    if (!b.flow_ok || b.flow_other) {
        g_units.clear(); g_flow_seen = 0;
        g_status = b.flow_other && !b.flow_ok ? "Off in this mode (only Training / Practice vs CPU)." : "Waiting for Training mode...";
        return;
    }
    // (re)build the unit list when the scan changed
    bool changed = b.flow_ok != g_flow_seen || b.units.size() != g_units.size();
    for (size_t i = 0; !changed && i < b.units.size(); i++) changed = b.units[i].addr != g_units[i].addr;
    if (!changed)
        for (size_t i = 0; i < b.units.size(); i++) { g_units[i].bones = b.units[i].bones; g_units[i].nbones = b.units[i].nbones; }
    if (changed) {
        g_units.clear();
        for (auto &f : b.units) {
            Unit u = {}; u.addr = f.addr; u.act_start = -1; u.first_hit = u.last_hit = -1; u.bones = f.bones; u.nbones = f.nbones;
            size_t n = strcspn(f.name, "_"); if (n > sizeof(u.code) - 1) n = sizeof(u.code) - 1;
            memcpy(u.code, f.name, n);
            memcpy(u.full, f.name, sizeof(u.full) - 1);
            g_units.push_back(u);
        }
        g_hitlog = 0;
        g_flow_seen = b.flow_ok; g_pend.active = false; g_sum[0] = g_sum[1] = FdSummary();
    }
    // camera
    uintptr_t view = rp(b.dof + 0x18);
    g_cam_ok = rd(view + 0x2b0, g_view, sizeof(g_view)) && rd(view + 0x2f0, g_proj, sizeof(g_proj)) && fabsf(g_proj[11] + 1) < 0.01f;
    // units
    bool any_new = false; uint32_t clock = 0;
    for (auto &u : g_units) if (read_unit(u)) clock = u.clock;
    int frame = (int)(clock / 26);
    if (frame != g_frame) { any_new = true; }
    if (!any_new) { g_status = "Active."; return; }
    int steps = g_frame && frame > g_frame ? std::min(frame - g_frame, 8) : 1;
    g_frame = frame;
    // player = unit nearest to the camera, target = other unit nearest to screen centre
    if (g_cam_ok) {
        float cam[3];
        for (int j = 0; j < 3; j++) cam[j] = -(g_view[12] * g_view[j * 4 + 0] + g_view[13] * g_view[j * 4 + 1] + g_view[14] * g_view[j * 4 + 2]);
        float best = 1e30f; g_player = -1;
        for (int i = 0; i < (int)g_units.size(); i++) {
            float d = 0; for (int j = 0; j < 3; j++) d += (g_units[i].pos[j] - cam[j]) * (g_units[i].pos[j] - cam[j]);
            if (d < best) { best = d; g_player = i; }
        }
        best = 1e30f; g_target = -1;
        for (int i = 0; i < (int)g_units.size(); i++) {
            if (i == g_player) continue;
            float w[3] = { g_units[i].pos[0], g_units[i].pos[1] + 10, g_units[i].pos[2] }, x, y;
            if (fd_project(w, &x, &y) && x * x + y * y < best) { best = x * x + y * y; g_target = i; }
        }
    }
    for (int s = 0; s < steps; s++) step_frame();
    log_pool(b.pool, b.proj_al);
    update_projs(b);
    g_status = "Active.";
}

int fd_unit_count(void) { return (int)g_units.size(); }
int fd_player(void) { return g_player; }
int fd_target(void) { return g_target; }
const char *fd_unit_code(int i) { return i >= 0 && i < (int)g_units.size() ? g_units[i].code : ""; }

// "069GQUUUX_001GQUUUX_001" -> unit id 69001001
static unsigned unit_id(const char *n) {
    unsigned a = 0, b = 0, c = 0;
    const char *p1 = strchr(n, '_'), *p2 = p1 ? strchr(p1 + 1, '_') : NULL;
    if (sscanf(n, "%3u", &a) == 1 && p1 && sscanf(p1 + 1, "%3u", &b) == 1 && p2 && sscanf(p2 + 1, "%3u", &c) == 1) return a * 1000000 + b * 1000 + c;
    return 0;
}

static bool playable(const char *full) {
    unsigned id = unit_id(full);
    int lo = 0, hi = (int)(sizeof(UNIT_NAMES) / sizeof(UNIT_NAMES[0])) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (UNIT_NAMES[m].id == id) return true; if (UNIT_NAMES[m].id < id) lo = m + 1; else hi = m - 1; }
    return false;
}

// unit id -> "GQuuuuuuX" (table generated from the game's unit names)
const char *fd_unit_name(int i) {
    if (i < 0 || i >= (int)g_units.size()) return "?";
    Unit &u = g_units[i];
    if (!u.name[0]) {
        unsigned id = unit_id(u.full);
        if (id) {
            int lo = 0, hi = (int)(sizeof(UNIT_NAMES) / sizeof(UNIT_NAMES[0])) - 1;
            while (lo <= hi) {
                int m = (lo + hi) / 2;
                if (UNIT_NAMES[m].id == id) { snprintf(u.name, sizeof(u.name), "%s", UNIT_NAMES[m].name); break; }
                if (UNIT_NAMES[m].id < id) lo = m + 1; else hi = m - 1;
            }
        }
        if (!u.name[0]) snprintf(u.name, sizeof(u.name), "%s", u.code);
    }
    return u.name;
}
int fd_cell(int i, int back) {      // back = 0 newest
    if (i < 0 || i >= (int)g_units.size() || back >= RING) return C_NONE;
    return g_units[i].cells[(g_units[i].head - back + RING) % RING];
}

// The skeleton array moves when the model changes (transformation, reset): its root must stand on the unit.
static bool bones_here(Unit &un, const float *root) {
    bool ok = root[15] == 1.0f && fabsf(root[12] - un.pos[0]) <= 15 && fabsf(root[14] - un.pos[2]) <= 15 && fabsf(root[13] - un.pos[1]) <= 20;
    if (ok && un.root_frame != g_frame) {                                    // once per frame: frozen while the unit moves?
        un.root_frame = g_frame;
        bool same = root[12] == un.last_root[0] && root[13] == un.last_root[1] && root[14] == un.last_root[2];
        bool moved = fabsf(un.pos[0] - un.last_pos[0]) + fabsf(un.pos[2] - un.last_pos[2]) > 0.3f;
        un.frozen = same && moved ? un.frozen + 1 : same ? un.frozen : 0;
        memcpy(un.last_root, root + 12, 12); memcpy(un.last_pos, un.pos, 12);
        if (un.frozen > 8) ok = false;
    }
    if (ok) return true;
    InterlockedExchange(&g_bones_bad, 1);
    un.nbones = 0; un.bones = 0; un.frozen = 0;
    return false;
}

// Hurt spheres (body spheres the unit is hit on, from the game's params via tools/make_hurtboxes.py), placed on the
// live bones: centre = offset * bone matrix. out[k] = x, y, z, radius (world). Only if the skeleton size matches.
int fd_hurt(int i, float (*out)[4], int max) {
    if (i < 0 || i >= (int)g_units.size()) return 0;
    Unit &un = g_units[i];
    unsigned id = unit_id(un.full);
    int lo = 0, hi = (int)(sizeof(HURT_UNITS) / sizeof(HURT_UNITS[0])) - 1; const HurtUnit *hu = NULL;
    while (lo <= hi) { int m = (lo + hi) / 2; if (HURT_UNITS[m].id == id) { hu = &HURT_UNITS[m]; break; } if (HURT_UNITS[m].id < id) lo = m + 1; else hi = m - 1; }
    if (!hu || !un.bones || un.nbones < hu->nbones) return 0;
    static float m[128][16];
    int nb = std::min((int)hu->nbones, 128);
    if (!rd(un.bones, m, nb * 64) || !bones_here(un, m[0])) return 0;
    int k = 0;
    for (int s = 0; s < hu->count && k < max; s++) {
        const HurtSphere &h = HURT_SPHERES[hu->first + s];
        if (h.bone >= nb) continue;
        const float *M = m[h.bone];
        float v[4] = { h.x, h.y, h.z, 1 }, w[4];
        mul_row(v, M, w);
        float scale = sqrtf(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]);
        if (scale < 0.01f) continue;                                  // part hidden (bone scaled to ~0)
        out[k][0] = w[0]; out[k][1] = w[1]; out[k][2] = w[2]; out[k][3] = h.r * scale; k++;
    }
    return k;
}

static const HitMove *find_move(const char *full, uint32_t hash) {
    unsigned id = unit_id(full);
    int lo = 0, hi = (int)(sizeof(HIT_UNITS) / sizeof(HIT_UNITS[0])) - 1; const HitUnit *hu = NULL;
    while (lo <= hi) { int m = (lo + hi) / 2; if (HIT_UNITS[m].id == id) { hu = &HIT_UNITS[m]; break; } if (HIT_UNITS[m].id < id) lo = m + 1; else hi = m - 1; }
    if (!hu) return NULL;
    lo = (int)hu->first; hi = (int)(hu->first + hu->count) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (HIT_MOVES[m].hash == hash) return &HIT_MOVES[m]; if (HIT_MOVES[m].hash < hash) lo = m + 1; else hi = m - 1; }
    return NULL;
}

bool fd_has_hitboxes(int i) {
    if (i < 0 || i >= (int)g_units.size()) return false;
    unsigned id = unit_id(g_units[i].full);
    for (auto &h : HIT_UNITS) if (h.id == id) return true;
    return false;
}

// Hit spheres of the move the unit is doing, during its active frames (frames of the hit animation).
static uintptr_t part_array(int ui, Unit &un, unsigned short pid) {
    if (g_part_ready) {                                                     // pick up a finished search: add its candidates
        if (g_res_unit == un.addr) {
            int j = 0; while (j < un.nparts && un.part_id[j] != g_res_part) j++;
            if (j == un.nparts && j < 8) { un.part_id[j] = g_res_part; memset(un.part_addr[j], 0, sizeof(un.part_addr[j])); un.part_asked[j] = GetTickCount(); un.nparts++; }
            if (j < 8)
                for (int q = 0; q < 4 && g_res_addr[q]; q++) {
                    int c = 0; while (c < 4 && un.part_addr[j][c] && un.part_addr[j][c] != g_res_addr[q]) c++;
                    if (c == 4) { memmove(un.part_addr[j], un.part_addr[j] + 1, 3 * sizeof(uintptr_t)); c = 3; un.part_matched[j] = (unsigned char)((un.part_matched[j] >> 1) & 7); }   // drop the oldest
                    if (un.part_addr[j][c] != g_res_addr[q]) un.part_matched[j] &= (unsigned char)~(1 << c);
                    un.part_addr[j][c] = g_res_addr[q];
                }
            InterlockedExchange(&g_part_ready, 0);
        } else {
            bool mine = false; for (auto &o : g_units) mine |= o.addr == g_res_unit;
            if (!mine) InterlockedExchange(&g_part_ready, 0);
        }
    }
    int j = 0; while (j < un.nparts && un.part_id[j] != pid) j++;
    if (j < un.nparts) {
        static float pm[64][16];
        int nb = std::min((int)HIT_PARTS[pid].nbones, 64);
        uintptr_t loose = 0, best = 0; float best_d = 1e30f;
        for (int c = 0; c < 4 && un.part_addr[j][c]; c++) {                 // the copy that is shown now
            if (!rd(un.part_addr[j][c], pm, nb * 64) || pm[0][15] != 1.0f) continue;
            if (fabsf(pm[0][12] - un.pos[0]) > 30 || fabsf(pm[0][14] - un.pos[2]) > 30) continue;   // its base stays on the unit
            float sc = sqrtf(pm[0][0] * pm[0][0] + pm[0][1] * pm[0][1] + pm[0][2] * pm[0][2]);
            if (sc <= 0.3f || sc >= 3) continue;
            if (part_matches(&pm[0][0], HIT_PARTS[pid])) {                  // several fit: the one hanging closest to the unit
                un.part_matched[j] |= (unsigned char)(1 << c);
                float dd = fabsf(pm[0][12] - un.pos[0]) + fabsf(pm[0][14] - un.pos[2]) + fabsf(pm[0][13] - un.pos[1]);
                if (dd < best_d) { best_d = dd; best = un.part_addr[j][c]; }
                continue;
            }
            if (!loose) loose = un.part_addr[j][c];
        }
        if (best) return best;
        bool any_match = false;                                              // a loose copy only if no copy ever matched
        for (int c = 0; c < 4 && un.part_addr[j][c]; c++) any_match |= (un.part_matched[j] >> c) & 1;
        if (loose && !any_match) return loose;
    }
    if (j == un.nparts) { if (j >= 8) return 0; un.part_id[j] = pid; memset(un.part_addr[j], 0, sizeof(un.part_addr[j])); un.part_asked[j] = 0; un.nparts++; }
    if (!g_part_req && !g_part_ready && GetTickCount() - un.part_asked[j] > 300) {   // none shown: search (adds copies)
        un.part_asked[j] = GetTickCount();
        g_req_unit = un.addr; g_req_part = pid;
        InterlockedExchange(&g_part_req, 1);
    }
    (void)ui;
    return 0;
}

int fd_hit(int i, float (*out)[4], int max) {
    if (i < 0 || i >= (int)g_units.size()) return 0;
    Unit &un = g_units[i];
    const HitMove *mv = un.hmove;
    if (!mv || un.hit_start < 0 || !un.bones) return 0;
    int f = g_frame - un.hit_start;
    if (f < mv->first || f > mv->last) return 0;
    static float m[128][16], pm[64][16];
    int nb = std::min(un.nbones, 128);
    if (!rd(un.bones, m, nb * 64) || !bones_here(un, m[0])) return 0;
    int k = 0; unsigned short cur_part = 0xfffe; bool part_ok = false; int pnb = 0;
    for (int s = 0; s < mv->count && k < max; s++) {
        const HitSphere &h = HIT_SPHERES[mv->first_sphere + s];
        const float *M;
        if (h.part == 0xffff) {
            if (h.bone >= nb) continue;
            M = m[h.bone];
        } else {
            if (h.part != cur_part) {
                cur_part = h.part; pnb = std::min((int)HIT_PARTS[h.part].nbones, 64);
                uintptr_t at = part_array(i, un, h.part);
                part_ok = at && rd(at, pm, pnb * 64);
            }
            if (!part_ok || h.bone >= pnb) continue;
            M = pm[h.bone];
        }
        float v[4] = { h.x, h.y, h.z, 1 }, w[4];
        mul_row(v, M, w);
        float sx = sqrtf(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]), sy = sqrtf(M[4] * M[4] + M[5] * M[5] + M[6] * M[6]);
        float sz = sqrtf(M[8] * M[8] + M[9] * M[9] + M[10] * M[10]);
        float scale = std::min(sx, std::min(sy, sz));
        if (std::max(sx, std::max(sy, sz)) < 0.01f) continue;
        if (scale < 0.05f) scale = std::max(sx, std::max(sy, sz));
        out[k][0] = w[0]; out[k][1] = w[1]; out[k][2] = w[2]; out[k][3] = h.r * scale; k++;
        if (h.part != 0xffff && un.part_logged != un.cur_serial && g_hitlog < 80) {
            un.part_logged = un.cur_serial; g_hitlog++;
            fd_log("framedata:   weapon sphere drawn: r %.1f (scales %.2f %.2f %.2f), %.1f from the unit", out[k - 1][3], sx, sy, sz,
                   sqrtf((w[0] - un.pos[0]) * (w[0] - un.pos[0]) + (w[2] - un.pos[2]) * (w[2] - un.pos[2])));
        }
    }
    return k;
}


// sphere -> screen circle: ndc centre and radius in ndc-y units
bool fd_project_sphere(const float *c, float r, float *x, float *y, float *r_ndc) {
    if (!g_cam_ok) return false;
    float p[4] = { c[0], c[1], c[2], 1 }, v[4], cl[4];
    mul_row(p, g_view, v); mul_row(v, g_proj, cl);
    if (cl[3] <= 0.5f) return false;
    *x = cl[0] / cl[3]; *y = cl[1] / cl[3]; *r_ndc = r * g_proj[5] / cl[3];
    return true;
}

static bool bones_here(Unit &un, const float *root);
int fd_bones(int i, float (*out)[3], int max) {   // world positions of the unit's bones (array found by scan_bones)
    if (i < 0 || i >= (int)g_units.size()) return 0;
    Unit &un = g_units[i];
    static float m[128][16];
    int n = std::min(std::min(max, 128), un.nbones);
    if (!un.bones || n <= 0) { InterlockedExchange(&g_bones_bad, 1); return 0; }
    if (!rd(un.bones, m, n * 64) || !bones_here(un, m[0])) return 0;   // the array moved or was freed: search again
    int k = 0;
    for (int b = 0; b < n; b++) { out[k][0] = m[b][12]; out[k][1] = m[b][13]; out[k][2] = m[b][14]; k++; }
    return k;
}
