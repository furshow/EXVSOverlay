// Custom Training: custom VS Training matches (2v2 / 1v2 / 1v3, CPU behavior and EX per slot), set up in the EXVS Launcher.
//
// The match itself is run by training_bridge.dll from aerial's "2v2 Training Extension" (non-commercial
// license, QQ group 1132405231; shipped unmodified by the launcher in <game>\Training\). Their package
// loads it with a .NET web API that injects it into the game and only accepts two exact exe hashes; here
// the overlay is already in the game, so it does that part itself:
//   1. checks the build: vsac29_Release.exe 1.13.30 (PE TimeDateStamp 0x69E2081B), any mods are fine
//   2. applies the 2 code patches of their isohook_2v2.dll (member count 2 -> 3+1, 2v2 gate on); the rest of
//      that DLL is splitscreen window / input handling we don't want, so it isn't loaded
//   3. creates the shared config block Local\EXVS2IB_TRAINING_V8_<pid> and loads the bridge
//   4. writes the launcher's settings (Training\training.ini) into the block whenever they change
// State for the launcher goes to Training\status.ini.
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"

static const DWORD EXE_STAMP = 0x69e2081b;                    // 1.13.30
static const DWORD EXE_IMAGES[] = { 0x311f000, 0x310f000 };   // with / without the .fpfix section
static const DWORD RVA_MEMBERS = 0x79e5aa;                    // lea r9d,[r10+2]; setne al
static const BYTE SIG_MEMBERS[] = { 0x45, 0x8d, 0x4a, 0x02, 0x0f, 0x95, 0xc0 };
static const DWORD RVA_GATE = 0x79eb8b;                       // jmp +2; xor al,al; mov [rsi+13D5],al
static const BYTE SIG_GATE[] = { 0xeb, 0x02, 0x32, 0xc0, 0x88, 0x86, 0xd5, 0x13, 0x00, 0x00 };
static const char BRIDGE_SHA256[] = "17F868D9750A404475C865E96254B453B3C31CB65A8CBFB0901B14535630292C";

// shared block (bridge protocol 8)
enum : DWORD {
    B_MAGIC = 827479113u, B_PROTOCOL = 8, B_SIZE = 13120, B_POOL_MAX = 512,
    O_MAGIC = 0, O_PROTOCOL = 4, O_SIZE = 8, O_PID = 12, O_SEQ = 16, O_ENABLED = 20, O_MEMBERS = 24,
    O_READ_SEQ = 92, O_HOOK_READY = 96, O_ERROR = 100, O_APPLIED = 108, O_MODE = 264, O_MODE_APPLIED = 268,
    O_POOL_COUNT = 272, O_POOL = 280, O_RANDOM_STAGE = 2328, O_SAVE_MASTERY = 2332, O_CONSUME_COST = 2364,
};

struct Config {
    bool enabled, random_stage, consume_cost;
    // 1 = 2v2 (bridge), 0 = 1v2 and 2 = 1v3 (without team cost: the game's own training, see "solo" below),
    // 3 = any other layout (1v1, 2v1, 3v1: the bridge's 2v2 rearranged, see "layouts" below)
    int partner;
    int layout[4];          // per bridge position (0 = you): 0 your team, 1 enemy team, -1 taken out
    int allies, enemies;    // CPUs on your team / on the enemy team
    int seq, unit[4], level[4], ex[4];   // per bridge position (read_config puts the launcher's slots there)
    int pool[B_POOL_MAX], npool;
    int slot_pool[4][256], nslot[4];   // random by category (unit <= -2): the overlay draws one per match
};

static char g_tdir[MAX_PATH];   // <game>\Training\ (trailing backslash)
static volatile BYTE *g_block;
static bool g_loaded;
static char g_state[24] = "off", g_msg[256];

static void set_status(const char *state, const char *fmt, ...) {
    char m[256];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    if (!strcmp(state, g_state) && !strcmp(m, g_msg)) return;
    snprintf(g_state, sizeof(g_state), "%s", state); snprintf(g_msg, sizeof(g_msg), "%s", m);
    ov_log("training: %s - %s", state, m);
    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%sstatus.ini", g_tdir);
    char pid[16]; snprintf(pid, sizeof(pid), "%lu", GetCurrentProcessId());
    WritePrivateProfileStringA("Status", "Pid", pid, ini);
    WritePrivateProfileStringA("Status", "State", state, ini);
    WritePrivateProfileStringA("Status", "Message", m, ini);
}

static DWORD rd(DWORD off) { return *(volatile DWORD *)(g_block + off); }
static void wr(DWORD off, DWORD v) { *(volatile DWORD *)(g_block + off) = v; }

// ---------------------------------------------------------------------------------------------
static bool build_ok(BYTE *base) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != EXE_STAMP) return false;
    for (DWORD s : EXE_IMAGES) if (nt->OptionalHeader.SizeOfImage == s) return true;
    return false;
}

static bool patch_code(BYTE *at, const BYTE *sig, size_t n, size_t off, const BYTE *to, size_t tn, const char *what) {
    if (!memcmp(at + off, to, tn)) {   // already on (bridge reloaded / game restarted from the overlay)
        BYTE tmp[64]; if (n > sizeof(tmp)) return false;
        memcpy(tmp, sig, n); memcpy(tmp + off, to, tn);
        if (!memcmp(at, tmp, n)) return true;
    }
    if (memcmp(at, sig, n)) { set_status("error", "The %s code isn't the expected one (another mod changed it?)", what); return false; }
    DWORD old;
    if (!VirtualProtect(at + off, tn, PAGE_EXECUTE_READWRITE, &old)) { set_status("error", "VirtualProtect failed (%lu)", GetLastError()); return false; }
    memcpy(at + off, to, tn);
    VirtualProtect(at + off, tn, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at + off, tn);
    return true;
}

