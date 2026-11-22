// Replays on every machine: loads Replay\EXVSReplay.dll, M1kou's "EXVS multi-function patch" (20261028), when
// the EXVS Launcher put it there (Gameplay > Replays).
//
// The game records a match on one machine only: the one with the lowest key (the "elected" one), which sends
// it to the Live Monitor. M1kou's patch hooks the game's replay step machine so the other machines record the
// match too and keep it on disk only (29\replay_store, as the game does), and on the LM it adds the loose .bin
// files to replay_list.json. It checks the exe itself (entry point + 16-byte fingerprints of every target) and
// installs nothing on an exe it doesn't know.
//
// It's built as an xinput9_1_0.dll proxy, like this overlay; renamed and loaded from here it only does the
// replay part: its input blocking is off in the config the launcher writes ([InputFix] Enabled = 0; the overlay
// does that itself). Its config (xinputPatchConfig.ini) and log (xinputPatch.log) stay next to it in Replay\.
// The DLL is checked by SHA-256, so only the file the launcher ships gets loaded.
#include "menu.h"
#include <stdio.h>
#include <string.h>

bool ov_sha256_file(const wchar_t *path, char out[65]);   // training.cpp

static const char *REPLAY_SHA256 = "D78CF6BDC178B2E8E2116573766B93BFF4FD2B1686CBE59B0787444FF1B2D632";   // 20261028

void replay_init(void) {
    wchar_t dir[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, ov_dir(), -1, dir, MAX_PATH); wcscat(dir, L"Replay\\");
    wchar_t old[MAX_PATH]; wcscpy(old, dir); wcscat(old, L"EXVSReplay.old.dll");
    if (DeleteFileW(old)) ov_log("replay: removed leftover Replay\\EXVSReplay.old.dll");
    wchar_t dll[MAX_PATH]; wcscpy(dll, dir); wcscat(dll, L"EXVSReplay.dll");
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) return;   // off in the launcher
    // splitscreen: the copies share the game folder, so only player 1's copy keeps the replay (the others would
    // write the same match into the same replay_store)
    if (split_player() > 1) { ov_log("replay: splitscreen player %d, only player 1 saves replays", split_player()); return; }
    char sha[65];
    if (!ov_sha256_file(dll, sha)) { ov_log("replay: can't read Replay\\EXVSReplay.dll"); return; }
    if (strcmp(sha, REPLAY_SHA256)) { ov_log("replay: Replay\\EXVSReplay.dll isn't the expected file (SHA-256 %s), not loaded", sha); return; }
    if (!LoadLibraryW(dll)) { ov_log("replay: EXVSReplay.dll didn't load (%lu)", GetLastError()); return; }
    ov_log("replay: loaded M1kou's replay patch (Replay\\EXVSReplay.dll, its log: Replay\\xinputPatch.log)");
}
