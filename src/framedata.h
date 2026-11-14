// Training tools (frame meter + skeleton): see framedata.cpp.
#pragma once
#include <windows.h>
#include <stdint.h>

struct FdSummary {
    bool valid;        // an action finished
    bool hit;          // it connected
    bool chained;      // cancelled into the next stage (no advantage)
    int startup;       // frame of the first hit (1 = first frame of the action), -1 on whiff
    int total;         // frames until the unit could act (or until the next stage)
    bool adv_known; int adv;   // frame advantage after the action (+ = attacker acts first)
};

enum { FD_NONE, FD_FREE, FD_STARTUP, FD_ACTIVE, FD_RECOVERY, FD_HITSTUN, FD_DOWN, FD_GUARD };

void fd_init(HANDLE proc, uintptr_t exe_base);   // starts the discovery thread (checks the exe version first)
void fd_update(bool want);                       // every Present: reads the game, advances the meter
const char *fd_status(void);
bool fd_active(void);                            // Training / Practice battle found
int fd_unit_count(void);
int fd_player(void);                             // unit index of the player (nearest to the camera), -1 if unknown
int fd_target(void);
const char *fd_unit_code(int i);                 // e.g. "069GQUUUX"
const char *fd_unit_name(int i);                 // e.g. "GQuuuuuuX" (falls back to the code)
int fd_cell(int i, int back);                    // FD_* of unit i, back frames ago
const FdSummary *fd_summary(int slot);           // 0 = player, 1 = target
bool fd_project(const float *world, float *ndc_x, float *ndc_y);
int fd_bones(int i, float (*out)[3], int max);   // live bone positions (world)
int fd_hurt(int i, float (*out)[4], int max);    // hurt spheres: x, y, z, radius (world)
int fd_hit(int i, float (*out)[4], int max);     // hit spheres of the current move, only in its active frames
bool fd_has_hitboxes(int i);                     // the unit has hitbox data (units with an action table)
int fd_proj(float (*out)[4], int max);           // projectiles in flight: x, y, z, radius (research: radius fixed)
int fd_learned(void);                            // hit start of approach moves learned (-1 while learning)
bool fd_project_sphere(const float *c, float r, float *x, float *y, float *r_ndc);
void fd_log(const char *fmt, ...);