static bool sha256_file(const wchar_t *path, char out[65]) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        static BYTE buf[65536]; DWORD got;
        while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got) BCryptHashData(h, buf, got, 0);
        BYTE d[32];
        if (BCryptFinishHash(h, d, 32, 0) == 0) { for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02X", d[i]); ok = true; }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

// Bridge state: seqlock at O_READ_SEQ (odd = the game is writing)
static bool read_state(DWORD *ready, DWORD *err, DWORD *applied, DWORD *mode, DWORD *mode_applied) {
    for (int i = 0; i < 20; i++) {
        DWORD s = rd(O_READ_SEQ);
        if (s & 1) { Sleep(1); continue; }
        MemoryBarrier();
        *ready = rd(O_HOOK_READY); *err = rd(O_ERROR); *applied = rd(O_APPLIED); *mode = rd(O_MODE); *mode_applied = rd(O_MODE_APPLIED);
        MemoryBarrier();
        if (s == rd(O_READ_SEQ)) return true;
    }
    return false;
}

static const char *bridge_error(DWORD e) {
    switch (e) {
    case 2: return "the bridge got an invalid setup";
    case 3: return "the game didn't build 4 players";
    case 4: return "the training slots don't match this game build";
    case 10: return "a mod changed game code the bridge checks (tell the developer which exe mods you use)";
    case 5: return "no single human player found in the training match";
    case 6: return "the setup read back differently";
    case 7: return "the training match couldn't be read";
    case 8: return "unexpected team value";
    case 11: case 12: case 13: return "the bridge hooks couldn't be installed";
    case 14: return "the 2v2 switch failed";
    case 15: return "the battle rule hooks couldn't be created (restart the game)";
    case 16: return "the random unit list was rejected";
    case 17: return "the entry scene hook failed";
    case 18: return "the random stage pick failed";
    default: { static char b[32]; snprintf(b, sizeof(b), "bridge error %lu", e); return b; }
    }
}

// Unit kind table in .rdata (8-byte entries: unit id, flags; 4 = boss). The bridge only starts when the 13
// bosses and Eldora Dotress it offers still have the boss bit there. The Mod Manager's "bosses as playable
// units" turns that bit off (the game treats flag & 6 as "boss" and leaves bosses out of the unit select), so
// the bit is put back in memory only while the bridge checks the game, and taken off again right after.
static const DWORD RVA_KIND_TABLE = 0x13b5160, KIND_ENTRIES = 0x3cc, KIND_BOSS = 4;
static const DWORD BRIDGE_BOSSES[] = { 601001001, 601007001, 603002001, 613001001, 615001001, 623001001, 633001001,
                                       642001001, 643001001, 651001001, 654001001, 654002001, 654003001, 662001001 };
struct KindFix { DWORD *flags; DWORD old; };
static KindFix g_kind_fix[16];
static int g_kind_n;

static void write_mem(void *at, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(at, src, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}
static void kind_flags_for_bridge(BYTE *base) {
    g_kind_n = 0;
    DWORD *t = (DWORD *)(base + RVA_KIND_TABLE);
    for (DWORD i = 0; i < KIND_ENTRIES; i++)
        for (DWORD id : BRIDGE_BOSSES)
            if (t[i * 2] == id && !(t[i * 2 + 1] & KIND_BOSS) && g_kind_n < 16) {
                g_kind_fix[g_kind_n++] = { &t[i * 2 + 1], t[i * 2 + 1] };
                DWORD v = t[i * 2 + 1] | KIND_BOSS;
                write_mem(&t[i * 2 + 1], &v, 4);
            }
    if (g_kind_n) ov_log("training: boss bit back on for %d playable bosses while the bridge checks the game", g_kind_n);
}
static void kind_flags_restore(void) {
    for (int i = 0; i < g_kind_n; i++) write_mem(g_kind_fix[i].flags, &g_kind_fix[i].old, 4);
    g_kind_n = 0;
}

// training-entry.ini [Entry] Enabled: the bridge's own match intro (VS screen with the unit lineup, then the match
// set up like an online one), read once when the bridge loads. Off for 1v3 with team cost (see the 1v3 section);
// switching between 2v2 and 1v3 with team cost then needs a game restart.
static int g_bridge_entry = -1;   // what the loaded bridge read (1 on, 0 off)
static void write_entry_ini(bool on) {
    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%straining-entry.ini", g_tdir);
    WritePrivateProfileStringA("Entry", "Enabled", on ? "1" : "0", ini);
    WritePrivateProfileStringA("Entry", "Lineup", "1", ini);
    WritePrivateProfileStringA("Entry", "NativeStart", "1", ini);
    WritePrivateProfileStringA("Entry", "Background", "1", ini);
}

static bool load_bridge(void) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    if (!build_ok(base)) { set_status("error", "Needs the 1.13.30 game (build 0x%08lX)", EXE_STAMP); return false; }
    wchar_t dll[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, g_tdir, -1, dll, MAX_PATH); wcscat(dll, L"training_bridge.dll");
    char sha[65];
    if (!sha256_file(dll, sha)) { set_status("error", "Training\\training_bridge.dll is missing (turn 2v2 off and on in the launcher)"); return false; }
    if (strcmp(sha, BRIDGE_SHA256)) { set_status("error", "Training\\training_bridge.dll isn't the expected file"); return false; }

    static const BYTE three = 3, gate_on[2] = { 0xb0, 0x01 };
    BYTE members_was = base[RVA_MEMBERS + 3], gate_was[2] = { base[RVA_GATE + 2], base[RVA_GATE + 3] };
    // anything below fails: the game goes back to the normal 1v1 (no half-applied 2v2 with default units)
    auto undo = [&] {
        kind_flags_restore();
        if (base[RVA_MEMBERS + 3] != members_was) write_mem(base + RVA_MEMBERS + 3, &members_was, 1);
        if (memcmp(base + RVA_GATE + 2, gate_was, 2)) write_mem(base + RVA_GATE + 2, gate_was, 2);
        return false;
    };
    if (!patch_code(base + RVA_MEMBERS, SIG_MEMBERS, sizeof(SIG_MEMBERS), 3, &three, 1, "player count")) return undo();
    if (!patch_code(base + RVA_GATE, SIG_GATE, sizeof(SIG_GATE), 2, gate_on, 2, "2v2 gate")) return undo();

    wchar_t name[64]; swprintf(name, 64, L"Local\\EXVS2IB_TRAINING_V8_%lu", GetCurrentProcessId());
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, B_SIZE, name);
    if (!m) { set_status("error", "CreateFileMapping failed (%lu)", GetLastError()); return undo(); }
    g_block = (volatile BYTE *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, B_SIZE);
    if (!g_block) { set_status("error", "MapViewOfFile failed (%lu)", GetLastError()); return undo(); }
    if (rd(O_MAGIC) == 0) {
        wr(O_PROTOCOL, B_PROTOCOL); wr(O_SIZE, B_SIZE); wr(O_PID, GetCurrentProcessId()); wr(O_MODE, 0xffffffffu);
        MemoryBarrier();
        wr(O_MAGIC, B_MAGIC);
    }
    if (rd(O_MAGIC) != B_MAGIC || rd(O_PROTOCOL) != B_PROTOCOL || rd(O_SIZE) != B_SIZE) { set_status("error", "Unexpected training block"); return undo(); }

    kind_flags_for_bridge(base);
    if (!LoadLibraryW(dll)) { set_status("error", "training_bridge.dll didn't load (%lu)", GetLastError()); return undo(); }
    for (int i = 0; i < 100; i++) {
        DWORD ready, err, a, md, ma;
        if (read_state(&ready, &err, &a, &md, &ma)) {
            if (err) { set_status("error", "Bridge: %s", bridge_error(err)); return undo(); }
            if (ready == 1) { kind_flags_restore(); g_loaded = true; ov_log("training: bridge loaded, hooks ready"); return true; }
        }
        Sleep(100);
    }
    set_status("error", "The bridge didn't get ready in 10 s");
    return undo();
}

// writer side of the config seqlock (same steps as the original TrainingApi)
static DWORD begin_write(void) {
    DWORD s = rd(O_SEQ);
    if (s & 1) s++;
    if (s >= 2147483643u) s = 0;
    wr(O_SEQ, s + 1);
    MemoryBarrier();
    return s;
}
static DWORD end_write(DWORD s) { MemoryBarrier(); wr(O_SEQ, s + 2); return s + 2; }

static const int EX_RANDOM = 3;
static int random_below(int n) {   // 0..n-1
    static bool seeded;
    if (!seeded) { srand(GetTickCount() ^ (GetCurrentProcessId() << 16)); seeded = true; }
    return n > 0 ? rand() % n : 0;
}
static int random_ex(void) { return random_below(3); }
// something the overlay draws itself (random EX, random unit by category): drawn again after every match
static bool any_random_ex(const Config &c) { for (int i = 0; i < 4; i++) if (c.ex[i] == EX_RANDOM || c.unit[i] < -1) return true; return false; }

