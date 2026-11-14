// Automatic updates of the overlay DLL.
//
// The builds are published as releases of UPDATE_REPO on GitHub, with the bare DLL (UPDATE_ASSET)
// attached. A background thread asks the GitHub API for the latest release once, ~20 s after the game
// starts (no token needed). If its tag is newer than OVERLAY_VERSION, the menu shows it and the player
// can install it from About > Updates:
//     download        -> xinput9_1_0.new.dll  (size, SHA-256 from GitHub's asset "digest", "MZ" checked)
//     loaded DLL      -> xinput9_1_0.old.dll  (Windows allows renaming a loaded DLL)
//     downloaded DLL  -> xinput9_1_0.dll
// The new version runs from the next game start (the menu offers a restart). init_thread in
// overlay.cpp deletes the .old.dll on the next start.
//
// The network part runs in a helper process (rundll32 running UpdateHelperW from this same DLL): inside the game
// the loader (bngrw.dll) hooks the sockets for the card server, and WinHTTP from the game process can't reach
// GitHub (error 12029). The helper writes its results to EXVSOverlay.update.ini; the game process reads them.
//
// EXVSOVERLAY_NO_UPDATE=1 (and the test host's EXVSOVERLAY_TEST) turn the check off.
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"

#ifndef UPDATE_REPO   // (the test overrides these)
#define UPDATE_REPO "furshow/EXVSOverlay"
#define UPDATE_ASSET "xinput9_1_0.dll"
#endif
#ifndef UPDATE_MAX_MB
#define UPDATE_MAX_MB 64
#endif

// Version marker: EXVS Launcher reads it from the installed DLL, so it doesn't offer to "update" an
// overlay that updated itself to a version newer than the one the launcher bundles.
extern "C" __declspec(dllexport) const char EXVSOverlayVersionTag[] = "EXVSOverlayVersion=" OVERLAY_VERSION ";";

static volatile LONG g_state = UPD_IDLE;
static char g_ver[32], g_tag[48], g_url[1024], g_sha[65], g_notes[1024], g_err[256];
static unsigned long long g_size;
static volatile LONG64 g_done, g_total;
static volatile LONG g_busy;   // a check or install thread is running

static void fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_err, sizeof(g_err), fmt, ap); va_end(ap);
    ov_log("update: %s", g_err);
    InterlockedExchange(&g_state, UPD_FAILED);
}

static bool g_in_helper;   // running inside rundll32 (UpdateHelperW)

static void self_paths(wchar_t *self, wchar_t *nw, wchar_t *old);
static void ini_path(char *out) {
    wchar_t self[MAX_PATH], nw[MAX_PATH], old[MAX_PATH]; self_paths(self, nw, old);
    char d[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, self, -1, d, MAX_PATH, NULL, NULL);
    char *sl = strrchr(d, '\\'); if (sl) sl[1] = 0;
    snprintf(out, MAX_PATH, "%sEXVSOverlay.update.ini", d);
}
static void ini_put(const char *k, const char *v) { char p[MAX_PATH]; ini_path(p); WritePrivateProfileStringA("update", k, v, p); }
static void ini_putu(const char *k, unsigned long long v) { char b[32]; snprintf(b, sizeof(b), "%llu", v); ini_put(k, b); }
static void ini_get(const char *k, char *out, DWORD n) { char p[MAX_PATH]; ini_path(p); GetPrivateProfileStringA("update", k, "", out, n, p); }
static unsigned long long ini_getu(const char *k) { char b[32]; ini_get(k, b, sizeof(b)); return _strtoui64(b, NULL, 10); }

// helper -> ini (notes: newlines escaped as \n so they fit on one line)
static void save_result(void) {
    char notes[2 * sizeof(g_notes)]; size_t o = 0;
    for (const char *c = g_notes; *c && o + 2 < sizeof(notes); c++) {
        if (*c == '\n') { notes[o++] = '\\'; notes[o++] = 'n'; } else if (*c != '\r') notes[o++] = *c;
    }
    notes[o] = 0;
    ini_putu("state", (unsigned long long)g_state);
    ini_put("version", g_ver); ini_put("tag", g_tag); ini_put("url", g_url); ini_put("sha256", g_sha);
    ini_putu("size", g_size); ini_put("notes", notes); ini_put("error", g_err);
}
// ini -> globals (game process)
static void load_result(void) {
    char notes[2 * sizeof(g_notes)];
    ini_get("version", g_ver, sizeof(g_ver)); ini_get("tag", g_tag, sizeof(g_tag)); ini_get("url", g_url, sizeof(g_url));
    ini_get("sha256", g_sha, sizeof(g_sha)); g_size = ini_getu("size"); ini_get("error", g_err, sizeof(g_err));
    ini_get("notes", notes, sizeof(notes));
    size_t o = 0;
    for (const char *c = notes; *c && o + 1 < sizeof(g_notes); c++) {
        if (c[0] == '\\' && c[1] == 'n') { g_notes[o++] = '\n'; c++; } else g_notes[o++] = *c;
    }
    g_notes[o] = 0;
}

