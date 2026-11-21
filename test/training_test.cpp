// Offline test of training.cpp: settings file parsing, the shared block layout written for the bridge
// (protocol 8, same offsets as aerial's TrainingApi) and the 2v2 code patches on a copy of the code bytes.
//   zig c++ ... test/training_test.cpp -o build/training_test.exe ; build/training_test.exe <temp dir>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static char g_test_dir[MAX_PATH];
void ov_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
const char *ov_dir(void) { return g_test_dir; }
HWND ov_game_hwnd(void) { return NULL; }
bool split_active(void) { return false; }
const char *ov_cfg_dir(void) { return g_test_dir; }
int menu_key_vk(const char *n) { return n[0] == 'B' ? 'B' : 0; }

#include "../src/training.cpp"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: training_test <empty temp dir>\n"); return 2; }
    snprintf(g_test_dir, sizeof(g_test_dir), "%s\\", argv[1]);
    snprintf(g_tdir, sizeof(g_tdir), "%sTraining\\", g_test_dir);
    CreateDirectoryA(g_tdir, NULL);

    // --- settings file (as the launcher writes it)
    char ini[MAX_PATH]; snprintf(ini, sizeof(ini), "%straining.ini", g_tdir);
    FILE *f = fopen(ini, "w");
    fprintf(f, "[Training]\nUnit1=0\nLevel1=0\nEx1=-1\nUnit2=1001001\nCpu2=7\nEx2=0\nUnit3=-1\nLevel3=5\nEx3=1\n"
               "Unit4=601001001\nCpu4=12\nEx4=2\nPool=2001001,1001001,1001001,3001001\nRandomStage=1\nConsumeCost=0\nEnabled=1\nSeq=4\n");
    fclose(f);
    Config c; read_config(ini, &c);
    CHECK(c.enabled && c.seq == 4 && c.random_stage && !c.consume_cost);
    CHECK(c.unit[0] == 0 && c.unit[1] == 1001001 && c.unit[2] == -1 && c.unit[3] == 601001001);
    CHECK(c.level[1] == 7 && c.level[2] == 3 && c.level[3] == 9);   // old Level3 ignored (default hard), clamped to 0-9
    CHECK(c.ex[0] == -1 && c.ex[1] == 0 && c.ex[2] == 1 && c.ex[3] == 2);
    CHECK(c.npool == 3 && c.pool[0] == 1001001 && c.pool[1] == 2001001 && c.pool[2] == 3001001);   // sorted, no duplicates

    // --- block layout
    static BYTE block[B_SIZE];
    g_block = block;
    DWORD seq = submit(c);
    CHECK(seq == 2 && rd(O_SEQ) == 2 && rd(O_ENABLED) == 1);
    CHECK(rd(O_RANDOM_STAGE) == 1 && rd(O_SAVE_MASTERY) == 0 && rd(O_CONSUME_COST) == 0);
    CHECK(rd(O_POOL_COUNT) == 3 && rd(O_POOL) == 1001001 && rd(O_POOL + 8) == 3001001);
    // slot 0: machine 0 (game's pick), level 0, EX 0xFFFFFFFF (game's pick), team 0
    CHECK(rd(24) == 0 && rd(28) == 0 && rd(32) == 0xffffffffu && rd(36) == 0);
    CHECK(rd(40) == 1001001 && rd(44) == 7 && rd(48) == 0 && rd(52) == 0);
    CHECK(rd(56) == 0xffffffffu && rd(60) == 3 && rd(64) == 1 && rd(68) == 1);       // random enemy, team 1
    CHECK(rd(72) == 601001001 && rd(76) == 9 && rd(80) == 2 && rd(84) == 1);
    disable();
    CHECK(rd(O_ENABLED) == 0 && rd(O_SEQ) == 4);
    c.unit[2] = 2001001; seq = submit(c);
    CHECK(seq == 6 && rd(O_POOL_COUNT) == 0);   // no random slot: no pool

    // --- random EX (3): a real F / S / E is written, and the draws vary
    c.ex[1] = 3; c.ex[0] = 3;
    CHECK(any_random_ex(c));
    int seen[3] = {};
    for (int k = 0; k < 60; k++) { submit(c); DWORD e0 = rd(32), e1 = rd(48); CHECK(e0 <= 2 && e1 <= 2); if (e1 <= 2) seen[e1]++; }
    CHECK(seen[0] && seen[1] && seen[2]);
    c.ex[0] = -1; c.ex[1] = 0;
    CHECK(!any_random_ex(c));

    // --- random unit by category: drawn by the overlay from SlotPool<n>
    f = fopen(ini, "w");
    fprintf(f, "[Training]\nUnit1=0\nUnit2=-2500\nSlotPool2=2001001,3001001\nUnit3=-9\nSlotPool3=601001001,654003001\n"
               "Unit4=-1500\nSlotPool4=\nPool=1001001,2001001\nEnabled=1\nSeq=5\n");
    fclose(f);
    Config k2; read_config(ini, &k2);
    CHECK(k2.unit[1] == -2500 && k2.nslot[1] == 2 && k2.unit[2] == -9 && k2.nslot[2] == 2);
    CHECK(k2.unit[3] == -1);   // empty list: any unit (bridge random)
    CHECK(any_random_ex(k2));
    bool got_a = false, got_b = false;
    for (int k = 0; k < 40; k++) {
        submit(k2);
        DWORD u1 = rd(40), u2 = rd(56);
        CHECK(u1 == 2001001 || u1 == 3001001);
        CHECK(u2 == 601001001 || u2 == 654003001);
        got_a |= u1 == 2001001; got_b |= u1 == 3001001;
    }
    CHECK(got_a && got_b);
    CHECK(rd(72) == 0xffffffffu && rd(O_POOL_COUNT) > 0);   // slot 4 random through the bridge pool

    // --- 1v2: setup written into the game's 3 training slots (fake training object, human in slot 1)
    InitializeCriticalSection(&g_1v2_cs);
    static BYTE obj[0x1400];
    *(ULONG64 *)obj = 0x1122334455667788ull;
    *(DWORD *)(obj + O_UNIT_COUNT) = 3;
    for (int i = 0; i < 3; i++) {
        BYTE *sl = obj + SLOT0 + i * SLOT_SIZE;
        *(DWORD *)(sl + 0x158) = i; *(DWORD *)(sl + 0x160) = i == 1 ? 0 : 1; *(DWORD *)(sl + 0x164) = 1001001;
    }
    obj[SLOT0 + 1 * SLOT_SIZE] = 1;   // the human player
    Config v = {}; v.unit[0] = 0; v.ex[0] = -1;
    v.unit[2] = 2001001; v.level[2] = 3; v.ex[2] = 1;
    v.unit[3] = -9; v.slot_pool[3][0] = 601001001; v.nslot[3] = 1; v.level[3] = 0; v.ex[3] = 2;
    set_1v2_config(v);
    InterlockedExchange(&g_1v2_on, 1);
    fill_1v2(obj);
    BYTE *s0 = obj + SLOT0, *s1 = obj + SLOT0 + SLOT_SIZE, *s2 = obj + SLOT0 + 2 * SLOT_SIZE;
    CHECK(*(DWORD *)(s1 + 0x164) == 1001001 && *(DWORD *)(s1 + 0x160) == 0);   // you: unit picked in game kept
    CHECK(*(DWORD *)(s0 + 0x160) == 1 && *(DWORD *)(s0 + 0x164) == 2001001 && *(DWORD *)(s0 + 0x168) == 1);
    CHECK(*(DWORD *)(s0 + 0x16c) == 7 && *(DWORD *)(s0 + 0x170) == 6 && *(DWORD *)(s0 + 0x174) == 0);   // hard
    CHECK(*(DWORD *)(s2 + 0x160) == 1 && *(DWORD *)(s2 + 0x164) == 601001001 && *(DWORD *)(s2 + 0x168) == 2);
    CHECK(*(DWORD *)(s2 + 0x16c) == 0 && *(DWORD *)(s2 + 0x170) == 0 && *(DWORD *)(s2 + 0x174) == 1);   // stand still
    CHECK(training_block_start());   // Start hidden while the 1v2 training object lives
    *(ULONG64 *)obj = 0;             // object gone / reused
    watch_1v2();
    CHECK(!training_block_start());
    *(DWORD *)(obj + O_UNIT_COUNT) = 4; *(DWORD *)(s0 + 0x164) = 5; fill_1v2(obj);
    CHECK(*(DWORD *)(s0 + 0x164) == 5);   // not 3 units: nothing written
    InterlockedExchange(&g_1v2_on, 0);

    // --- 1v3: 4 slots, human in slot 0; the partner card (launcher slot 1) is the third enemy
    *(ULONG64 *)obj = 0x1122334455667788ull;
    *(DWORD *)(obj + O_UNIT_COUNT) = 4;
    for (int i = 0; i < 4; i++) {
        BYTE *sl = obj + SLOT0 + i * SLOT_SIZE;
        sl[0] = i == 0; *(DWORD *)(sl + 0x158) = i; *(DWORD *)(sl + 0x15c) = 0; *(DWORD *)(sl + 0x160) = i >= 2; *(DWORD *)(sl + 0x164) = 1001001;
    }
    Config w = {}; w.partner = 2; w.unit[0] = 0; w.ex[0] = -1;
    w.unit[1] = 3001001; w.level[1] = 4; w.ex[1] = 0;
    w.unit[2] = 2001001; w.level[2] = 2; w.ex[2] = 1;
    w.unit[3] = 4001001; w.level[3] = 3; w.ex[3] = 2;
    set_1v2_config(w);
    InterlockedExchange(&g_1v2_on, 1);
    fill_1v2(obj);
    {
        BYTE *a = obj + SLOT0, *b = a + SLOT_SIZE, *c3 = a + 2 * SLOT_SIZE, *d = a + 3 * SLOT_SIZE;
        CHECK(*(DWORD *)(a + 0x160) == 0 && *(DWORD *)(a + 0x164) == 1001001);                 // you
        CHECK(*(DWORD *)(b + 0x160) == 1 && *(DWORD *)(b + 0x164) == 2001001 && *(DWORD *)(b + 0x16c) == 5);    // Enemy 1, normal
        CHECK(*(DWORD *)(c3 + 0x160) == 1 && *(DWORD *)(c3 + 0x164) == 4001001 && *(DWORD *)(c3 + 0x16c) == 7); // Enemy 2, hard
        CHECK(*(DWORD *)(d + 0x160) == 1 && *(DWORD *)(d + 0x164) == 3001001 && *(DWORD *)(d + 0x16c) == 9);    // partner card, extreme
    }
    *(DWORD *)(obj + O_UNIT_COUNT) = 3; *(DWORD *)(obj + SLOT0 + SLOT_SIZE + 0x164) = 7; fill_1v2(obj);
    CHECK(*(DWORD *)(obj + SLOT0 + SLOT_SIZE + 0x164) == 7);   // 1v3 wants 4 units: nothing written with 3
    InterlockedExchange(&g_1v2_on, 0);
    InterlockedExchange64(&g_1v2_obj, 0);

    // --- 1v3 with team cost: after the bridge's 2v2 (teams 0,0,1,1 around the human in slot 2) the partner moves
    *(DWORD *)(obj + O_UNIT_COUNT) = 4;
    for (int i = 0; i < 4; i++) {
        BYTE *sl = obj + SLOT0 + i * SLOT_SIZE;
        sl[0] = i == 2; *(DWORD *)(sl + 0x164) = 1000 + i;
    }
    // human slot 2 on team 1 (the bridge writes team ^ player team): partner = first other slot (0), enemies 1 and 3
    *(DWORD *)(obj + SLOT0 + 2 * SLOT_SIZE + 0x160) = 1; *(DWORD *)(obj + SLOT0 + 0x160) = 1;
    *(DWORD *)(obj + SLOT0 + SLOT_SIZE + 0x160) = 0; *(DWORD *)(obj + SLOT0 + 3 * SLOT_SIZE + 0x160) = 0;
    CHECK(flip_partner(obj, false));
    CHECK(*(DWORD *)(obj + SLOT0 + 0x160) == 0 && *(DWORD *)(obj + SLOT0 + SLOT_SIZE + 0x160) == 0 && *(DWORD *)(obj + SLOT0 + 3 * SLOT_SIZE + 0x160) == 0);
    CHECK(*(DWORD *)(obj + SLOT0 + 2 * SLOT_SIZE + 0x160) == 1 && g_flips == 1);   // you alone on your team
    CHECK(!flip_partner(obj, true));   // already moved (a rematch): the first other slot isn't on your team any more
    CHECK(g_flips == 1);

    // --- the builder hook, on the real game exe mapped as an image (not run): argv[3]
    if (argc > 3) {
        wchar_t exe[MAX_PATH]; swprintf(exe, MAX_PATH, L"%hs", argv[3]);
        BYTE *base = (BYTE *)LoadLibraryExW(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
        CHECK(base != NULL);
        if (base) {
            BYTE *fn = base + RVA_BUILDER;
            CHECK(!memcmp(fn, BUILDER_PROLOGUE, 9));
            CHECK(builder_hook(base));
            CHECK(fn[0] == 0xE9 && fn + 5 + *(int32_t *)(fn + 1) == g_builder_page);
            CHECK(!memcmp(g_builder_page + 16, BUILDER_PROLOGUE, 9));                      // trampoline: stolen code
            CHECK(*(BYTE **)(g_builder_page + 16 + 9 + 6) == fn + 9);                     // ... then back to the function
            CHECK(*(void **)(g_builder_page + 6) == (void *)tr_builder_detour);           // relay -> detour
            write_mem(fn, BUILDER_PROLOGUE, 9); g_builder_hooked = false;
            CHECK(!memcmp(fn, BUILDER_PROLOGUE, 9));
            // chained in front of a (fake) bridge hook: fn starts with jmp rel32 -> X; ours must end up going to X
            BYTE *fake_bridge = g_builder_page + 0x200;
            BYTE j[5] = { 0xE9 }; int32_t rel = (int32_t)(fake_bridge - (fn + 5)); memcpy(j + 1, &rel, 4);
            write_mem(fn, j, 5);
            CHECK(chain_hook(base));
            CHECK(fn[0] == 0xE9 && fn + 5 + *(int32_t *)(fn + 1) == g_chain_page);
            CHECK(*(void **)(g_chain_page + 6) == (void *)tr_chain_detour && *(BYTE **)(g_chain_page + 16 + 6) == fake_bridge);
            write_mem(fn, BUILDER_PROLOGUE, 9); g_chain_hooked = false;
            CHECK(!memcmp(base + RVA_MEMBERS, SIG_MEMBERS, sizeof(SIG_MEMBERS)) && !memcmp(base + RVA_GATE, SIG_GATE, sizeof(SIG_GATE)));
            CHECK(!memcmp(base + RVA_FOURTH, SIG_FOURTH, 6));   // the 4th-unit jz (1v3 NOPs it)
            CHECK(!memcmp(base + RVA_STAGE_AT, SIG_STAGE_AT, sizeof(SIG_STAGE_AT)) && !memcmp(base + RVA_STAGE_OK, SIG_STAGE_OK, sizeof(SIG_STAGE_OK)));   // stage list functions
            CHECK(patch_code(base + RVA_FOURTH, SIG_FOURTH, 6, 0, NOP6, 6, "4th unit") && !memcmp(base + RVA_FOURTH, NOP6, 6));
            write_mem(base + RVA_FOURTH, SIG_FOURTH, 6);

            // rematch: the hook on the game's match restart (prologue, relay, trampoline)
            BYTE *rfn = base + RVA_RESTART;
            CHECK(!memcmp(rfn, RESTART_PROLOGUE, sizeof(RESTART_PROLOGUE)));
            CHECK(restart_hook(base));
            CHECK(rfn[0] == 0xE9 && rfn + 5 + *(int32_t *)(rfn + 1) == g_restart_page);
            CHECK(*(void **)(g_restart_page + 6) == (void *)tr_restart_detour);
            CHECK(!memcmp(g_restart_page + 16, RESTART_PROLOGUE, 5) && *(BYTE **)(g_restart_page + 21 + 6) == rfn + 5);
            write_mem(rfn, RESTART_PROLOGUE, 5); g_restart_hooked = false;
        }
    }

    // --- 1v2 with team cost: the partner slot taken out of the bridge's 4 (you in slot 0, partner 1, enemies 2 and 3)
    {
        static BYTE o2[SLOT0 + 4 * SLOT_SIZE + 0x200];
        memset(o2, 0, sizeof(o2));
        *(DWORD *)(o2 + O_UNIT_COUNT) = 4;
        for (int i = 0; i < 4; i++) {
            BYTE *sl = o2 + SLOT0 + i * SLOT_SIZE;
            sl[0] = i == 0; *(DWORD *)(sl + 0x158) = i; *(DWORD *)(sl + 0x160) = i >= 2; *(DWORD *)(sl + 0x164) = 5000 + i;
            sl[0x200] = (BYTE)(0x40 + i);   // something else of the slot: moves with it
        }
        LONG fl = g_flips;
        CHECK(drop_partner(o2) && g_flips == fl + 1 && *(DWORD *)(o2 + O_UNIT_COUNT) == 3);
        BYTE *s0 = o2 + SLOT0, *s1 = s0 + SLOT_SIZE, *s2 = s1 + SLOT_SIZE;
        CHECK(s0[0] == 1 && *(DWORD *)(s0 + 0x164) == 5000 && *(DWORD *)(s0 + 0x160) == 0);
        CHECK(*(DWORD *)(s1 + 0x164) == 5002 && *(DWORD *)(s1 + 0x158) == 1 && *(DWORD *)(s1 + 0x160) == 1 && s1[0x200] == 0x42);
        CHECK(*(DWORD *)(s2 + 0x164) == 5003 && *(DWORD *)(s2 + 0x158) == 2 && *(DWORD *)(s2 + 0x160) == 1 && s2[0x200] == 0x43);
        CHECK(!drop_partner(o2));   // 3 units now: left alone
    }

    // --- code patches (on a copy of the bytes)
    BYTE code[16];
    memcpy(code, SIG_MEMBERS, sizeof(SIG_MEMBERS));
    static const BYTE three = 3, gate_on[2] = { 0xb0, 0x01 };
    CHECK(patch_code(code, SIG_MEMBERS, sizeof(SIG_MEMBERS), 3, &three, 1, "player count") && code[3] == 3);
    CHECK(patch_code(code, SIG_MEMBERS, sizeof(SIG_MEMBERS), 3, &three, 1, "player count"));   // already on
    memcpy(code, SIG_GATE, sizeof(SIG_GATE));
    CHECK(patch_code(code, SIG_GATE, sizeof(SIG_GATE), 2, gate_on, 2, "2v2 gate") && code[2] == 0xb0 && code[3] == 0x01);
    code[5] = 0x90;   // something else changed this code
    CHECK(!patch_code(code, SIG_GATE, sizeof(SIG_GATE), 2, gate_on, 2, "2v2 gate"));

    // --- Start bindings from config.ini
    char cfg[MAX_PATH]; snprintf(cfg, sizeof(cfg), "%sconfig.ini", g_test_dir);
    f = fopen(cfg, "w");
    fprintf(f, "[keyboard]\nStart=B\n[controller]\nArcadeStartButton = 16\nStart=10,5\n[controller-1]\nStart=12\n");
    fclose(f);
    load_start_bindings();
    CHECK(training_start_vk() == 'B' && training_start_pad_mask() == 0x0010);
    CHECK(training_start_hid_button(10) && training_start_hid_button(5) && training_start_hid_button(12) && !training_start_hid_button(1));

    // --- 2v2 match detection from the bridge's rules block
    CHECK(!training_block_start());
    wchar_t rn[64]; swprintf(rn, 64, L"Local\\EXVS2IB_RULES_V13_%lu", GetCurrentProcessId());
    HANDLE rm = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 328, rn);
    DWORD *rv = (DWORD *)MapViewOfFile(rm, FILE_MAP_ALL_ACCESS, 0, 0, 328);
    rv[0] = 810700361u; rv[1] = 2; rv[2] = 328; rv[3] = GetCurrentProcessId(); rv[5] = 1; rv[6] = 0;
    watch_rules();
    CHECK(!training_block_start());   // rules ready, no 2v2 match
    rv[6] = 1; watch_rules();
    CHECK(training_block_start());
    rv[6] = 0; watch_rules();
    CHECK(!training_block_start());

    // --- bridge hash check on the shipped file
    char sha[65];
    wchar_t dll[MAX_PATH]; swprintf(dll, MAX_PATH, L"%hs", argc > 2 ? argv[2] : "");
    if (argc > 2) { CHECK(sha256_file(dll, sha) && !strcmp(sha, BRIDGE_SHA256)); }

    // --- the diagnostic hook on the bridge's finish, on the shipped bridge mapped as an image (not run)
    if (argc > 2) {
        BYTE *br = (BYTE *)LoadLibraryExW(dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
        CHECK(br != NULL && (BYTE *)GetModuleHandleW(L"training_bridge.dll") == br);
        if (br) {
            BYTE *fn = br + BR_FINISH;
            CHECK(!memcmp(fn, BR_FINISH_PROLOGUE, sizeof(BR_FINISH_PROLOGUE)));
            CHECK(finish_hook());
            CHECK(fn[0] == 0xE9 && fn + 5 + *(int32_t *)(fn + 1) == g_finish_page);
            CHECK(*(void **)(g_finish_page + 6) == (void *)tr_finish_detour);
            CHECK(!memcmp(g_finish_page + 16, BR_FINISH_PROLOGUE, 5) && *(BYTE **)(g_finish_page + 21 + 6) == fn + 5);
            CHECK(finish_hook());   // once
            // 1v3 result screen: the 2-per-team checks off and back on
            CHECK(!memcmp(br + BR_TEAM_CHECK, SIG_TEAM_CHECK, sizeof(SIG_TEAM_CHECK)));
            team_check(true);
            BYTE nop22[22]; memset(nop22, 0x90, sizeof(nop22));
            CHECK(g_team_check_off && !memcmp(br + BR_TEAM_CHECK, nop22, 22) && br[BR_TEAM_CHECK - 6] == 0x0f && br[BR_TEAM_CHECK + 22] == 0x44);
            team_check(false);
            CHECK(!g_team_check_off && !memcmp(br + BR_TEAM_CHECK, SIG_TEAM_CHECK, sizeof(SIG_TEAM_CHECK)));
            // 1v2 rematch: the phase 3 / 0 errors jumps off and back on
            CHECK(!memcmp(br + BR_REMATCH_CHECK, SIG_REMATCH_CHECK, sizeof(SIG_REMATCH_CHECK)));
            rematch_check(true);
            BYTE *rc = br + BR_REMATCH_CHECK;
            CHECK(g_rematch_check_off && rc[7] == 0x90 && rc[8] == 0x90 && rc[16] == 0x90 && rc[17] == 0x90 && !memcmp(rc, SIG_REMATCH_CHECK, 7) &&
                  !memcmp(rc + 9, SIG_REMATCH_CHECK + 9, 7));
            rematch_check(false);
            CHECK(!g_rematch_check_off && !memcmp(rc, SIG_REMATCH_CHECK, sizeof(SIG_REMATCH_CHECK)));
            // 1v2 result rows: the stub on the "4 rows?" check
            CHECK(!memcmp(br + BR_ROWS_CHECK, SIG_ROWS_CHECK, sizeof(SIG_ROWS_CHECK)));
            rows_hook(true);
            BYTE *ra = br + BR_ROWS_CHECK;
            CHECK(g_rows_hooked && ra[0] == 0xE9 && ra + 5 + *(int32_t *)(ra + 1) == g_rows_page && ra[5] == 0x90 && ra[7] == 0x90);
            CHECK(*(void **)(g_rows_page + 23) == (void *)tr_payload_fix);
            CHECK(*(BYTE **)(g_rows_page + 31 + 25 + 6) == br + BR_ROWS_OK && *(BYTE **)(g_rows_page + 31 + 25 + 14 + 6) == br + BR_ROWS_BAD);
            { char sp[MAX_PATH]; snprintf(sp, sizeof(sp), "%sstub.bin", g_test_dir); FILE *sf = fopen(sp, "wb"); fwrite(g_rows_page, 1, 31 + 25 + 28, sf); fclose(sf); }
            CHECK(br[BR_PLACE_CHECK + 4] == 0x90 && br[BR_PLACE_CHECK + 9] == 0x90 && !memcmp(br + BR_PLACE_CHECK, SIG_PLACE_CHECK, 4));
            rows_hook(false);
            CHECK(!g_rows_hooked && !memcmp(br + BR_ROWS_CHECK, SIG_ROWS_CHECK, sizeof(SIG_ROWS_CHECK)));
            CHECK(!memcmp(br + BR_PLACE_CHECK, SIG_PLACE_CHECK, sizeof(SIG_PLACE_CHECK)));
        }
    }

    // --- 1v2 result: the made-up 4th row (rows at frame+0x2F0, local key at frame+0xC0, count at out+0x28)
    {
        static BYTE frame[0x600], out[0x40];
        memset(frame, 0, sizeof(frame)); memset(out, 0, sizeof(out));
        BYTE *rows = frame + 0x2f0;
        ULONG64 keys[3] = { 0, 2, 3 }; DWORD teams[3] = { 0, 1, 1 };
        for (int i = 0; i < 3; i++) {
            *(ULONG64 *)(rows + i * 0xa0) = keys[i]; *(DWORD *)(rows + i * 0xa0 + 0xc) = 7000 + i; *(DWORD *)(rows + i * 0xa0 + 0x10) = teams[i];
            rows[i * 0xa0 + 0x40] = (BYTE)(0x60 + i);
        }
        *(ULONG64 *)(frame + 0xc0) = 0;   // you are member 0
        *(DWORD *)(out + 0x28) = 3;
        LONG was = g_flip_partner; g_flip_partner = 2;
        tr_payload_fix(frame, out);
        BYTE *r3 = rows + 3 * 0xa0;
        CHECK(*(DWORD *)(out + 0x28) == 4 && *(ULONG64 *)r3 == 1 && *(DWORD *)(r3 + 0x10) == 0 && r3[0x40] == 0x61);   // a copy of enemy row 1
        CHECK(*(DWORD *)(r3 + 0xc) != 0);
        *(DWORD *)(out + 0x28) = 3; g_flip_partner = 1;
        memset(r3, 0, 0xa0); tr_payload_fix(frame, out);
        CHECK(*(DWORD *)(out + 0x28) == 3 && *(ULONG64 *)r3 == 0);   // not 1v2: nothing
        g_flip_partner = was;
    }

    printf(fails ? "%d FAILED\n" : "all training tests passed\n", fails);
    return fails ? 1 : 0;
}