static DWORD submit(const Config &c) {
    DWORD s = begin_write();
    wr(O_ENABLED, 1);
    wr(O_RANDOM_STAGE, c.random_stage); wr(O_SAVE_MASTERY, 0); wr(O_CONSUME_COST, c.consume_cost);
    bool any_random = false;
    for (int i = 0; i < 4; i++) any_random |= c.unit[i] == -1;
    int n = any_random ? c.npool : 0;
    wr(O_POOL_COUNT, n);
    for (int i = 0; i < n; i++) wr(O_POOL + i * 4, c.pool[i]);
    for (int i = 0; i < 4; i++) {
        DWORD o = O_MEMBERS + i * 16;
        wr(o, c.unit[i] < -1 ? (DWORD)c.slot_pool[i][random_below(c.nslot[i])] : (DWORD)c.unit[i]);
        wr(o + 4, i ? c.level[i] : 0);
        wr(o + 8, c.ex[i] == EX_RANDOM ? (DWORD)random_ex() : (DWORD)c.ex[i]);
        wr(o + 12, i >= 2 ? 1 : 0);
    }
    return end_write(s);
}
static void disable(void) {
    if (!rd(O_ENABLED)) return;
    DWORD s = begin_write(); wr(O_ENABLED, 0); end_write(s);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

// The launcher's slots (you + 3 CPUs, each on a team or out) in the bridge's 4 positions, whose teams it always
// makes 0, 0, 1, 1: you at 0, your CPUs at 1 then 2, the enemies at 2 then 3 then 1, the CPUs that are out in the
// positions left (the bridge still needs a unit there). So a 2v2 needs no change, 1v3 moves position 1 to the
// enemy team, 3v1 moves position 2 to yours, and 1v2 / 2v1 / 1v1 take positions out (see "layouts" below).
static void arrange(Config *c, const int team[4], const int on[4]) {
    int slot_at[4] = { 0, -1, -1, -1 }, a = 0, e = 0;
    static const int ALLY_POS[2] = { 1, 2 }, ENEMY_POS[3] = { 2, 3, 1 };
    for (int i = 1; i < 4; i++) if (on[i] && !team[i] && a < 2) slot_at[ALLY_POS[a++]] = i;
    for (int i = 1; i < 4; i++) {
        if (!on[i] || !team[i]) continue;
        for (int p : ENEMY_POS) if (slot_at[p] < 0) { slot_at[p] = i; e++; break; }
    }
    if (!e) {   // no enemy (shouldn't happen, the launcher keeps one): the plain 2v2
        for (int p = 0; p < 4; p++) slot_at[p] = p;
        a = 1; e = 2;
    }
    for (int p = 1, i = 1; p < 4; p++) {   // the CPUs that are out, in the positions left
        if (slot_at[p] >= 0) continue;
        while (i < 4 && [&] { for (int q = 0; q < 4; q++) if (slot_at[q] == i) return true; return false; }()) i++;
        slot_at[p] = i < 4 ? i++ : 1;
        c->layout[p] = -1;
    }
    for (int p = 1; p < 4; p++)
        if (c->layout[p] != -1) c->layout[p] = team[slot_at[p]];
    Config o = *c;
    for (int p = 0; p < 4; p++) {
        int i = slot_at[p];
        c->unit[p] = o.unit[i]; c->level[p] = o.level[i]; c->ex[p] = o.ex[i];
        c->nslot[p] = o.nslot[i]; memcpy(c->slot_pool[p], o.slot_pool[i], sizeof(c->slot_pool[p]));
    }
    c->allies = a; c->enemies = e;
    bool plain = a == 1 && e == 2;   // positions 1 / 2 / 3 = ally / enemy / enemy: the bridge's own 2v2
    c->partner = plain ? 1 : a == 0 && e == 2 ? 0 : a == 0 && e == 3 ? 2 : 3;
}

static bool read_config(const char *ini, Config *c) {
    memset(c, 0, sizeof(*c));
    c->enabled = GetPrivateProfileIntA("Training", "Enabled", 0, ini) != 0;
    c->seq = GetPrivateProfileIntA("Training", "Seq", 0, ini);
    c->random_stage = GetPrivateProfileIntA("Training", "RandomStage", 0, ini) != 0;
    c->consume_cost = GetPrivateProfileIntA("Training", "ConsumeCost", 1, ini) != 0;
    char k[16], v[32];
    for (int i = 0; i < 4; i++) {
        snprintf(k, sizeof(k), "Unit%d", i + 1); GetPrivateProfileStringA("Training", k, "-1", v, sizeof(v), ini); c->unit[i] = atoi(v);
        // CPU behavior (bridge "level"): 0 stop, 1 easy, 2 normal, 3 hard, 4 extreme, 5 walk, 6 evade, 7-9 low/mid/high jump
        snprintf(k, sizeof(k), "Cpu%d", i + 1); c->level[i] = clampi(GetPrivateProfileIntA("Training", k, i == 1 ? 2 : 3, ini), 0, 9);
        snprintf(k, sizeof(k), "Ex%d", i + 1); GetPrivateProfileStringA("Training", k, i ? "0" : "-1", v, sizeof(v), ini);
        c->ex[i] = clampi(atoi(v), i ? 0 : -1, 3);   // 3 = random F / S / E, drawn again for every match
        // bosses have 9-digit ids (601001001); only P1 may be "the unit picked in game" (0);
        // below -1 = random by category (cost / boss) from SlotPool<n>
        if (c->unit[i] > 999999999 || (i && c->unit[i] == 0)) c->unit[i] = -1;
        if (c->unit[i] < -1) {
            static char sp[4096];
            snprintf(k, sizeof(k), "SlotPool%d", i + 1); GetPrivateProfileStringA("Training", k, "", sp, sizeof(sp), ini);
            c->nslot[i] = 0;
            for (char *p = sp; *p && c->nslot[i] < 256;) {
                long id = strtol(p, &p, 10);
                if (id > 0 && id <= 999999999) c->slot_pool[i][c->nslot[i]++] = (int)id;
                while (*p && (*p < '0' || *p > '9')) p++;
            }
            if (!c->nslot[i]) c->unit[i] = -1;   // nothing to draw from: any unit
        }
    }
    static char pool[8192];
    GetPrivateProfileStringA("Training", "Pool", "", pool, sizeof(pool), ini);
    for (char *p = pool; *p && c->npool < (int)B_POOL_MAX;) {
        long id = strtol(p, &p, 10);
        if (id > 0 && id <= 99999999) c->pool[c->npool++] = (int)id;
        while (*p && (*p < '0' || *p > '9')) p++;
    }
    qsort(c->pool, c->npool, sizeof(int), cmp_int);   // the bridge wants it sorted, no duplicates
    int w = 0;
    for (int i = 0; i < c->npool; i++) if (!w || c->pool[w - 1] != c->pool[i]) c->pool[w++] = c->pool[i];
    c->npool = w;

    // teams: Team<n> (0 yours, 1 enemy) and On<n> for the CPU slots 2-4 (launcher 1.3.1+); older settings only
    // have Partner (1 ally, 0 off, 2 enemy) for slot 2, with slots 3 and 4 the enemies
    int team[4] = { 0, 0, 1, 1 }, on[4] = { 1, 1, 1, 1 };
    char probe[8];
    if (GetPrivateProfileStringA("Training", "Team2", "", probe, sizeof(probe), ini)) {
        for (int i = 1; i < 4; i++) {
            snprintf(k, sizeof(k), "Team%d", i + 1); team[i] = GetPrivateProfileIntA("Training", k, i == 1 ? 0 : 1, ini) ? 1 : 0;
            snprintf(k, sizeof(k), "On%d", i + 1); on[i] = GetPrivateProfileIntA("Training", k, 1, ini) ? 1 : 0;
        }
    } else {
        int pm = clampi(GetPrivateProfileIntA("Training", "Partner", 1, ini), 0, 2);
        on[1] = pm != 0; team[1] = pm == 2;
    }
    arrange(c, team, on);
    return true;
}

// ---- Start during a 2v2 match ------------------------------------------------------------------
// Start opens the training pause menu, which is made for 1v1 and freezes the game while the bridge runs
// a 2v2 match. While the bridge's battle rules are active (Local\EXVS2IB_RULES_V13_<pid>, "Active" at +24)
// the game doesn't see Start: the keyboard key, the XInput button and the DirectInput buttons of config.ini.
static volatile const BYTE *g_rules;
static volatile LONG g_rules_active;
static int g_start_vk = -1;
static WORD g_start_mask;
static DWORD g_start_hid;   // bit n = HID button n+1
static volatile LONG g_start_note_tick;
static volatile LONG g_match_over;   // a 2v2 match just ended (random EX: draw again for the next one)
static volatile LONG g_flip_slots;   // ... and for 1v3 with team cost: log what the rematch looks at

static void load_start_bindings(void) {
    char cfg[MAX_PATH], v[64];
    snprintf(cfg, sizeof(cfg), "%sconfig.ini", ov_cfg_dir());
    GetPrivateProfileStringA("keyboard", "Start", "", v, sizeof(v), cfg);
    int vk = v[0] ? menu_key_vk(v) : 0;
    g_start_mask = 0x0010;   // XInput Start
    UINT arc = GetPrivateProfileIntA("controller", "ArcadeStartButton", 0, cfg);
    if (arc && arc < 0x10000) g_start_mask |= (WORD)arc;
    g_start_hid = 0;
    static const char *const PAD_SECS[] = { "controller", "controller-1" };
    for (const char *sec : PAD_SECS) {   // DirectInput pads: Start = "10" or "10,5"
        GetPrivateProfileStringA(sec, "Start", "", v, sizeof(v), cfg);
        for (char *q = v; *q;) {
            long b = strtol(q, &q, 10);
            if (b >= 1 && b <= 32) g_start_hid |= 1u << (b - 1);
            while (*q && (*q < '0' || *q > '9')) q++;
        }
    }
    g_start_vk = vk;
    ov_log("training: Start is hidden from the game during 2v2 matches (key 0x%02X, XInput mask 0x%04X, HID buttons 0x%lX)", vk, g_start_mask, g_start_hid);
}

static void watch_rules(void) {
    if (!g_rules) {
        wchar_t name[64]; swprintf(name, 64, L"Local\\EXVS2IB_RULES_V13_%lu", GetCurrentProcessId());
        HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!m) return;
        const BYTE *v = (const BYTE *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        if (!v) { CloseHandle(m); return; }
        if (*(const DWORD *)v != 810700361u) { UnmapViewOfFile(v); CloseHandle(m); return; }
        g_rules = v;
    }
    auto f = [](DWORD o) { return *(volatile const DWORD *)(g_rules + o); };
    LONG active = f(20) == 1 && f(24) == 1;
    if (InterlockedExchange(&g_rules_active, active) != active) {
        ov_log("training: 2v2 match %s", active ? "started (Start hidden from the game)" : "over (Start works again)");
        if (!active) { InterlockedExchange(&g_match_over, 1); InterlockedExchange(&g_flip_slots, 1); }
    }
    // the bridge's finish / rematch state (RuleState FinishPhase, FinishErrors, RestartPending, RestartPhase,
    // Restarts, RestartErrors), logged when it changes: says why a rematch did or didn't start
    static DWORD last[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
    static const DWORD OFF[6] = { 136, 132, 172, 188, 180, 184 };
    DWORD now[6]; bool changed = false;
    for (int i = 0; i < 6; i++) { now[i] = f(OFF[i]); changed |= now[i] != last[i]; }
    if (changed && last[0] != ~0u) {
        ov_log("training: finish phase %lu (errors %lu), rematch pending %lu phase %lu, rematches %lu (errors %lu)",
               now[0], now[1], now[2], now[3], now[4], now[5]);
        // the rest of the bridge's RuleState (block offsets 112-276), for the 1v3 / 1v2 finish research:
        // FinishStarted 116, PayloadReady 192, ScoreRows 196, ScoreErrors 200, LocalTeam 204, EnumeratedActors 232,
        // EligibleActors 236, PayloadStage 240 (0x110 = ok), EngineMatched 244, ResultDeferred 268, ResultLaunches 272
        char buf[512]; int k = 0;
        for (DWORD o = 112; o <= 276 && k < (int)sizeof(buf) - 16; o += 4) k += snprintf(buf + k, sizeof(buf) - k, "%lX ", f(o));
        ov_log("training:   rules 112-276: %s", buf);
    }
    memcpy(last, now, sizeof(last));
}

bool training_block_start(void);
int training_start_vk(void) { return g_start_vk; }
WORD training_start_pad_mask(void) { return g_start_mask; }
bool training_start_hid_button(USHORT b) { return b >= 1 && b <= 32 && (g_start_hid >> (b - 1)) & 1; }
void training_note_start_blocked(const char *from) {
    LONG now = (LONG)GetTickCount(), last = g_start_note_tick;
    if (now - last < 3000) return;
    g_start_note_tick = now;
    ov_log("training: Start (%s) ignored during the 2v2 match", from);
}

// ---- 1v2 / 1v3 ("solo": partner off or on the enemy team) -----------------------------------------------
// The bridge only runs 4 units. With just the player-count and gate patches (no bridge, no 6-byte NOP the
// bridge adds for the 4th unit) the game's training builder (RVA 0x79E4F0) makes 3 units: you and two CPUs.
// For 1v3 the bridge's third patch (the 6-byte jz at RVA 0x79E5BE becomes NOPs) is added too, so the game
// builds 4 units; the partner slot then goes to the enemy team.
// A detour on that function lets the game build them and then writes the launcher's setup into the slots,
// the way the bridge does for 4. Training object: unit count at +0x1098, slots of 0x2E0 bytes from +0x570.
// Slot fields (from the slot start): +0 human (byte), +0x158 slot index, +0x15C 0, +0x160 team, +0x164 unit,
// +0x168 EX, +0x16C/+0x170/+0x174 CPU behavior (CPU_BEHAVIOR[level] = values for +0x174, +0x16C, +0x170).
static const DWORD RVA_BUILDER = 0x79e4f0;
static const BYTE BUILDER_PROLOGUE[9] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00 };   // push rbx; sub rsp,0C0h
static const DWORD O_UNIT_COUNT = 0x1098, SLOT0 = 0x570, SLOT_SIZE = 0x2e0;
static const DWORD RVA_FOURTH = 0x79e5be;   // jz that keeps the 4th unit out (the bridge NOPs it for 2v2)
static const BYTE SIG_FOURTH[6] = { 0x0f, 0x84, 0x0d, 0x01, 0x00, 0x00 }, NOP6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
static bool g_fourth_patched;
static const DWORD CPU_BEHAVIOR[10][3] = { {1,0,0}, {0,2,1}, {0,5,4}, {0,7,6}, {0,9,9}, {3,0,0}, {4,0,9}, {8,0,0}, {6,0,0}, {7,0,0} };

typedef void *(*BuilderFn)(void *, void *, void *, void *);
static BuilderFn g_builder_tramp;
static BYTE *g_builder_page;
static bool g_builder_hooked, g_1v2_patched;
static BYTE g_members_was, g_gate_was[2];
static CRITICAL_SECTION g_1v2_cs;
static Config g_1v2_cfg;                 // the setup, copied for the game thread
static volatile LONG g_1v2_on, g_1v2_built;
static volatile LONG64 g_1v2_obj;        // training object of the running 1v2 (Start is hidden while it lives)
static ULONG64 g_1v2_vtable;

// Random stage (what the bridge does for 2v2): the game's stage list is index 1..254 -> stage id (RVA 0x902360),
// and RVA 0x902300 says whether that stage can be played; the training object takes the stage at +0x11E4.
static const DWORD RVA_STAGE_AT = 0x902360, RVA_STAGE_OK = 0x902300, O_STAGE = 0x11e4;   // index -> id, id -> index
static const BYTE SIG_STAGE_AT[10] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20 };
static const BYTE SIG_STAGE_OK[8] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8b, 0xd9 };
typedef int (*StageFn)(int);