// ---------------------------------------------------------------------------------------------
// HTTPS GET with WinHTTP (follows GitHub's redirect to its download host)
// ---------------------------------------------------------------------------------------------
typedef bool (*Sink)(void *ctx, const char *data, DWORD n);

static bool http_get(const char *url_a, const wchar_t *accept, Sink sink, void *ctx, char *err, size_t errn) {
    wchar_t url[1024]; MultiByteToWideChar(CP_UTF8, 0, url_a, -1, url, 1024);
    wchar_t host[256] = L"", path[1024] = L"";
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    wchar_t extra[1024] = L""; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { snprintf(err, errn, "bad address"); return false; }
    wcsncat(path, extra, 1023 - wcslen(path));

    HINTERNET s = WinHttpOpen(L"EXVS-Overlay-updater/" OVERLAY_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) s = WinHttpOpen(L"EXVS-Overlay-updater/" OVERLAY_VERSION_W, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) { snprintf(err, errn, "WinHTTP not available (%lu)", GetLastError()); return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 10000, 20000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0), r = NULL;
    do {
        if (!c) { snprintf(err, errn, "can't reach %ls (%lu)", host, GetLastError()); break; }
        r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!r) { snprintf(err, errn, "request failed (%lu)", GetLastError()); break; }
        wchar_t hdr[128]; _snwprintf(hdr, 128, L"Accept: %ls", accept);
        WinHttpAddRequestHeaders(r, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) {
            snprintf(err, errn, "no answer from %ls (error %lu)", host, GetLastError()); break;
        }
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) { snprintf(err, errn, code == 404 ? "no release published yet (HTTP 404)" : code == 403 ? "GitHub limit reached, try later (HTTP 403)" : "HTTP %lu", code); break; }
        wchar_t len[32]; sz = sizeof(len);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &sz, WINHTTP_NO_HEADER_INDEX))
            InterlockedExchange64(&g_total, (LONG64)_wcstoui64(len, NULL, 10));
        static char buf[1 << 16];
        ok = true;
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(r, buf, sizeof(buf), &n)) { snprintf(err, errn, "download interrupted (%lu)", GetLastError()); ok = false; break; }
            if (!n) break;
            if (!sink(ctx, buf, n)) { snprintf(err, errn, "the answer is too big"); ok = false; break; }
        }
    } while (0);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// tiny JSON reading (only what the GitHub release answer needs)