static int random_stage_id(void) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    if (memcmp(base + RVA_STAGE_AT, SIG_STAGE_AT, sizeof(SIG_STAGE_AT)) || memcmp(base + RVA_STAGE_OK, SIG_STAGE_OK, sizeof(SIG_STAGE_OK))) {
        ov_log("training: random stage: unexpected game code, stage left as it is");
        return 0;
    }
    StageFn stage_at = (StageFn)(base + RVA_STAGE_AT), stage_ok = (StageFn)(base + RVA_STAGE_OK);
    static int ids[256];
    int n = 0;
    for (int i = 1; i < 255; i++) {
        int id = stage_at(i);
        if (!id || id == (int)0xfe67f4f9 || stage_ok(id) != i) continue;   // index -> id -> same index: a playable stage
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) dup = ids[k] == id;
        if (!dup) ids[n++] = id;
    }
    if (!n) { ov_log("training: random stage: no playable stage found"); return 0; }
    return ids[random_below(n)];
}

static int pick_unit(const Config &c, int i) {
    if (c.unit[i] < -1) return c.nslot[i] ? c.slot_pool[i][random_below(c.nslot[i])] : 0;   // random by category
    if (c.unit[i] == -1) return c.npool ? c.pool[random_below(c.npool)] : 0;                 // random, any unit
    return c.unit[i];
}

static void fill_1v2(BYTE *obj) {
    static Config c;
    EnterCriticalSection(&g_1v2_cs); c = g_1v2_cfg; LeaveCriticalSection(&g_1v2_cs);
    DWORD n = *(DWORD *)(obj + O_UNIT_COUNT), want = c.partner == 2 ? 4 : 3;
    if (n != want) { ov_log("training: %s: the game built %lu units, setup not written", want == 4 ? "1v3" : "1v2", n); return; }
    int human = -1, humans = 0;
    for (int i = 0; i < (int)n; i++) {
        BYTE *sl = obj + SLOT0 + i * SLOT_SIZE;
        if (*(DWORD *)(sl + 0x158) != (DWORD)i || *(DWORD *)(sl + 0x15c) != 0) { ov_log("training: 1v2: unexpected slot layout, setup not written"); return; }
        if (sl[0]) { humans++; human = i; }
    }
    if (humans != 1) { ov_log("training: 1v2: %d human players found, setup not written", humans); return; }
    BYTE *ps = obj + SLOT0 + human * SLOT_SIZE;
    DWORD pt = *(DWORD *)(ps + 0x160);
    if (pt > 1) { ov_log("training: 1v2: player team %lu, setup not written", pt); return; }
    if (c.unit[0]) { int u = pick_unit(c, 0); if (u > 0) *(DWORD *)(ps + 0x164) = (DWORD)u; }
    if (c.ex[0] != -1) *(DWORD *)(ps + 0x168) = (DWORD)(c.ex[0] == EX_RANDOM ? random_ex() : c.ex[0]);
    DWORD units[4] = { *(DWORD *)(ps + 0x164), 0, 0, 0 };
    static const int ORDER[3] = { 2, 3, 1 };   // launcher slots: Enemy 1, Enemy 2, then the partner card (1v3)
    int e = 0;
    for (int i = 0; i < (int)n; i++) {
        if (i == human) continue;
        int k = ORDER[e];
        BYTE *sl = obj + SLOT0 + i * SLOT_SIZE;
        *(DWORD *)(sl + 0x160) = 1 ^ pt;
        int u = pick_unit(c, k); if (u > 0) *(DWORD *)(sl + 0x164) = (DWORD)u;
        *(DWORD *)(sl + 0x168) = (DWORD)(c.ex[k] == EX_RANDOM ? random_ex() : c.ex[k]);
        const DWORD *b = CPU_BEHAVIOR[clampi(c.level[k], 0, 9)];
        *(DWORD *)(sl + 0x16c) = b[1]; *(DWORD *)(sl + 0x170) = b[2]; *(DWORD *)(sl + 0x174) = b[0];
        units[++e] = *(DWORD *)(sl + 0x164);
    }
    if (c.random_stage) {   // a different stage every match (else the stage the game chose: the training room)
        int st = random_stage_id();
        if (st) { *(DWORD *)(obj + O_STAGE) = (DWORD)st; ov_log("training: random stage 0x%08X", (unsigned)st); }
    }
    g_1v2_vtable = *(ULONG64 *)obj;
    InterlockedExchange64(&g_1v2_obj, (LONG64)(ULONG_PTR)obj);
    InterlockedIncrement(&g_1v2_built);
    if (n == 4) ov_log("training: 1v3 match built: you (%lu) vs %lu + %lu + %lu", units[0], units[1], units[2], units[3]);
    else ov_log("training: 1v2 match built: you (%lu) vs %lu + %lu", units[0], units[1], units[2]);
}

extern "C" void *tr_builder_detour(void *obj, void *a, void *b, void *c) {
    void *r = g_builder_tramp(obj, a, b, c);   // the game builds the training units
    if (g_1v2_on && obj) fill_1v2((BYTE *)obj);
    return r;
}

static void write_abs_jmp(BYTE *at, const void *to) {   // jmp qword ptr [rip+0]; dq to
    at[0] = 0xFF; at[1] = 0x25; memset(at + 2, 0, 4); memcpy(at + 6, &to, 8);
}
static BYTE *alloc_near_exe(BYTE *base) {   // within +-2 GB of the exe for the 5-byte jmp
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    SYSTEM_INFO si; GetSystemInfo(&si);
    ULONG_PTR gran = si.dwAllocationGranularity;
    ULONG_PTR start = ((ULONG_PTR)base + nt->OptionalHeader.SizeOfImage + gran) & ~(gran - 1);
    for (ULONG_PTR a = start; a < (ULONG_PTR)base + 0x70000000; a += gran) {
        void *pg = VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (pg) return (BYTE *)pg;
    }
    return NULL;
}
static bool builder_hook(BYTE *base) {
    if (g_builder_hooked) return true;
    BYTE *fn = base + RVA_BUILDER;
    if (memcmp(fn, BUILDER_PROLOGUE, sizeof(BUILDER_PROLOGUE))) {
        set_status("error", "2v2 already ran in this game session: restart the game to play 1v2");
        return false;
    }
    if (!g_builder_page) {
        BYTE *pg = alloc_near_exe(base);
        if (!pg) { set_status("error", "No memory near the game for the 1v2 hook"); return false; }
        write_abs_jmp(pg, (const void *)tr_builder_detour);
        memcpy(pg + 16, fn, sizeof(BUILDER_PROLOGUE));   // whole, position-independent instructions
        write_abs_jmp(pg + 16 + sizeof(BUILDER_PROLOGUE), fn + sizeof(BUILDER_PROLOGUE));
        FlushInstructionCache(GetCurrentProcess(), pg, 4096);
        g_builder_tramp = (BuilderFn)(pg + 16);
        g_builder_page = pg;
    }
    BYTE patch[sizeof(BUILDER_PROLOGUE)]; memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    int32_t rel = (int32_t)(g_builder_page - (fn + 5));
    memcpy(patch + 1, &rel, 4);
    write_mem(fn, patch, sizeof(patch));
    g_builder_hooked = true;
    return true;
}

static void stop_1v2(void) {
    InterlockedExchange(&g_1v2_on, 0);
    InterlockedExchange64(&g_1v2_obj, 0);
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    if (g_builder_hooked) { write_mem(base + RVA_BUILDER, BUILDER_PROLOGUE, sizeof(BUILDER_PROLOGUE)); g_builder_hooked = false; }
    if (g_1v2_patched) {   // back to the normal 1v1
        write_mem(base + RVA_MEMBERS + 3, &g_members_was, 1);
        write_mem(base + RVA_GATE + 2, g_gate_was, 2);
        g_1v2_patched = false;
        ov_log("training: 1v2 / 1v3 off, normal 1v1 training");
    }
    if (g_fourth_patched) { write_mem(base + RVA_FOURTH, SIG_FOURTH, 6); g_fourth_patched = false; }
}

static bool start_1v2(int units) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    if (!build_ok(base)) { set_status("error", "Needs the 1.13.30 game (build 0x%08lX)", EXE_STAMP); return false; }
    if (!g_1v2_patched) {
        static const BYTE three = 3, gate_on[2] = { 0xb0, 0x01 };
        g_members_was = base[RVA_MEMBERS + 3]; memcpy(g_gate_was, base + RVA_GATE + 2, 2);
        if (!patch_code(base + RVA_MEMBERS, SIG_MEMBERS, sizeof(SIG_MEMBERS), 3, &three, 1, "player count")) return false;
        g_1v2_patched = true;
        if (!patch_code(base + RVA_GATE, SIG_GATE, sizeof(SIG_GATE), 2, gate_on, 2, "2v2 gate")) { stop_1v2(); return false; }
    }
    if (units == 4 && !g_fourth_patched) {
        if (!patch_code(base + RVA_FOURTH, SIG_FOURTH, 6, 0, NOP6, 6, "4th unit")) { stop_1v2(); return false; }
        g_fourth_patched = true;
    } else if (units == 3 && g_fourth_patched) {
        write_mem(base + RVA_FOURTH, SIG_FOURTH, 6); g_fourth_patched = false;
    }
    if (!builder_hook(base)) { stop_1v2(); return false; }
    InterlockedExchange(&g_1v2_on, 1);
    ov_log("training: %s ready: the game builds you + %d CPUs, the overlay fills in the setup", units == 4 ? "1v3" : "1v2", units - 1);
    return true;
}

bool training_block_start(void) { return g_rules_active != 0 || (g_1v2_on && g_1v2_obj); }

static void set_1v2_config(const Config &c) { EnterCriticalSection(&g_1v2_cs); g_1v2_cfg = c; LeaveCriticalSection(&g_1v2_cs); }

// the 1v2 match is over when its training object is gone (freed or reused for something else)
static void watch_1v2(void) {
    LONG64 obj = g_1v2_obj;
    if (!obj) return;
    ULONG64 vt = 0; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)obj, &vt, 8, &got) || got != 8 || vt != g_1v2_vtable) {
        InterlockedCompareExchange64(&g_1v2_obj, 0, obj);
        ov_log("training: 1v2 training left (Start works again)");
    }
}

// ---- 1v3 / 1v2 with team cost: the bridge's 2v2, then the partner moved to the enemy team / taken out ------
// Team cost, COST OVER, the finish and the result screen are the bridge's own battle-rule hooks, and they only run
// after its 4-unit setup. So for 1v3 with team cost the bridge runs a normal 2v2 (partner on team 0) and a second
// detour, chained in front of the bridge's (MinHook left a jmp rel32 at the builder), moves the partner slot to the
// enemy team right after the bridge has filled the slots, with the bridge's intro (training-entry.ini) off:
// its VS screen (game VS scene, RVA 0x85EE00) lines up 2 per side and the game crashed there with 1 vs 3.
// Without the intro the bridge's finish fails (phase 4, 1 error) and there's no rematch. Keeping the intro and
// moving the partner after the VS screen doesn't work: tried 2026-11-21 at the bridge's lineup check (stage 5) in
// the session players, the match's player nodes (std::map keyed by member index, +0x40 team) and the training
// slots, all three moved and the match was still 2v2 (that flow makes the teams from the member order).
// 1v2 with team cost works the same way, but the partner slot is taken out instead (unit count 4 -> 3, the enemy
// slots moved down): a copy of only the bridge's dispatcher rules (2026-11-21) left the training's scaled unit
// costs and ended on a black screen, the bridge's per-unit cost fix and finish hooks are needed.
static BuilderFn g_chain_next;
static BYTE *g_chain_page;
static bool g_chain_hooked;
static volatile LONG g_flip_partner, g_flips;   // g_flip_partner: a layout other than the bridge's 2v2 is on
static volatile LONG64 g_flip_obj;   // training object of the running 1v3

// ---- layouts: the bridge's 2v2 rearranged -------------------------------------------------------------------
// g_layout = the setup's layout per bridge position (0 you, 1-3 the bridge's members in order: it fills the
// player's slot, then the other slots in order). After the bridge filled the slots (and again before every
// rematch, which puts them back to 0, 0, 1, 1), each position goes to its team or out: the slots that stay move
// down and the unit count drops to what's left.
static volatile LONG g_layout[4], g_layout_out;   // g_layout_out: positions taken out (1v2, 2v1: 1; 1v1: 2)
static bool apply_layout(BYTE *obj) {
    if (*(DWORD *)(obj + O_UNIT_COUNT) != 4) return false;   // already applied (or not the bridge's 4)
    int human = -1;
    for (int i = 0; i < 4; i++) if (obj[SLOT0 + i * SLOT_SIZE]) { if (human >= 0) return false; human = i; }
    if (human < 0) return false;
    DWORD pt = *(DWORD *)(obj + SLOT0 + human * SLOT_SIZE + 0x160);
    if (pt > 1) return false;
    bool out[4] = {}, changed = false;
    for (int i = 0, p = 1; i < 4; i++) {
        if (i == human) continue;
        int want = g_layout[p++];
        DWORD *team = (DWORD *)(obj + SLOT0 + i * SLOT_SIZE + 0x160);
        if (want < 0) { out[i] = true; changed = true; continue; }
        DWORD t = want ? 1 ^ pt : pt;
        if (*team != t) { *team = t; changed = true; }
    }
    if (!changed) return false;
    int n = 0;
    for (int i = 0; i < 4; i++) {   // the slots that stay move down, each with its slot index field
        if (out[i]) continue;
        BYTE *dst = obj + SLOT0 + n * SLOT_SIZE;
        if (n != i) memcpy(dst, obj + SLOT0 + i * SLOT_SIZE, SLOT_SIZE);
        *(DWORD *)(dst + 0x158) = (DWORD)n;
        n++;
    }
    *(DWORD *)(obj + O_UNIT_COUNT) = (DWORD)n;
    InterlockedIncrement(&g_flips);
    int mine = 0;
    for (int i = 0; i < n; i++) mine += *(DWORD *)(obj + SLOT0 + i * SLOT_SIZE + 0x160) == pt;
    ov_log("training: %dv%d: units %lu %lu %lu %lu, teams %lu %lu %lu %lu (count %d)", mine, n - mine,
               *(DWORD *)(obj + SLOT0 + 0x164), n > 1 ? *(DWORD *)(obj + SLOT0 + SLOT_SIZE + 0x164) : 0,
               n > 2 ? *(DWORD *)(obj + SLOT0 + 2 * SLOT_SIZE + 0x164) : 0, n > 3 ? *(DWORD *)(obj + SLOT0 + 3 * SLOT_SIZE + 0x164) : 0,
               *(DWORD *)(obj + SLOT0 + 0x160), n > 1 ? *(DWORD *)(obj + SLOT0 + SLOT_SIZE + 0x160) : 9,
               n > 2 ? *(DWORD *)(obj + SLOT0 + 2 * SLOT_SIZE + 0x160) : 9, n > 3 ? *(DWORD *)(obj + SLOT0 + 3 * SLOT_SIZE + 0x160) : 9, n);
    return true;
}

// When a match ends: what the bridge's rematch looks at (its exit hook, bridge fn 0x42E0, only rematches when the
// training object's state function at +0x468 is game RVA 0x7A0990 or 0x7A2FC0 and its flags +0x12FE/+0x12FF are 0)
static const DWORD RVA_OWNER_VTABLE = 0x15b0c10;   // the training object's class (the bridge checks it too)
static void log_owner_after_match(void) {
    LONG64 obj = g_flip_obj;
    if (!obj) return;
    BYTE *base = (BYTE *)GetModuleHandleW(NULL), buf[0x1310]; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(ULONG_PTR)obj, buf, sizeof(buf), &got) || got != sizeof(buf) ||
        *(ULONG64 *)buf != (ULONG64)(base + RVA_OWNER_VTABLE)) { ov_log("training: 1v3: the training object is gone after the match"); return; }
    ULONG64 st = *(ULONG64 *)(buf + 0x468);
    ov_log("training: custom match over: state fn %s0x%llX, flags 12FC-1300 = %u %u %u %u %u", st >= (ULONG64)base ? "RVA " : "",
           st >= (ULONG64)base ? st - (ULONG64)base : st, buf[0x12fc], buf[0x12fd], buf[0x12fe], buf[0x12ff], buf[0x1300]);
}