// ---------------------------------------------------------------------------------------------
static const char *json_value(const char *from, const char *key) {
    char k[64]; snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(from, k);
    if (!p) return NULL;
    p += strlen(k);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
static bool json_string(const char *v, char *out, size_t n) {   // v points at the opening quote
    if (!v || *v != '"') return false;
    size_t o = 0;
    for (v++; *v && *v != '"'; v++) {
        char ch = *v;
        if (ch == '\\' && v[1]) {
            v++;
            switch (*v) {
                case 'n': ch = '\n'; break; case 'r': ch = '\r'; break; case 't': ch = ' '; break;
                case 'u': {   // \uXXXX -> UTF-8 (BMP only; surrogates become '?')
                    char hex[5] = {}; for (int i = 0; i < 4 && v[1]; i++) hex[i] = *++v;
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    char u[4]; int un = 0;
                    if (cp < 0x80) u[un++] = (char)cp;
                    else if (cp < 0x800) { u[un++] = (char)(0xC0 | (cp >> 6)); u[un++] = (char)(0x80 | (cp & 0x3F)); }
                    else if (cp >= 0xD800 && cp < 0xE000) u[un++] = '?';
                    else { u[un++] = (char)(0xE0 | (cp >> 12)); u[un++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[un++] = (char)(0x80 | (cp & 0x3F)); }
                    for (int i = 0; i < un; i++) if (o + 1 < n) out[o++] = u[i];
                    continue;
                }
                default: ch = *v;
            }
        }
        if (ch == '\r') continue;
        if (o + 1 < n) out[o++] = ch;
    }
    out[o] = 0;
    return true;
}

static void parse_version(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = -1;
    while (*s && (*s < '0' || *s > '9')) s++;
    for (int i = 0; i < 3 && *s >= '0' && *s <= '9'; i++) {
        v[i] = (int)strtol(s, (char **)&s, 10);
        if (*s == '.') s++; else break;
    }
    for (int i = 1; i < 3; i++) if (v[0] >= 0 && v[i] < 0) v[i] = 0;
}
static int cmp_version(const char *a, const char *b) {
    int x[3], y[3]; parse_version(a, x); parse_version(b, y);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------------------------
struct Mem { char *p; size_t n, cap; };
static bool mem_sink(void *ctx, const char *d, DWORD n) {
    Mem *m = (Mem *)ctx;
    if (m->n + n + 1 > m->cap) {
        if (m->cap >= (4u << 20)) return false;
        size_t cap = m->cap ? m->cap * 2 : 1 << 16;
        while (cap < m->n + n + 1) cap *= 2;
        char *q = (char *)realloc(m->p, cap); if (!q) return false;
        m->p = q; m->cap = cap;
    }
    memcpy(m->p + m->n, d, n); m->n += n; m->p[m->n] = 0;
    return true;
}

static void do_check(void) {   // network: runs in the helper
    InterlockedExchange(&g_state, UPD_CHECKING);
    Mem m = {};
    char err[200];
    if (!http_get("https://api.github.com/repos/" UPDATE_REPO "/releases/latest", L"application/vnd.github+json", mem_sink, &m, err, sizeof(err)) || !m.p) {
        fail("Couldn't check for updates: %s.", err);
    } else {
        char tag[48] = "";
        json_string(json_value(m.p, "tag_name"), tag, sizeof(tag));
        // the DLL asset: its size / digest / browser_download_url come after its "name" in GitHub's answer
        char key[64]; snprintf(key, sizeof(key), "\"" UPDATE_ASSET "\"");
        const char *assets = json_value(m.p, "assets");
        const char *a = assets ? strstr(assets, key) : NULL;
        if (!tag[0]) fail("Couldn't check for updates: unexpected answer from GitHub.");
        else if (cmp_version(tag, OVERLAY_VERSION) <= 0) {
            snprintf(g_ver, sizeof(g_ver), "%s", tag[0] == 'v' ? tag + 1 : tag);
            ov_log("update: up to date (latest release %s)", tag);
            InterlockedExchange(&g_state, UPD_LATEST);
        } else if (!a) {
            fail("Version %s has no %s to download.", tag, UPDATE_ASSET);
        } else {
            snprintf(g_tag, sizeof(g_tag), "%s", tag);
            snprintf(g_ver, sizeof(g_ver), "%s", tag[0] == 'v' ? tag + 1 : tag);
            const char *sz = json_value(a, "size");
            g_size = sz ? _strtoui64(sz, NULL, 10) : 0;
            char digest[96] = "";
            json_string(json_value(a, "digest"), digest, sizeof(digest));
            g_sha[0] = 0;
            if (strncmp(digest, "sha256:", 7) == 0) snprintf(g_sha, sizeof(g_sha), "%s", digest + 7);
            json_string(json_value(a, "browser_download_url"), g_url, sizeof(g_url));
            g_notes[0] = 0;
            json_string(json_value(m.p, "body"), g_notes, sizeof(g_notes));
            if (strlen(g_notes) >= sizeof(g_notes) - 2) strcpy(g_notes + sizeof(g_notes) - 6, "...");
            if (strncmp(g_url, "https://", 8) != 0) fail("Version %s: bad download address.", tag);
            else {
                ov_log("update: %s available (%llu bytes, sha256 %s)", tag, g_size, g_sha[0] ? g_sha : "none");
                InterlockedExchange(&g_state, UPD_AVAILABLE);
            }
        }
    }
    free(m.p);
}

// ---------------------------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------------------------
struct FileSink { HANDLE f; BCRYPT_HASH_HANDLE h; unsigned long long n; };
static bool file_sink(void *ctx, const char *d, DWORD n) {
    FileSink *s = (FileSink *)ctx;
    DWORD w = 0;
    if (!WriteFile(s->f, d, n, &w, NULL) || w != n) return false;
    if (s->h) BCryptHashData(s->h, (PUCHAR)d, n, 0);
    s->n += n;
    InterlockedExchange64(&g_done, (LONG64)s->n);
    static unsigned long long last;
    if (g_in_helper && (s->n - last >= (256u << 10) || s->n < last)) { last = s->n; ini_putu("done", s->n); }
    return s->n < ((unsigned long long)UPDATE_MAX_MB << 20);
}

static void self_paths(wchar_t *self, wchar_t *nw, wchar_t *old) {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&self_paths, &m);
    GetModuleFileNameW(m, self, MAX_PATH);
    wchar_t dir[MAX_PATH]; wcscpy(dir, self);
    wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0;
    _snwprintf(nw, MAX_PATH, L"%lsxinput9_1_0.new.dll", dir);
    _snwprintf(old, MAX_PATH, L"%lsxinput9_1_0.old.dll", dir);
}

static bool do_download(void) {   // network: runs in the helper; leaves the checked file at xinput9_1_0.new.dll
    wchar_t self[MAX_PATH], nw[MAX_PATH], old[MAX_PATH]; self_paths(self, nw, old);
    InterlockedExchange64(&g_done, 0); InterlockedExchange64(&g_total, (LONG64)g_size);
    InterlockedExchange(&g_state, UPD_DOWNLOADING);
    ov_log("update: downloading %s", g_url);

    FileSink fs = {};
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) BCryptCreateHash(alg, &fs.h, NULL, 0, NULL, 0, 0);
    fs.f = CreateFileW(nw, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char err[200] = "";
    bool ok = false;
    if (fs.f == INVALID_HANDLE_VALUE) fail("Couldn't write in the game folder (error %lu).", GetLastError());
    else if (!http_get(g_url, L"application/octet-stream", file_sink, &fs, err, sizeof(err))) fail("Download failed: %s.", err);
    else if (g_size && fs.n != g_size) fail("The download is incomplete (%llu of %llu bytes).", fs.n, g_size);
    else {
        unsigned char hash[32] = {}; char hex[65] = "";
        if (fs.h) { BCryptFinishHash(fs.h, hash, 32, 0); for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]); }
        char mz[2] = {}; DWORD rd = 0;
        SetFilePointer(fs.f, 0, NULL, FILE_BEGIN); ReadFile(fs.f, mz, 2, &rd, NULL);
        if (g_sha[0] && _stricmp(hex, g_sha) != 0) fail("The downloaded file is damaged (SHA-256 doesn't match).");
        else if (rd != 2 || mz[0] != 'M' || mz[1] != 'Z') fail("The downloaded file isn't a DLL.");
        else ok = true;
    }
    if (fs.f != INVALID_HANDLE_VALUE) CloseHandle(fs.f);
    if (fs.h) BCryptDestroyHash(fs.h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) DeleteFileW(nw);
    return ok;
}