extern "C" void *tr_chain_detour(void *obj, void *a, void *b, void *c) {
    void *r = g_chain_next(obj, a, b, c);   // the bridge's detour (it calls the game's builder, then fills the slots)
    if (obj) InterlockedExchange64(&g_flip_obj, (LONG64)obj);
    if (g_flip_partner && obj) apply_layout((BYTE *)obj);
    return r;
}

// ---- diagnostics: the bridge's finish in 1v3 / 1v2 with team cost ----------------------------------------
// There the bridge's finish ends in phase 4 with an error (back to the menu, no rematch). Its finish detour
// (bridge fn 0x2D10, on game fn 0x7DBCF0, rcx = the match owner) is watched: whenever the finish phase or error
// count changes, what it decides on is logged: the owner's children at +0x114C (the result scene it expects,
// class RVA 0x18A7720) and +0x1198, the training object's flags and state, and the bridge's globals 0x32510-0x32547
// (0x32514 / 0x32515 armed, 0x32528 owner, 0x32530 training object, 0x32538 result scene seen being made).
static const DWORD BR_FINISH = 0x2d10, BR_PHASE = 0x325f8, BR_FERR = 0x325f4, RVA_CHILD_INDEX = 0x11f690;
static const BYTE BR_FINISH_PROLOGUE[10] = { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x83, 0xec, 0x20 };   // mov [rsp+8],rcx; push rbx; sub rsp,20h
typedef void *(*FinishFn)(void *, void *, void *, void *);
typedef int (*ChildIndexFn)(void *, void *);
static FinishFn g_finish_tramp;
static BYTE *g_finish_page;
static bool g_finish_hooked;

static void log_child(BYTE *base, BYTE *owner, DWORD off, char *out, size_t n) {
    int idx = ((ChildIndexFn)(base + RVA_CHILD_INDEX))(owner, owner + off);
    BYTE *c = idx >= 0 && idx < 0x40 ? *(BYTE **)(owner + 0x38 + idx * 16) : NULL;
    ULONG64 vt = 0; SIZE_T got = 0;
    if (c) ReadProcessMemory(GetCurrentProcess(), c, &vt, 8, &got);
    snprintf(out, n, "+%lX handle %08lX/%08lX -> idx %d %p class %s0x%llX", off, *(DWORD *)(owner + off), *(DWORD *)(owner + off + 4), idx, c,
             vt >= (ULONG64)base ? "RVA " : "", vt >= (ULONG64)base ? vt - (ULONG64)base : vt);
}

static void log_finish(BYTE *br, BYTE *owner, DWORD ph0, DWORD er0) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    char a[160], b[160];
    log_child(base, owner, 0x114c, a, sizeof(a));
    log_child(base, owner, 0x1198, b, sizeof(b));
    ov_log("training: finish %lu/%lu -> %lu/%lu, owner %p (class RVA 0x%llX): %s; %s", ph0, er0, *(DWORD *)(br + BR_PHASE), *(DWORD *)(br + BR_FERR),
           owner, *(ULONG64 *)owner - (ULONG64)base, a, b);
    BYTE *tr = *(BYTE **)(br + 0x32530);
    if (tr) ov_log("training:   training %p flags 12FC-1300 = %u %u %u %u %u, state RVA 0x%llX", tr, tr[0x12fc], tr[0x12fd], tr[0x12fe], tr[0x12ff], tr[0x1300],
                   *(ULONG64 *)(tr + 0x468) - (ULONG64)base);
    const BYTE *g = br + 0x32510;
    ov_log("training:   bridge 32510: %02X %02X %02X %02X | %02X %02X %02X %02X | 32518 %p 32520 %p 32528 %p 32530 %p 32538 %p | 32540 %02X %02X %02X %02X %02X %02X %02X %02X",
           g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], *(void **)(g + 8), *(void **)(g + 0x10), *(void **)(g + 0x18), *(void **)(g + 0x20),
           *(void **)(g + 0x28), g[0x30], g[0x31], g[0x32], g[0x33], g[0x34], g[0x35], g[0x36], g[0x37]);
}

extern "C" void *tr_finish_detour(void *owner, void *a, void *b, void *c) {
    BYTE *br = (BYTE *)GetModuleHandleW(L"training_bridge.dll");
    DWORD ph0 = *(volatile DWORD *)(br + BR_PHASE), er0 = *(volatile DWORD *)(br + BR_FERR);
    void *r = g_finish_tramp(owner, a, b, c);
    if (g_flip_partner && owner && (*(volatile DWORD *)(br + BR_PHASE) != ph0 || *(volatile DWORD *)(br + BR_FERR) != er0))
        log_finish(br, (BYTE *)owner, ph0, er0);
    return r;
}

static bool finish_hook(void) {
    if (g_finish_hooked) return true;
    BYTE *br = (BYTE *)GetModuleHandleW(L"training_bridge.dll");
    if (!br) return false;
    BYTE *fn = br + BR_FINISH;
    if (memcmp(fn, BR_FINISH_PROLOGUE, sizeof(BR_FINISH_PROLOGUE))) { ov_log("training: the bridge's finish code isn't the expected one, not watched"); return false; }
    if (!g_finish_page) {
        BYTE *pg = alloc_near_exe(br);   // near the bridge for its 5-byte jmp
        if (!pg) return false;
        write_abs_jmp(pg, (const void *)tr_finish_detour);
        memcpy(pg + 16, fn, 5);                  // mov [rsp+8],rcx (position-independent)
        write_abs_jmp(pg + 21, fn + 5);
        FlushInstructionCache(GetCurrentProcess(), pg, 4096);
        g_finish_tramp = (FinishFn)(pg + 16);
        g_finish_page = pg;
    }
    BYTE patch[5] = { 0xE9 };
    int32_t rel = (int32_t)(g_finish_page - (fn + 5));
    memcpy(patch + 1, &rel, 4);
    write_mem(fn, patch, 5);
    g_finish_hooked = true;
    ov_log("training: watching the bridge's finish (1v3 / 1v2 rematch research)");
    return true;
}

// ---- 1v3 result screen: the bridge's score rows need 2 players per team ------------------------------------
// Its result payload (bridge fn 0x7560, called from its hook on game fn 0x7DBAB0) counts the eligible players of
// each team and gives up unless they're 2 and 2 (ScoreErrors 3, seen 2026-11-21 in 1v3: no result screen, the
// finish goes to phase 4 and back to the menu, no rematch). For 1v3 those two checks are turned off in the
// loaded bridge (memory only; the file stays unmodified): cmp [rsp+20h],2 / jne / cmp [rsp+24h],2 / jne at 0x78E6.
// The "exactly one local player" check before them stays.
static const DWORD BR_TEAM_CHECK = 0x78e6;
static const BYTE SIG_TEAM_CHECK[22] = { 0x83, 0x7c, 0x24, 0x20, 0x02, 0x0f, 0x85, 0x0c, 0x04, 0x00, 0x00,
                                         0x83, 0x7c, 0x24, 0x24, 0x02, 0x0f, 0x85, 0x01, 0x04, 0x00, 0x00 };
static bool g_team_check_off;
static void team_check(bool off) {
    if (off == g_team_check_off) return;
    BYTE *br = (BYTE *)GetModuleHandleW(L"training_bridge.dll");
    if (!br) return;
    static BYTE nops[sizeof(SIG_TEAM_CHECK)];
    memset(nops, 0x90, sizeof(nops));
    if (off) {
        if (!patch_code(br + BR_TEAM_CHECK, SIG_TEAM_CHECK, sizeof(SIG_TEAM_CHECK), 0, nops, sizeof(nops), "bridge result teams")) return;
        ov_log("training: 1v3: the bridge's result screen accepts 1 vs 3");
    } else write_mem(br + BR_TEAM_CHECK, SIG_TEAM_CHECK, sizeof(SIG_TEAM_CHECK));
    g_team_check_off = off;
}

// ---- 1v2 result screen: a 4th row for the partner that was taken out -----------------------------------------
// The bridge's result payload (fn 0x7560) gathers the eligible players into 4 rows of 0xA0 bytes at rbp+0x2F0
// ({+0 member key (qword), +0xC unit, +0x10 team, ...}, the session's player info) and gives up unless there are 4
// (ScoreErrors 8 in 1v2). The game's result builder (RVA 0x883250) is made for 2 teams of 2, so in 1v2 a 4th row is
// made up: a copy of an enemy's row with the partner's member key and unit, on your team (the result shows a 2v2
// with the partner on 0). The bridge handles a key the match doesn't know (its place lookup, game fn 0x7C63B0,
// gives -1 -> place 0). A stub on its "4 rows?" check (bridge 0x77F4: cmp dword [r12+28h],4 / je 0x7836) calls
// tr_payload_fix(rbp, r12) and checks again: 4 rows -> 0x7836, else the original error path at 0x77FC.
static const DWORD BR_ROWS_CHECK = 0x77f4, BR_ROWS_OK = 0x7836, BR_ROWS_BAD = 0x77fc;
static const BYTE SIG_ROWS_CHECK[8] = { 0x41, 0x83, 0x7c, 0x24, 0x28, 0x04, 0x74, 0x3a };
static BYTE *g_rows_page;
static bool g_rows_hooked;
static volatile LONG g_rows_fixed;

extern "C" void tr_payload_fix(BYTE *frame, BYTE *out) {
    if (!g_flip_partner || !g_layout_out) return;
    DWORD *count = (DWORD *)(out + 0x28);
    if (*count < 2 || *count >= 4) return;
    BYTE *rows = frame + 0x2f0;
    ULONG64 local = *(ULONG64 *)(frame + 0xc0);
    int me = -1, cpu = -1, n = (int)*count;
    for (int i = 0; i < n; i++) if (*(ULONG64 *)(rows + i * 0xa0) == local) me = i; else if (cpu < 0) cpu = i;
    if (me < 0 || cpu < 0) { ov_log("training: result rows: your row / a CPU row wasn't found"); return; }
    DWORD pt = *(DWORD *)(rows + me * 0xa0 + 0x10);
    if (pt > 1) return;
    BYTE *br = (BYTE *)GetModuleHandleW(L"training_bridge.dll");
    while (n < 4) {
        int mine = 0;
        for (int i = 0; i < n; i++) mine += *(DWORD *)(rows + i * 0xa0 + 0x10) == pt;
        DWORD t = mine < 2 ? pt : 1 ^ pt;   // the game's result builder wants 2 teams of 2
        ULONG64 key = ~0ull;   // a member key no row uses (the bridge positions that were taken out)
        for (ULONG64 k = 0; k < 4 && key == ~0ull; k++) {
            bool used = false;
            for (int i = 0; i < n; i++) used |= *(ULONG64 *)(rows + i * 0xa0) == k;
            if (!used) key = k;
        }
        if (key == ~0ull) return;
        DWORD unit = br ? *(DWORD *)(br + 0x31158 + 4 * key) : 0;   // that position's unit in the bridge's setup
        if (!unit) unit = *(DWORD *)(rows + cpu * 0xa0 + 0xc);
        BYTE *r = rows + n * 0xa0;
        memcpy(r, rows + cpu * 0xa0, 0xa0);
        *(ULONG64 *)r = key;
        *(DWORD *)(r + 0xc) = unit;
        *(DWORD *)(r + 0x10) = t;
        n++;
        ov_log("training: result rows: row added for member %llu (unit %lu, %s team)", key, unit, t == pt ? "your" : "enemy");
    }
    *count = 4;
    InterlockedIncrement(&g_rows_fixed);
}

// After the game's result builder ran, the bridge checks that every row got its place (bridge 0x7AB1: mov eax,[rcx]
// / cmp [rdx],eax / jne error 5); the made-up rows don't (ScoreErrors 5, 2026-11-21), so with made-up rows that jne
// is off too and they go on with place 0 (the bridge's own fallback for a key the match doesn't know).
static const DWORD BR_PLACE_CHECK = 0x7ab1;
static const BYTE SIG_PLACE_CHECK[10] = { 0x8b, 0x01, 0x39, 0x02, 0x0f, 0x85, 0x26, 0x02, 0x00, 0x00 };

static void rows_hook(bool on) {
    BYTE *br = (BYTE *)GetModuleHandleW(L"training_bridge.dll");
    if (!br || on == g_rows_hooked) return;
    BYTE *at = br + BR_ROWS_CHECK;
    if (!on) {
        write_mem(at, SIG_ROWS_CHECK, sizeof(SIG_ROWS_CHECK));
        write_mem(br + BR_PLACE_CHECK, SIG_PLACE_CHECK, sizeof(SIG_PLACE_CHECK));
        g_rows_hooked = false;
        return;
    }
    if (memcmp(br + BR_PLACE_CHECK, SIG_PLACE_CHECK, sizeof(SIG_PLACE_CHECK))) { ov_log("training: the bridge's result place check isn't the expected code: no 1v2 result screen"); return; }
    if (memcmp(at, SIG_ROWS_CHECK, sizeof(SIG_ROWS_CHECK))) { ov_log("training: the bridge's result rows check isn't the expected code: no 1v2 result screen"); return; }
    if (!g_rows_page) {
        BYTE *pg = alloc_near_exe(br);
        if (!pg) return;
        static const BYTE head[] = {
            0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,   // push rax rcx rdx r8 r9 r10 r11
            0x48, 0x83, 0xec, 0x28,                                             // sub rsp,28h (shadow space, rsp 16-aligned)
            0x48, 0x89, 0xe9,                                                   // mov rcx,rbp
            0x4c, 0x89, 0xe2,                                                   // mov rdx,r12
            0x48, 0xb8 };                                                       // mov rax,imm64
        static const BYTE tail[] = {
            0xff, 0xd0,                                                         // call rax
            0x48, 0x83, 0xc4, 0x28,                                             // add rsp,28h
            0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58,   // pop r11 r10 r9 r8 rdx rcx rax
            0x41, 0x83, 0x7c, 0x24, 0x28, 0x04,                                 // cmp dword [r12+28h],4
            0x75, 0x0e };                                                       // jne +14 (past the first jmp)
        BYTE *p = pg;
        memcpy(p, head, sizeof(head)); p += sizeof(head);
        void *fn = (void *)tr_payload_fix; memcpy(p, &fn, 8); p += 8;
        memcpy(p, tail, sizeof(tail)); p += sizeof(tail);
        write_abs_jmp(p, br + BR_ROWS_OK); p += 14;
        write_abs_jmp(p, br + BR_ROWS_BAD);
        FlushInstructionCache(GetCurrentProcess(), pg, 4096);
        g_rows_page = pg;
    }
    BYTE patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)(g_rows_page - (at + 5));
    memcpy(patch + 1, &rel, 4);
    write_mem(at, patch, sizeof(patch));
    static const BYTE nop6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    write_mem(br + BR_PLACE_CHECK + 4, nop6, 6);
    g_rows_hooked = true;
    ov_log("training: 1v2: the result screen gets a row for the partner");
}

// ---- 1v2 rematch without the result screen -------------------------------------------------------------------
// The bridge's result data is made for 4 players (the 4-eligible check, a loop of 4 rows, the 2v2 layout), so in
// 1v2 its finish fails (phase 4, 1 error) and there's no result screen. The rematch doesn't need it: the bridge's
// exit hook (bridge fn 0x42E0, on game fn 0x7DB5F0) only asks for finish phase 3 with 0 errors before it sets
// RestartPending. In 1v2 those two jne are turned off in the loaded bridge (memory only), so a 1v2 match goes
// straight to the rematch; its other conditions (the training's state function and its exit flags) stay.
static const DWORD BR_REMATCH_CHECK = 0x434d;
static const BYTE SIG_REMATCH_CHECK[18] = { 0x83, 0x3d, 0xa4, 0xe2, 0x02, 0x00, 0x03, 0x75, 0x69,    // cmp FinishPhase,3; jne
                                            0x83, 0x3d, 0x97, 0xe2, 0x02, 0x00, 0x00, 0x75, 0x60 };  // cmp FinishErrors,0; jne
static bool g_rematch_check_off;
static void rematch_check(bool off) {
    if (off == g_rematch_check_off) return;
    BYTE *br = (BYTE *)GetModuleHandleW(L"training_bridge.dll");
    if (!br) return;
    static const BYTE nop2[2] = { 0x90, 0x90 };
    if (off) {
        if (!patch_code(br + BR_REMATCH_CHECK, SIG_REMATCH_CHECK, sizeof(SIG_REMATCH_CHECK), 7, nop2, 2, "bridge rematch")) return;
        BYTE half[18]; memcpy(half, SIG_REMATCH_CHECK, 18); memcpy(half + 7, nop2, 2);
        if (!patch_code(br + BR_REMATCH_CHECK, half, sizeof(half), 16, nop2, 2, "bridge rematch")) { write_mem(br + BR_REMATCH_CHECK, SIG_REMATCH_CHECK, 18); return; }
        ov_log("training: 1v2: rematch without the result screen");
    } else write_mem(br + BR_REMATCH_CHECK, SIG_REMATCH_CHECK, sizeof(SIG_REMATCH_CHECK));
    g_rematch_check_off = off;
}

// ---- rematch: the partner moved / taken out again -----------------------------------------------------------
// The bridge's rematch calls the game's match restart (RVA 0x7A0710, rcx = the training object): it makes the new
// match from the training slots without going through the builder, and the rematch came out 2v2 (2026-11-21). A
// detour on it moves the partner (1v3) / takes it out (1v2) again right before, logging the teams it found.
static const DWORD RVA_RESTART = 0x7a0710;
static const BYTE RESTART_PROLOGUE[10] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };   // mov [rsp+10h],rbx; mov [rsp+18h],rsi
typedef void *(*RestartFn)(void *, void *, void *, void *);
static RestartFn g_restart_tramp;
static BYTE *g_restart_page;
static bool g_restart_hooked;

extern "C" void *tr_restart_detour(void *obj, void *a, void *b, void *c) {
    if (g_flip_partner && obj) {
        BYTE *o = (BYTE *)obj;
        DWORD n = *(DWORD *)(o + O_UNIT_COUNT);
        ov_log("training: rematch: %lu units, teams %lu %lu %lu %lu", n, *(DWORD *)(o + SLOT0 + 0x160), *(DWORD *)(o + SLOT0 + SLOT_SIZE + 0x160),
               *(DWORD *)(o + SLOT0 + 2 * SLOT_SIZE + 0x160), *(DWORD *)(o + SLOT0 + 3 * SLOT_SIZE + 0x160));
        apply_layout(o);
    }
    return g_restart_tramp(obj, a, b, c);
}