static bool do_swap(void) {   // game process: the downloaded DLL takes the place of the loaded one
    wchar_t self[MAX_PATH], nw[MAX_PATH], old[MAX_PATH]; self_paths(self, nw, old);
    HANDLE f = CreateFileW(nw, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz = {}; char mz[2] = {}; DWORD rd = 0;
    if (f != INVALID_HANDLE_VALUE) { GetFileSizeEx(f, &sz); ReadFile(f, mz, 2, &rd, NULL); CloseHandle(f); }
    if (f == INVALID_HANDLE_VALUE || rd != 2 || mz[0] != 'M' || mz[1] != 'Z' || (g_size && (unsigned long long)sz.QuadPart != g_size)) {
        fail("The downloaded file is missing or incomplete.");
        DeleteFileW(nw);
        return false;
    }
    bool ok = true;
    {
        DeleteFileW(old);   // may still be loaded by the Live Monitor or the other client: then the rename fails
        if (!MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING)) {
            fail("Couldn't replace the overlay (error %lu). Close the game and try again, or update from EXVS Launcher.", GetLastError());
            ok = false;
        } else if (!MoveFileExW(nw, self, 0)) {
            DWORD e = GetLastError();
            MoveFileExW(old, self, 0);   // put the running one back: the overlay keeps working
            fail("Couldn't put the new overlay in place (error %lu).", e);
            ok = false;
        }
    }
    if (ok) {
        ov_log("update: %s installed, used from the next game start", g_tag);
        InterlockedExchange(&g_state, UPD_INSTALLED);
    } else {
        DeleteFileW(nw);
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// helper process: rundll32 "<this dll>",UpdateHelper check|download
// ---------------------------------------------------------------------------------------------
extern "C" __declspec(dllexport) void CALLBACK UpdateHelperW(HWND, HINSTANCE, LPWSTR args, int) {
    g_in_helper = true;
    if (args && wcsstr(args, L"download")) {
        load_result();
        ini_putu("done", 0); ini_put("result", "");
        bool ok = do_download();
        ini_put("error", g_err); ini_put("result", ok ? "ok" : "failed");
    } else {
        do_check();
        save_result();
        ini_put("result", "checked");
    }
}

// game side: starts the helper and waits for it (polling the progress of a download)
static bool run_helper(const wchar_t *mode, DWORD timeout_ms) {
    wchar_t self[MAX_PATH], nw[MAX_PATH], old[MAX_PATH], sys[MAX_PATH], line[MAX_PATH * 2 + 64];
    self_paths(self, nw, old); GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(line, sizeof(line) / sizeof(line[0]), L"\"%ls\\rundll32.exe\" \"%ls\",UpdateHelper %ls", sys, self, mode);
    ini_put("result", "");
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ov_log("update: helper failed to start (%lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD start = GetTickCount(), r;
    while ((r = WaitForSingleObject(pi.hProcess, 200)) == WAIT_TIMEOUT) {
        InterlockedExchange64(&g_done, (LONG64)ini_getu("done"));
        if (GetTickCount() - start > timeout_ms) { TerminateProcess(pi.hProcess, 1); ov_log("update: helper timed out"); break; }
    }
    CloseHandle(pi.hProcess);
    return r == WAIT_OBJECT_0;
}

static DWORD WINAPI check_thread(LPVOID delay) {
    if (delay) Sleep((DWORD)(UINT_PTR)delay);
    InterlockedExchange(&g_state, UPD_CHECKING);
    char res[16] = "";
    if (run_helper(L"check", 60000)) ini_get("result", res, sizeof(res));
    if (strcmp(res, "checked") == 0) {
        load_result();
        LONG st = (LONG)ini_getu("state");
        if (st == UPD_AVAILABLE) ov_log("update: %s available (%llu bytes)", g_tag, g_size);
        else if (st == UPD_FAILED) ov_log("update: %s", g_err);
        InterlockedExchange(&g_state, st);
    } else {
        do_check();   // no helper: try from here (works when the loader doesn't hook the sockets)
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static DWORD WINAPI install_thread(LPVOID) {
    InterlockedExchange64(&g_done, 0); InterlockedExchange64(&g_total, (LONG64)g_size);
    InterlockedExchange(&g_state, UPD_DOWNLOADING);
    save_result();   // the helper reads url / sha256 / size from the ini
    char res[16] = "";
    if (run_helper(L"download", 10 * 60000)) ini_get("result", res, sizeof(res));
    bool ok;
    if (strcmp(res, "ok") == 0) ok = true;
    else if (strcmp(res, "failed") == 0) { ini_get("error", g_err, sizeof(g_err)); ov_log("update: %s", g_err); InterlockedExchange(&g_state, UPD_FAILED); ok = false; }
    else ok = do_download();   // no helper: try from here
    if (ok) do_swap();
    InterlockedExchange(&g_busy, 0);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------------------------
static void spawn(LPTHREAD_START_ROUTINE fn, LPVOID arg) {
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_busy, 0);
}

bool upd_supported(void) {
    return !GetEnvironmentVariableA("EXVSOVERLAY_NO_UPDATE", NULL, 0) && !GetEnvironmentVariableA("EXVSOVERLAY_TEST", NULL, 0);
}

void upd_startup(bool auto_check) {
    wchar_t self[MAX_PATH], nw[MAX_PATH], old[MAX_PATH]; self_paths(self, nw, old);
    DeleteFileW(nw);   // a download that didn't finish
    if (auto_check && upd_supported()) spawn(check_thread, (LPVOID)(UINT_PTR)20000);
}

void upd_check_now(void) { spawn(check_thread, NULL); }

void upd_install(void) {
    if (g_state == UPD_AVAILABLE || (g_state == UPD_FAILED && g_url[0] && g_ver[0])) spawn(install_thread, NULL);
}

#ifdef UPDATE_TEST_EXPORTS   // test builds only: runs the game-side check + install synchronously
extern "C" __declspec(dllexport) void CALLBACK UpdTestRunW(HWND, HINSTANCE, LPWSTR, int) {
    InterlockedExchange(&g_busy, 1); check_thread(NULL);
    ov_log("TEST check -> state %ld version %s error %s", g_state, g_ver, g_err);
    if (g_state == UPD_AVAILABLE) { InterlockedExchange(&g_busy, 1); install_thread(NULL); ov_log("TEST install -> state %ld error %s done %lld", g_state, g_err, (long long)g_done); }
}
#endif

void upd_info(UpdInfo *u) {
    u->state = (int)g_state;
    u->version = g_ver; u->notes = g_notes; u->error = g_err;
    u->done = (unsigned long long)g_done; u->total = (unsigned long long)g_total;
    u->can_install = g_url[0] && cmp_version(g_ver, OVERLAY_VERSION) > 0;
    u->repo = UPDATE_REPO;
}