static bool restart_hook(BYTE *base) {
    if (g_restart_hooked) return true;
    BYTE *fn = base + RVA_RESTART;
    if (memcmp(fn, RESTART_PROLOGUE, sizeof(RESTART_PROLOGUE))) { ov_log("training: the game's match restart isn't the expected code: no 1v3 / 1v2 rematch fix"); return false; }
    if (!g_restart_page) {
        BYTE *pg = alloc_near_exe(base);
        if (!pg) return false;
        write_abs_jmp(pg, (const void *)tr_restart_detour);
        memcpy(pg + 16, fn, 5);                  // mov [rsp+10h],rbx (position-independent)
        write_abs_jmp(pg + 21, fn + 5);
        FlushInstructionCache(GetCurrentProcess(), pg, 4096);
        g_restart_tramp = (RestartFn)(pg + 16);
        g_restart_page = pg;
    }
    BYTE patch[5] = { 0xE9 };
    int32_t rel = (int32_t)(g_restart_page - (fn + 5));
    memcpy(patch + 1, &rel, 4);
    write_mem(fn, patch, 5);
    g_restart_hooked = true;
    return true;
}

static bool chain_hook(BYTE *base) {
    if (g_chain_hooked) return true;
    BYTE *fn = base + RVA_BUILDER;
    if (fn[0] != 0xE9) { set_status("error", "1v3 with team cost: the bridge's hook wasn't found"); return false; }
    BYTE *bridge = fn + 5 + *(int32_t *)(fn + 1);
    if (!g_chain_page) {
        BYTE *pg = alloc_near_exe(base);
        if (!pg) { set_status("error", "No memory near the game for the 1v3 hook"); return false; }
        write_abs_jmp(pg, (const void *)tr_chain_detour);
        write_abs_jmp(pg + 16, bridge);   // "next" = where the bridge's jmp went
        FlushInstructionCache(GetCurrentProcess(), pg, 4096);
        g_chain_next = (BuilderFn)(pg + 16);
        g_chain_page = pg;
    }
    BYTE patch[5] = { 0xE9 };
    int32_t rel = (int32_t)(g_chain_page - (fn + 5));
    memcpy(patch + 1, &rel, 4);
    write_mem(fn, patch, 5);
    g_chain_hooked = true;
    ov_log("training: 1v3 with team cost: hook chained in front of the bridge's");
    return true;
}

static DWORD WINAPI training_thread(LPVOID) {
    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%straining.ini", g_tdir);
    FILETIME last = {};
    Config c = {};
    bool have = false, failed = false;
    DWORD want_seq = 0, start = GetTickCount();
    int last_seq = -1; bool last_enabled = false;
    load_start_bindings();
    for (int tick = 0;; tick++) {
        Sleep(100);
        if (g_loaded) watch_rules();
        if (InterlockedExchange(&g_flip_slots, 0) && g_flip_partner) log_owner_after_match();
        watch_1v2();
        if (tick % 5) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(ini, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &last)) {
            last = fa.ftLastWriteTime;
            read_config(ini, &c); have = true; failed = false;   // a new setup: try again after an error
        }
        if (!have || !c.enabled) {
            if (g_loaded) { disable(); }
            if (g_1v2_on || g_1v2_patched) stop_1v2();
            if (have && last_enabled != c.enabled) set_status("off", "Custom Training is off");
            last_enabled = have && c.enabled;
            continue;
        }
        // 1v3 / 1v2 with team cost: the bridge's rules on its 2v2, the partner moved / taken out after its setup
        // the bridge's 2v2 rearranged: 1v1 / 2v1 / 3v1 always, 1v2 / 1v3 with team cost (without it: the game's own
        // training, below)
        bool bridge_1v3 = c.partner == 3 || (c.partner != 1 && c.consume_cost);
        LONG solo_mode = bridge_1v3 ? 1 : 0;
        int outs = 0;
        for (int p = 1; p < 4; p++) outs += c.layout[p] < 0;
        char solo_name[8]; snprintf(solo_name, sizeof(solo_name), "%dv%d", 1 + c.allies, c.enemies);
        if (c.partner != 1 && !bridge_1v3) {   // 1v2 / 1v3: the game's own training with 3 or 4 units, no bridge
            int units = c.partner == 2 ? 4 : 3;
            const char *mode = units == 4 ? "1v3" : "1v2";
            static int units_on;
            if (g_loaded) {
                disable();
                set_status("error", "2v2 already ran in this game session: restart the game to play %s", mode);
                last_enabled = true; continue;
            }
            if (g_1v2_on && units_on != units) { set_1v2_config(c); if (!start_1v2(units)) { failed = true; continue; } units_on = units; last_seq = -1; }
            if (!g_1v2_on) {
                if (failed) continue;
                if (!ov_game_hwnd() && GetTickCount() - start < 30000) continue;
                if (GetTickCount() - start < 8000) continue;
                set_1v2_config(c);
                if (!start_1v2(units)) { failed = true; continue; }
                units_on = units; last_seq = -1;
            }
            static LONG built_seen;
            if (c.seq != last_seq || !last_enabled) {
                set_1v2_config(c); last_seq = c.seq; last_enabled = true; built_seen = g_1v2_built;
                set_status("queued", "Start a VS Training match to use the new setup (%s)", mode);
            } else if (g_1v2_built != built_seen) {
                built_seen = g_1v2_built;
                set_status("applied", "%s setup is active in VS Training", mode);
            }
            continue;
        }
        if (g_1v2_on || g_1v2_patched) { stop_1v2(); last_seq = -1; }   // partner back on: 2v2 with the bridge
        if (!g_loaded) {
            if (failed) continue;
            // let the game finish starting first (window up), like loading the bridge from a running game
            if (!ov_game_hwnd() && GetTickCount() - start < 30000) continue;
            if (GetTickCount() - start < 8000) continue;
            write_entry_ini(!bridge_1v3);
            if (!load_bridge()) { failed = true; continue; }
            g_bridge_entry = bridge_1v3 ? 0 : 1;
            last_seq = -1;
        }
        if (g_bridge_entry != (bridge_1v3 ? 0 : 1)) {   // the bridge read its intro setting for the other mode
            disable(); InterlockedExchange(&g_flip_partner, 0);
            if (bridge_1v3) set_status("error", "2v2 already ran in this game session: restart the game to play %s with team cost", solo_name);
            else set_status("error", "1v3 / 1v2 with team cost ran in this game session: restart the game to play 2v2");
            last_enabled = true; last_seq = -1; continue;
        }
        if (bridge_1v3 && !chain_hook((BYTE *)GetModuleHandleW(NULL))) { failed = true; continue; }
        if (bridge_1v3) finish_hook();   // diagnostics only: nothing depends on it
        team_check(bridge_1v3);                // the result screen with teams other than 2 and 2 (1v3, 3v1)
        rematch_check(bridge_1v3 && outs > 0);  // fewer than 4 units: the rematch even when the result screen fails
        rows_hook(bridge_1v3 && outs > 0);      // fewer than 4 units: made-up rows for the result screen
        if (bridge_1v3) restart_hook((BYTE *)GetModuleHandleW(NULL));   // the rematch keeps the 1v3 / 1v2
        if (bridge_1v3) {   // the layout first, then the flag the game thread checks
            bool same = g_flip_partner != 0;
            for (int p = 0; p < 4; p++) { same &= g_layout[p] == c.layout[p]; InterlockedExchange(&g_layout[p], c.layout[p]); }
            InterlockedExchange(&g_layout_out, outs);
            if (!same) last_seq = -1;
        }
        if (g_flip_partner != solo_mode) { InterlockedExchange(&g_flip_partner, solo_mode); last_seq = -1; }
        if (InterlockedExchange(&g_match_over, 0) && any_random_ex(c) && c.seq == last_seq && last_enabled) {
            want_seq = submit(c);   // same setup, new random EX; the status stays as it is
            ov_log("training: random EX / units drawn again for the next match");
        }
        if (c.seq != last_seq || !last_enabled) {
            want_seq = submit(c); last_seq = c.seq; last_enabled = true;
            set_status("queued", "Start a VS Training match to use the new setup");
        }
        DWORD ready, err, applied, mode, mode_applied;
        if (!read_state(&ready, &err, &applied, &mode, &mode_applied)) continue;
        if (err && applied != want_seq) set_status("error", "Bridge: %s", bridge_error(err));
        else if (applied == want_seq && mode_applied == want_seq && mode == 1) {
            if (bridge_1v3) set_status("applied", "%s setup (team cost) is active in VS Training", solo_name);
            else set_status("applied", "2v2 setup is active in VS Training");
        }
    }
}

void training_init(void) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char *n = strrchr(exe, '\\'); n = n ? n + 1 : exe;
    if (_stricmp(n, "vsac29_Release.exe")) return;   // the client only (not the Live Monitor)
    if (split_active()) return;                       // one copy per player: no 4-unit training there
    InitializeCriticalSection(&g_1v2_cs);
    snprintf(g_tdir, sizeof(g_tdir), "%sTraining\\", ov_dir());
    DWORD a = GetFileAttributesA(g_tdir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;   // not installed by the launcher
    HANDLE t = CreateThread(NULL, 0, training_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
